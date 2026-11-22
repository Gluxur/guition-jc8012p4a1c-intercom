#include "esphome_api.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h" // xTaskCreatePinnedToCoreWithCaps
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "nvs.h"

#include "net/esphome_frame.h"
#include "net/esphome_pb.h"
#include "net/noise_nnpsk0.h"

namespace esphome_api {
namespace {

constexpr const char *TAG = "esphome_api";

constexpr uint16_t kPort = 6053;
constexpr int kMaxClients = 3; // HA + a CLI/dashboard + one spare

// What we tell clients we speak. 1.13 on purpose: from 1.14 clients may omit
// object_id handling we rely on, and from 1.15 they send a
// DeviceCapabilitiesRequest we do not implement (aioesphomeapi client.py).
constexpr uint32_t kApiMajor = 1;
constexpr uint32_t kApiMinor = 13;
// Reported as the "ESPHome version". 2025.10.0 is the first release with
// action responses, which HA must believe we support.
constexpr const char *kEsphomeVersion = "2025.10.0";
constexpr const char *kModel = "Guition JC8012P4A1C (ESP32-P4)";
constexpr const char *kProjectName = "gluxur.guition-jc8012p4a1c-intercom";
constexpr const char *kNoiseName = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";

constexpr size_t kMaxHandshakeFrame = 128; // same limit as ESPHome
constexpr size_t kRxMax = 32 * 1024;       // a month of calendar events fits
constexpr size_t kTxCap = 4096;            // biggest message we send
constexpr size_t kTxRoom = 16;             // header room before the payload
constexpr int64_t kIdleCloseUs = 150LL * 1000 * 1000; // HA pings every ~20 s
constexpr int64_t kRefreshUs = 30LL * 1000 * 1000;
constexpr int64_t kActionTimeoutUs = 20LL * 1000 * 1000;

// Message ids, from esphome/components/api/api.proto.
enum Msg : uint16_t {
  kHelloRequest = 1,
  kHelloResponse = 2,
  kDisconnectRequest = 5,
  kDisconnectResponse = 6,
  kPingRequest = 7,
  kPingResponse = 8,
  kDeviceInfoRequest = 9,
  kDeviceInfoResponse = 10,
  kListEntitiesRequest = 11,
  kListBinarySensor = 12,
  kListLight = 15,
  kListSensor = 16,
  kListSwitch = 17,
  kListTextSensor = 18,
  kListEntitiesDone = 19,
  kSubscribeStatesRequest = 20,
  kBinarySensorState = 21,
  kLightState = 24,
  kSensorState = 25,
  kSwitchState = 26,
  kTextSensorState = 27,
  kSubscribeLogsRequest = 28,
  kSubscribeLogsResponse = 29,
  kLightCommand = 32,
  kSwitchCommand = 33,
  kSubscribeHaServices = 34,
  kHaActionRequest = 35,
  kSubscribeHaStates = 38,
  kSubscribeHaStateResponse = 39,
  kHaStateResponse = 40,
  kListText = 97,
  kTextState = 98,
  kTextCommand = 99,
  kListNumber = 49,
  kNumberState = 50,
  kNumberCommand = 51,
  kListSelect = 52,
  kSelectState = 53,
  kSelectCommand = 54,
  kListButton = 61,
  kButtonCommand = 62,
  kListUpdate = 116,
  kUpdateState = 117,
  kUpdateCommand = 118,
  kNoiseSetKeyRequest = 124,
  kNoiseSetKeyResponse = 125,
  kHaActionResponse = 130,
};

enum class Mode : uint8_t { kUnknown, kPlain, kNoise };
enum class NoiseState : uint8_t { kClientHello, kHandshake, kData };

struct Client {
  int fd = -1;
  Mode mode = Mode::kUnknown;
  NoiseState ns = NoiseState::kClientHello;
  uint8_t *rx = nullptr;
  size_t rx_len = 0;
  size_t rx_cap = 0;
  noise::Responder *hs = nullptr;
  noise::CipherState send;
  noise::CipherState recv;
  bool hello = false;
  bool sub_states = false;
  bool sub_services = false;
  bool sub_logs = false;
  bool sub_ha_states = false; // HA sent 38: it accepts our 39 requests
  uint8_t log_level = 0; // LogLevel requested by the client
  bool closing = false; // close once the current batch is processed
  int64_t last_rx_us = 0;
  char peer[16] = {};
};

struct QueuedAction {
  uint8_t *msg; // encoded HomeassistantActionRequest, heap_caps_malloc'd
  size_t len;
  uint32_t call_id;
  ActionDone done;
  void *ctx;
};

struct Pending {
  uint32_t call_id = 0;
  ActionDone done = nullptr;
  void *ctx = nullptr;
  int64_t deadline_us = 0;
};

const Config *s_cfg = nullptr;
Client s_clients[kMaxClients];
Pending s_pending[4];
QueueHandle_t s_actions = nullptr;
std::atomic<uint64_t> s_dirty{0};
std::atomic<int> s_subscribed{0};
std::atomic<int> s_log_subscribers{0};

// Log lines waiting to be sent: filled by log_line() from any task, drained
// by the server task. A byte ring of [be16 len][bytes] records; when full the
// newest line is dropped (the reader is the slow side, it catches up).
constexpr size_t kLogRingBytes = 16 * 1024;
uint8_t *s_log_ring = nullptr;
size_t s_log_head = 0; // write offset
size_t s_log_tail = 0; // read offset
size_t s_log_used = 0;
portMUX_TYPE s_log_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<uint32_t> s_next_call_id{1};

// HA states requested by the app (watch_ha_state). Written from any task,
// read by the server task: copied under s_watch_mux. `pending` = not yet sent
// to the clients already subscribed.
struct Watch {
  char entity_id[128];
  bool pending;
};
Watch s_watches[kMaxHaWatches] = {};
portMUX_TYPE s_watch_mux = portMUX_INITIALIZER_UNLOCKED;
std::atomic<bool> s_watch_pending{false};

uint8_t s_psk[noise::kKeyLen] = {};
// Read by net_wdt and the boot health check (other tasks), written here.
std::atomic<bool> s_has_psk{false};

// Shared work buffers, PSRAM: the server task is their only user.
uint8_t *s_tx = nullptr;  // kTxRoom + kTxCap: header room + payload
uint8_t *s_enc = nullptr; // noise header + inner header + payload + MAC
uint8_t *s_dec = nullptr; // decrypted inbound frame

// --- Small helpers ---------------------------------------------------------

uint32_t key_of(const char *object_id) {
  // FNV-1a: stable across boots, so HA's state cache keeps matching.
  uint32_t h = 2166136261u;
  for (const char *p = object_id; *p; p++) {
    h ^= (uint8_t)*p;
    h *= 16777619u;
  }
  return h == 0 ? 1 : h;
}

void read_mac(uint8_t mac[6]) {
  memset(mac, 0, 6);
  esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (n != nullptr) {
    esp_netif_get_mac(n, mac);
  }
}

bool is_zero_key(const uint8_t *k) {
  uint8_t acc = 0;
  for (size_t i = 0; i < noise::kKeyLen; i++) {
    acc |= k[i];
  }
  return acc == 0;
}

// --- PSK persistence ---------------------------------------------------------

constexpr const char *kNvsNs = "esphome_api";
constexpr const char *kNvsKey = "psk";

void load_psk() {
  nvs_handle_t h;
  if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) {
    return; // never provisioned: namespace absent
  }
  size_t n = sizeof s_psk;
  if (nvs_get_blob(h, kNvsKey, s_psk, &n) == ESP_OK && n == sizeof s_psk &&
      !is_zero_key(s_psk)) {
    s_has_psk = true;
  } else {
    memset(s_psk, 0, sizeof s_psk);
  }
  nvs_close(h);
}

bool save_psk(const uint8_t *key, bool clear) {
  nvs_handle_t h;
  if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) {
    return false;
  }
  esp_err_t err = clear ? nvs_erase_key(h, kNvsKey)
                        : nvs_set_blob(h, kNvsKey, key, noise::kKeyLen);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    err = ESP_OK; // clearing a key that was never there
  }
  if (err == ESP_OK) {
    err = nvs_commit(h);
  }
  nvs_close(h);
  return err == ESP_OK;
}

// --- mDNS --------------------------------------------------------------------

void mdns_publish_txt() {
  uint8_t mac[6];
  read_mac(mac);
  static char mac_hex[13];
  snprintf(mac_hex, sizeof mac_hex, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  mdns_txt_item_t txt[] = {
      {"friendly_name", s_cfg->friendly_name},
      {"version", kEsphomeVersion},
      {"mac", mac_hex},
      {"platform", "ESP32"},
      {"board", "esp32-p4"},
      {"network", "wifi"},
      {s_has_psk ? "api_encryption" : "api_encryption_supported", kNoiseName},
      {"project_name", kProjectName},
      {"project_version", s_cfg->project_version},
      // Unprovisioned: HA may push a key over a zero-PSK Noise session.
      {"api_provisioning", "zero-psk"},
  };
  const size_t n = sizeof(txt) / sizeof(txt[0]) - (s_has_psk ? 1 : 0);
  const esp_err_t err =
      mdns_service_txt_set("_esphomelib", "_tcp", txt, (uint8_t)n);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mdns txt: %s", esp_err_to_name(err));
  }
}

bool mdns_start() {
  esp_err_t err = mdns_init();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_init: %s", esp_err_to_name(err));
    return false;
  }
  mdns_hostname_set(s_cfg->name);
  mdns_instance_name_set(s_cfg->friendly_name);
  err = mdns_service_add(s_cfg->name, "_esphomelib", "_tcp", kPort, nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "mdns_service_add: %s", esp_err_to_name(err));
    return false;
  }
  mdns_publish_txt();
  return true;
}

// --- Sending -------------------------------------------------------------------

bool send_all(Client &c, const uint8_t *p, size_t n) {
  while (n > 0) {
    const int w = send(c.fd, p, n, 0);
    if (w <= 0) {
      ESP_LOGW(TAG, "%s: send failed (errno %d)", c.peer, errno);
      c.closing = true;
      return false;
    }
    p += w;
    n -= (size_t)w;
  }
  return true;
}

// Raw Noise frame (handshake phase).
bool send_noise_frame(Client &c, const uint8_t *data, size_t len) {
  uint8_t hdr[esphome_frame::kNoiseHeaderLen];
  esphome_frame::write_noise_header(hdr, (uint16_t)len);
  return send_all(c, hdr, sizeof hdr) && send_all(c, data, len);
}

void send_reject(Client &c, const char *reason) {
  uint8_t buf[40];
  const size_t rl = strnlen(reason, sizeof buf - 1);
  buf[0] = 0x01; // HANDSHAKE_STATUS_REJECT
  memcpy(buf + 1, reason, rl);
  send_noise_frame(c, buf, 1 + rl);
  c.closing = true;
}

// Sends the payload currently in s_tx[kTxRoom .. kTxRoom + len). The payload
// bytes are not modified, so the same encoding can go to several clients.
bool send_msg(Client &c, uint16_t type, size_t len) {
  if (c.closing || c.fd < 0) {
    return false;
  }
  uint8_t *payload = s_tx + kTxRoom;
  if (c.mode == Mode::kPlain) {
    uint8_t hdr[esphome_frame::kPlainHeaderMax];
    const size_t hl =
        esphome_frame::write_plain_header(hdr, sizeof hdr, (uint32_t)len, type);
    memcpy(payload - hl, hdr, hl);
    return send_all(c, payload - hl, hl + len);
  }
  // Noise: encrypt [type][len][payload] into s_enc after the frame header.
  uint8_t *inner = payload - esphome_frame::kInnerHeaderLen;
  esphome_frame::put_be16(inner, type);
  esphome_frame::put_be16(inner + 2, (uint16_t)len);
  const size_t plain = esphome_frame::kInnerHeaderLen + len;
  if (!c.send.encrypt(nullptr, 0, inner, plain,
                      s_enc + esphome_frame::kNoiseHeaderLen)) {
    ESP_LOGE(TAG, "%s: encrypt failed", c.peer);
    c.closing = true;
    return false;
  }
  const size_t frame = plain + noise::kTagLen;
  esphome_frame::write_noise_header(s_enc, (uint16_t)frame);
  return send_all(c, s_enc, esphome_frame::kNoiseHeaderLen + frame);
}

esphome_pb::Writer writer() {
  return esphome_pb::Writer(s_tx + kTxRoom, kTxCap);
}

bool send_written(Client &c, uint16_t type, const esphome_pb::Writer &w) {
  if (!w.ok()) {
    ESP_LOGE(TAG, "message %u does not fit %u bytes", type, (unsigned)kTxCap);
    return false;
  }
  return send_msg(c, type, w.size());
}

// --- Entities ------------------------------------------------------------------

uint16_t encode_list_entity(const Entity &e, esphome_pb::Writer &w) {
  const uint32_t key = key_of(e.object_id);
  const uint32_t cat = (uint32_t)e.category;
  w.str(1, e.object_id);
  w.fixed32(2, key);
  w.str(3, e.name);
  switch (e.kind) {
  case Kind::kBinarySensor:
    w.str(5, e.device_class);
    w.str(8, e.icon);
    w.u32(9, cat);
    return kListBinarySensor;
  case Kind::kSensor:
    w.str(5, e.icon);
    w.str(6, e.unit);
    w.i32(7, e.accuracy);
    w.str(9, e.device_class);
    w.u32(10, 1); // STATE_CLASS_MEASUREMENT: HA keeps long-term statistics
    w.u32(13, cat);
    return kListSensor;
  case Kind::kTextSensor:
    w.str(5, e.icon);
    w.u32(7, cat);
    w.str(8, e.device_class);
    return kListTextSensor;
  case Kind::kSwitch:
    w.str(5, e.icon);
    w.u32(8, cat);
    w.str(9, e.device_class);
    return kListSwitch;
  case Kind::kNumber:
    w.str(5, e.icon);
    w.f32(6, e.min);
    w.f32(7, e.max);
    w.f32(8, e.step);
    w.u32(10, cat);
    w.str(11, e.unit);
    w.u32(12, 2); // NUMBER_MODE_SLIDER
    w.str(13, e.device_class);
    return kListNumber;
  case Kind::kSelect:
    w.str(5, e.icon);
    for (uint8_t i = 0; i < e.option_count; i++) {
      w.bytes_always(6, e.options[i], strlen(e.options[i]));
    }
    w.u32(8, cat);
    return kListSelect;
  case Kind::kButton:
    w.str(5, e.icon);
    w.u32(7, cat);
    w.str(8, e.device_class);
    return kListButton;
  case Kind::kLight:
    w.boolean(5, true); // legacy_supports_brightness (pre-color-mode clients)
    w.u32(12, 3);       // supported_color_modes: COLOR_MODE_BRIGHTNESS
    w.str(14, e.icon);
    w.u32(15, cat);
    return kListLight;
  case Kind::kUpdate:
    w.str(5, e.icon);
    w.u32(7, cat);
    w.str(8, e.device_class);
    return kListUpdate;
  case Kind::kText:
    w.str(5, e.icon);
    w.u32(7, cat);
    w.u32(9, e.max_len);
    w.str(10, e.pattern);
    // mode 11 = TEXT_MODE_TEXT = 0: omitted (proto3 default)
    return kListText;
  }
  return 0;
}

// Encodes the state of `e` into s_tx. Returns the message type, 0 if the
// entity has no state.
uint16_t encode_state(const Entity &e, esphome_pb::Writer &w) {
  w.fixed32(1, key_of(e.object_id));
  switch (e.kind) {
  case Kind::kBinarySensor:
  case Kind::kSwitch:
    if (e.get_bool == nullptr) {
      return 0;
    }
    w.boolean(2, e.get_bool());
    return e.kind == Kind::kSwitch ? kSwitchState : kBinarySensorState;
  case Kind::kSensor:
  case Kind::kNumber: {
    if (e.get_float == nullptr) {
      return 0;
    }
    const float v = e.get_float();
    if (std::isnan(v)) {
      w.boolean(3, true); // missing_state
    } else {
      w.f32(2, v);
    }
    return e.kind == Kind::kSensor ? kSensorState : kNumberState;
  }
  case Kind::kTextSensor:
  case Kind::kSelect:
  case Kind::kText: {
    if (e.get_text == nullptr) {
      return 0;
    }
    char buf[128];
    buf[0] = '\0';
    e.get_text(buf, sizeof buf);
    if (e.kind == Kind::kText) {
      // An empty text is a real state ("not configured"), not a missing one.
      w.bytes_always(2, buf, strlen(buf));
      return kTextState;
    }
    w.str(2, buf);
    return e.kind == Kind::kSelect ? kSelectState : kTextSensorState;
  }
  case Kind::kLight: {
    if (e.get_bool == nullptr) {
      return 0;
    }
    w.boolean(2, e.get_bool());
    if (e.get_float != nullptr) {
      w.f32(3, e.get_float());
    }
    w.u32(11, 3); // COLOR_MODE_BRIGHTNESS
    return kLightState;
  }
  case Kind::kButton:
    return 0;
  case Kind::kUpdate: {
    if (e.get_update == nullptr) {
      return 0;
    }
    // Static: 250 B is a big bite of the 12 KB server stack, and only the
    // server task ever encodes.
    static UpdateState u;
    u = UpdateState{};
    e.get_update(&u);
    if (u.current[0] == '\0') {
      w.boolean(2, true); // missing_state
      return kUpdateState;
    }
    w.boolean(3, u.in_progress);
    if (u.in_progress && u.progress >= 0) {
      w.boolean(4, true); // has_progress
      w.f32(5, u.progress);
    }
    w.str(6, u.current);
    w.str(7, u.latest[0] != '\0' ? u.latest : u.current);
    w.str(8, u.title);
    w.str(9, u.summary);
    return kUpdateState;
  }
  }
  return 0;
}

void push_state(size_t idx) {
  if (s_subscribed.load() == 0) {
    return;
  }
  const Entity &e = s_cfg->entities[idx];
  const bool busy = s_cfg->busy != nullptr && s_cfg->busy();
  if (busy && e.sd_touching) {
    return; // SD I/O during a call adds jitter to the MJPEG drain
  }
  esphome_pb::Writer w = writer();
  const uint16_t type = encode_state(e, w);
  if (type == 0 || !w.ok()) {
    return;
  }
  for (Client &c : s_clients) {
    if (c.fd >= 0 && c.sub_states) {
      send_msg(c, type, w.size());
    }
  }
}

const Entity *entity_by_key(uint32_t key, size_t *idx) {
  for (size_t i = 0; i < s_cfg->entity_count; i++) {
    if (key_of(s_cfg->entities[i].object_id) == key) {
      *idx = i;
      return &s_cfg->entities[i];
    }
  }
  return nullptr;
}

// --- Message handlers ----------------------------------------------------------

void on_hello(Client &c, esphome_pb::Reader &r) {
  char client_info[48] = {};
  uint32_t major = 0, minor = 0;
  while (r.next()) {
    switch (r.field()) {
    case 1: {
      size_t n;
      const uint8_t *p = r.as_bytes(&n);
      if (p != nullptr) {
        snprintf(client_info, sizeof client_info, "%.*s", (int)n, (const char *)p);
      }
      break;
    }
    case 2:
      major = r.as_u32();
      break;
    case 3:
      minor = r.as_u32();
      break;
    default:
      break;
    }
  }
  ESP_LOGI(TAG, "%s: hello from '%s' (API %u.%u, %s)", c.peer, client_info,
           (unsigned)major, (unsigned)minor,
           c.mode == Mode::kNoise ? "encrypted" : "plaintext");
  esphome_pb::Writer w = writer();
  w.u32(1, kApiMajor);
  w.u32(2, kApiMinor);
  char info[48];
  snprintf(info, sizeof info, "%s (%s)", kEsphomeVersion, s_cfg->project_version);
  w.str(3, info);
  w.str(4, s_cfg->name);
  c.hello = send_written(c, kHelloResponse, w);
}

void on_device_info(Client &c) {
  uint8_t mac[6];
  read_mac(mac);
  char mac_str[18];
  snprintf(mac_str, sizeof mac_str, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  const esp_app_desc_t *d = esp_app_get_description();
  char built[32];
  snprintf(built, sizeof built, "%s %s", d->date, d->time);

  esphome_pb::Writer w = writer();
  w.str(2, s_cfg->name);
  w.str(3, mac_str);
  w.str(4, kEsphomeVersion);
  w.str(5, built);
  w.str(6, kModel);
  w.str(8, kProjectName);
  w.str(9, s_cfg->project_version);
  w.str(12, "Espressif");
  w.str(13, s_cfg->friendly_name);
  w.boolean(19, true); // api_encryption_supported: HA may provision a key
  send_written(c, kDeviceInfoResponse, w);
}

void on_list_entities(Client &c) {
  for (size_t i = 0; i < s_cfg->entity_count && !c.closing; i++) {
    esphome_pb::Writer w = writer();
    const uint16_t type = encode_list_entity(s_cfg->entities[i], w);
    send_written(c, type, w);
  }
  esphome_pb::Writer w = writer();
  send_written(c, kListEntitiesDone, w);
}

void on_subscribe_states(Client &c) {
  if (!c.sub_states) {
    c.sub_states = true;
    s_subscribed.fetch_add(1);
  }
  for (size_t i = 0; i < s_cfg->entity_count && !c.closing; i++) {
    const Entity &e = s_cfg->entities[i];
    if (e.sd_touching && s_cfg->busy != nullptr && s_cfg->busy()) {
      continue;
    }
    esphome_pb::Writer w = writer();
    const uint16_t type = encode_state(e, w);
    if (type != 0) {
      send_written(c, type, w);
    }
  }
}

void on_command(uint16_t type, esphome_pb::Reader &r) {
  uint32_t key = 0;
  bool b = false;
  float f = NAN;
  char text[128] = {};
  bool has_state = false, has_bri = false;
  bool state_v = false;
  float bri_v = 0.0f; // proto3: an absent float is 0
  uint32_t update_cmd = 0; // UPDATE_COMMAND_NONE
  while (r.next()) {
    if (r.field() == 1) {
      key = r.as_fixed32();
    } else if (type == kLightCommand) {
      switch (r.field()) {
      case 2:
        has_state = r.as_bool();
        break;
      case 3:
        state_v = r.as_bool();
        break;
      case 4:
        has_bri = r.as_bool();
        break;
      case 5:
        bri_v = r.as_f32();
        break;
      default:
        break;
      }
    } else if (r.field() == 2) {
      if (type == kSwitchCommand) {
        b = r.as_bool();
      } else if (type == kNumberCommand) {
        f = r.as_f32();
      } else if (type == kUpdateCommand) {
        update_cmd = r.as_u32();
      } else if (type == kSelectCommand || type == kTextCommand) {
        size_t n;
        const uint8_t *p = r.as_bytes(&n);
        if (p != nullptr) {
          snprintf(text, sizeof text, "%.*s", (int)n, (const char *)p);
        }
      }
    }
  }
  // proto3 drops false/0: has_* tells "off" or "brightness 0" from "absent".
  const int light_state = has_state ? (state_v ? 1 : 0) : -1;
  const float light_bri = has_bri ? bri_v : NAN;
  size_t idx;
  const Entity *e = entity_by_key(key, &idx);
  if (e == nullptr) {
    ESP_LOGW(TAG, "command for unknown key 0x%08lx", (unsigned long)key);
    return;
  }
  switch (type) {
  case kSwitchCommand:
    if (e->set_bool) {
      e->set_bool(b);
    }
    break;
  case kNumberCommand:
    if (e->set_float && !std::isnan(f)) {
      e->set_float(f);
    }
    break;
  case kSelectCommand:
  case kTextCommand:
    if (e->set_text) {
      e->set_text(text);
    }
    break;
  case kButtonCommand:
    if (e->press) {
      e->press();
    }
    break;
  case kLightCommand:
    if (e->set_light) {
      e->set_light(light_state, light_bri);
    }
    break;
  case kUpdateCommand:
    // UPDATE_COMMAND_UPDATE = 1, UPDATE_COMMAND_CHECK = 2.
    if (e->update_cmd && (update_cmd == 1 || update_cmd == 2)) {
      e->update_cmd(update_cmd == 1);
    }
    break;
  default:
    break;
  }
  // Echo the real state at once: HA's controls are not optimistic.
  push_state(idx);
}

void on_set_key(Client &c, esphome_pb::Reader &r) {
  const uint8_t *p = nullptr;
  size_t n = 0;
  while (r.next()) {
    if (r.field() == 1) {
      p = r.as_bytes(&n);
    }
  }
  bool ok = false;
  uint8_t key[noise::kKeyLen];
  if (n == 0) {
    ok = save_psk(nullptr, true);
    if (ok) {
      memset(s_psk, 0, sizeof s_psk);
      s_has_psk = false;
      ESP_LOGW(TAG, "encryption key cleared by %s", c.peer);
    }
  } else if (esphome_frame::decode_base64((const char *)p, n, key, sizeof key) !=
             (int)noise::kKeyLen) {
    ESP_LOGW(TAG, "set key: not a base64 32-byte key");
  } else if (is_zero_key(key)) {
    ESP_LOGW(TAG, "set key: all-zero key refused (reserved for provisioning)");
  } else if (!save_psk(key, false)) {
    ESP_LOGE(TAG, "set key: NVS write failed");
  } else {
    memcpy(s_psk, key, sizeof s_psk);
    s_has_psk = true;
    ok = true;
    ESP_LOGI(TAG, "encryption key provisioned by %s", c.peer);
  }
  noise::secure_zero(key, sizeof key);

  esphome_pb::Writer w = writer();
  w.boolean(1, ok);
  send_written(c, kNoiseSetKeyResponse, w);
  if (ok) {
    mdns_publish_txt();
    // Sessions opened under the old key (or none) must not outlive it: HA
    // reconnects with the new one.
    for (Client &other : s_clients) {
      if (other.fd >= 0) {
        other.closing = true;
      }
    }
  }
}

// One 39 per non-empty slot, to one client.
void send_all_watches(Client &c) {
  for (size_t i = 0; i < kMaxHaWatches && !c.closing; i++) {
    char id[sizeof(Watch::entity_id)];
    taskENTER_CRITICAL(&s_watch_mux);
    memcpy(id, s_watches[i].entity_id, sizeof id);
    taskEXIT_CRITICAL(&s_watch_mux);
    if (id[0] == '\0') {
      continue;
    }
    esphome_pb::Writer w = writer();
    w.str(1, id);
    send_written(c, kSubscribeHaStateResponse, w);
  }
}

// Slots changed since the last pass: send them to the clients already
// subscribed (the others get every slot on their 38).
void flush_watches() {
  if (!s_watch_pending.exchange(false)) {
    return;
  }
  for (size_t i = 0; i < kMaxHaWatches; i++) {
    char id[sizeof(Watch::entity_id)];
    bool pending;
    taskENTER_CRITICAL(&s_watch_mux);
    pending = s_watches[i].pending;
    s_watches[i].pending = false;
    memcpy(id, s_watches[i].entity_id, sizeof id);
    taskEXIT_CRITICAL(&s_watch_mux);
    if (!pending || id[0] == '\0') {
      continue;
    }
    for (Client &c : s_clients) {
      if (c.fd >= 0 && c.sub_ha_states && !c.closing) {
        esphome_pb::Writer w = writer();
        w.str(1, id);
        send_written(c, kSubscribeHaStateResponse, w);
      }
    }
  }
}

void on_ha_state_response(esphome_pb::Reader &r) {
  char id[128] = {}, state[128] = {}, attr[64] = {};
  while (r.next()) {
    size_t n = 0;
    const uint8_t *p = nullptr;
    switch (r.field()) {
    case 1:
      p = r.as_bytes(&n);
      if (p != nullptr) {
        snprintf(id, sizeof id, "%.*s", (int)n, (const char *)p);
      }
      break;
    case 2:
      p = r.as_bytes(&n);
      if (p != nullptr) {
        snprintf(state, sizeof state, "%.*s", (int)n, (const char *)p);
      }
      break;
    case 3:
      p = r.as_bytes(&n);
      if (p != nullptr) {
        snprintf(attr, sizeof attr, "%.*s", (int)n, (const char *)p);
      }
      break;
    default:
      break;
    }
  }
  if (id[0] != '\0' && s_cfg->on_ha_state != nullptr) {
    s_cfg->on_ha_state(id, attr, state);
  }
}

void on_action_response(esphome_pb::Reader &r) {
  uint32_t call_id = 0;
  bool success = false;
  char error[96] = {};
  const uint8_t *data = nullptr;
  size_t data_len = 0;
  while (r.next()) {
    switch (r.field()) {
    case 1:
      call_id = r.as_u32();
      break;
    case 2:
      success = r.as_bool();
      break;
    case 3: {
      size_t n;
      const uint8_t *p = r.as_bytes(&n);
      if (p != nullptr) {
        snprintf(error, sizeof error, "%.*s", (int)n, (const char *)p);
      }
      break;
    }
    case 4:
      data = r.as_bytes(&data_len);
      break;
    default:
      break;
    }
  }
  for (Pending &pd : s_pending) {
    if (pd.call_id == call_id && pd.done != nullptr) {
      const ActionDone done = pd.done;
      void *ctx = pd.ctx;
      pd = Pending{};
      done(ctx, success, error, (const char *)data, data_len);
      return;
    }
  }
  ESP_LOGW(TAG, "action response for unknown call %lu", (unsigned long)call_id);
}

void dispatch(Client &c, uint16_t type, const uint8_t *data, size_t len) {
  esphome_pb::Reader r(data, len);
  if (!c.hello && type != kHelloRequest && type != kPingRequest &&
      type != kDisconnectRequest) {
    ESP_LOGW(TAG, "%s: message %u before hello", c.peer, type);
    return;
  }
  switch (type) {
  case kHelloRequest:
    on_hello(c, r);
    break;
  case kDisconnectRequest: {
    esphome_pb::Writer w = writer();
    send_written(c, kDisconnectResponse, w);
    c.closing = true;
    break;
  }
  case kDisconnectResponse:
    c.closing = true;
    break;
  case kPingRequest: {
    esphome_pb::Writer w = writer();
    send_written(c, kPingResponse, w);
    break;
  }
  case kPingResponse:
    break;
  case kDeviceInfoRequest:
    on_device_info(c);
    break;
  case kListEntitiesRequest:
    on_list_entities(c);
    break;
  case kSubscribeStatesRequest:
    on_subscribe_states(c);
    break;
  case kSubscribeHaServices:
    if (!c.sub_services) {
      c.sub_services = true;
      if (s_cfg->on_ha_ready != nullptr) {
        s_cfg->on_ha_ready();
      }
    }
    break;
  case kSubscribeHaStates:
    c.sub_ha_states = true;
    send_all_watches(c);
    break;
  case kHaStateResponse:
    on_ha_state_response(r);
    break;
  case kSwitchCommand:
  case kNumberCommand:
  case kSelectCommand:
  case kButtonCommand:
  case kLightCommand:
  case kUpdateCommand:
  case kTextCommand:
    on_command(type, r);
    break;
  case kSubscribeLogsRequest:
    c.log_level = 3; // LOG_LEVEL_INFO when the field is absent
    while (r.next()) {
      if (r.field() == 1) {
        c.log_level = (uint8_t)r.as_u32();
      }
    }
    if (!c.sub_logs) {
      c.sub_logs = true;
      s_log_subscribers.fetch_add(1);
    }
    break;
  case kNoiseSetKeyRequest:
    on_set_key(c, r);
    break;
  case kHaActionResponse:
    on_action_response(r);
    break;
  default:
    ESP_LOGD(TAG, "%s: ignoring message %u", c.peer, type);
    break;
  }
  if (!r.ok()) {
    ESP_LOGW(TAG, "%s: malformed message %u", c.peer, type);
  }
}

// --- Inbound frames --------------------------------------------------------------

// Noise handshake frame (ClientHello or message 1).
void on_handshake_frame(Client &c, const uint8_t *p, size_t len) {
  if (c.ns == NoiseState::kClientHello) {
    // Prologue binds the hello into the handshake: "NoiseAPIInit" + be16
    // length + the ClientHello payload (empty today).
    uint8_t prologue[12 + 2 + kMaxHandshakeFrame];
    memcpy(prologue, "NoiseAPIInit", 12);
    esphome_frame::put_be16(prologue + 12, (uint16_t)len);
    memcpy(prologue + 14, p, len);

    uint8_t hello[48];
    uint8_t mac[6];
    read_mac(mac);
    const size_t hl =
        esphome_frame::write_server_hello(hello, sizeof hello, s_cfg->name, mac);
    if (hl == 0 || !send_noise_frame(c, hello, hl)) {
      c.closing = true;
      return;
    }
    c.hs = new (std::nothrow) noise::Responder();
    // Unprovisioned: the all-zero PSK, publicly known. It authenticates
    // nobody but still runs a fresh X25519 exchange, so the key HA pushes
    // next cannot be read by a passive listener.
    if (c.hs == nullptr || !c.hs->init(s_psk, prologue, 14 + len)) {
      ESP_LOGE(TAG, "%s: noise init failed", c.peer);
      send_reject(c, "Handshake error");
      return;
    }
    c.ns = NoiseState::kHandshake;
    return;
  }

  // Message 1, prefixed by a status byte.
  if (len == 0 || p[0] != 0x00) {
    send_reject(c, len == 0 ? "Empty handshake message" : "Bad handshake error byte");
    return;
  }
  uint8_t payload[kMaxHandshakeFrame];
  size_t pl = 0;
  if (!c.hs->read_message1(p + 1, len - 1, payload, &pl)) {
    // Wire contract: clients map this exact text to "wrong encryption key".
    ESP_LOGW(TAG, "%s: handshake MAC failure (wrong key?)", c.peer);
    send_reject(c, "Handshake MAC failure");
    return;
  }
  uint8_t out[1 + noise::kDhLen + noise::kTagLen];
  size_t ol = 0;
  out[0] = 0x00;
  if (!c.hs->write_message2(nullptr, 0, out + 1, sizeof out - 1, &ol) ||
      !send_noise_frame(c, out, 1 + ol) || !c.hs->split(c.send, c.recv)) {
    ESP_LOGE(TAG, "%s: handshake failed", c.peer);
    c.closing = true;
    return;
  }
  delete c.hs;
  c.hs = nullptr;
  c.ns = NoiseState::kData;
}

void on_noise_data_frame(Client &c, const uint8_t *p, size_t len) {
  if (len < esphome_frame::kInnerHeaderLen + noise::kTagLen) {
    c.closing = true;
    return;
  }
  if (!c.recv.decrypt(nullptr, 0, p, len, s_dec)) {
    ESP_LOGW(TAG, "%s: decrypt failed", c.peer);
    c.closing = true;
    return;
  }
  const size_t plain = len - noise::kTagLen;
  const uint16_t type = esphome_frame::get_be16(s_dec);
  const uint16_t dlen = esphome_frame::get_be16(s_dec + 2);
  if (dlen > plain - esphome_frame::kInnerHeaderLen) {
    c.closing = true;
    return;
  }
  dispatch(c, type, s_dec + esphome_frame::kInnerHeaderLen, dlen);
}

bool ensure_rx(Client &c, size_t need) {
  if (need <= c.rx_cap) {
    return true;
  }
  if (need > kRxMax + 16) {
    return false;
  }
  size_t cap = c.rx_cap == 0 ? 512 : c.rx_cap;
  while (cap < need) {
    cap *= 2;
  }
  auto *n = static_cast<uint8_t *>(
      heap_caps_realloc(c.rx, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (n == nullptr) {
    return false;
  }
  c.rx = n;
  c.rx_cap = cap;
  return true;
}

// Consumes every complete frame in c.rx.
void process_rx(Client &c) {
  size_t off = 0;
  while (!c.closing && off < c.rx_len) {
    const uint8_t *p = c.rx + off;
    const size_t avail = c.rx_len - off;
    if (c.mode == Mode::kUnknown) {
      if (p[0] == esphome_frame::kPlainIndicator) {
        if (s_has_psk) {
          // ESPHome answers a plaintext hello on a keyed device with a Noise
          // reject: aioesphomeapi turns it into "requires encryption".
          send_reject(c, "Bad indicator byte");
          break;
        }
        c.mode = Mode::kPlain;
      } else if (p[0] == esphome_frame::kNoiseIndicator) {
        c.mode = Mode::kNoise;
      } else {
        c.closing = true;
        break;
      }
    }

    if (c.mode == Mode::kPlain) {
      size_t hl = 0;
      uint32_t plen = 0, type = 0;
      const auto st = esphome_frame::parse_plain_header(p, avail, &hl, &plen, &type);
      if (st == esphome_frame::Parse::kNeedMore) {
        break; // header itself incomplete (at most 11 bytes: rx holds it)
      }
      if (st == esphome_frame::Parse::kBad || plen > kRxMax) {
        c.closing = true;
        break;
      }
      if (avail < hl + plen) {
        if (!ensure_rx(c, hl + plen)) {
          c.closing = true;
        }
        break;
      }
      dispatch(c, (uint16_t)type, p + hl, plen);
      off += hl + plen;
      continue;
    }

    uint16_t flen;
    const auto st = esphome_frame::parse_noise_header(p, avail, &flen);
    if (st == esphome_frame::Parse::kBad) {
      send_reject(c, "Bad indicator byte");
      break;
    }
    if (st == esphome_frame::Parse::kNeedMore) {
      break;
    }
    if (c.ns != NoiseState::kData && flen > kMaxHandshakeFrame) {
      send_reject(c, "Bad handshake packet len");
      break;
    }
    const size_t total = esphome_frame::kNoiseHeaderLen + flen;
    if (avail < total) {
      if (!ensure_rx(c, total)) {
        c.closing = true;
      }
      break;
    }
    const uint8_t *body = p + esphome_frame::kNoiseHeaderLen;
    if (c.ns == NoiseState::kData) {
      on_noise_data_frame(c, body, flen);
    } else {
      on_handshake_frame(c, body, flen);
    }
    off += total;
  }
  if (off > 0) {
    memmove(c.rx, c.rx + off, c.rx_len - off);
    c.rx_len -= off;
  }
}

// --- Connections -----------------------------------------------------------------

void close_client(Client &c) {
  if (c.fd >= 0) {
    ESP_LOGI(TAG, "%s: closed", c.peer);
    close(c.fd);
  }
  if (c.sub_states) {
    s_subscribed.fetch_sub(1);
  }
  if (c.sub_logs) {
    s_log_subscribers.fetch_sub(1);
  }
  delete c.hs;
  heap_caps_free(c.rx);
  c.send.clear();
  c.recv.clear();
  c.fd = -1;
  c.mode = Mode::kUnknown;
  c.ns = NoiseState::kClientHello;
  c.rx = nullptr;
  c.rx_len = c.rx_cap = 0;
  c.hs = nullptr;
  c.hello = c.sub_states = c.sub_services = c.sub_logs = c.closing = false;
  c.sub_ha_states = false;
  c.log_level = 0;
}

void accept_client(int listen_fd) {
  sockaddr_in addr = {};
  socklen_t alen = sizeof addr;
  const int fd = accept(listen_fd, reinterpret_cast<sockaddr *>(&addr), &alen);
  if (fd < 0) {
    return;
  }
  Client *slot = nullptr;
  for (Client &c : s_clients) {
    if (c.fd < 0) {
      slot = &c;
      break;
    }
  }
  if (slot == nullptr) {
    ESP_LOGW(TAG, "connection refused: %d clients already", kMaxClients);
    close(fd);
    return;
  }
  const int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  // Bounded send: a stalled peer must not freeze the server task.
  const timeval tv = {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  slot->fd = fd;
  slot->last_rx_us = esp_timer_get_time();
  inet_ntoa_r(addr.sin_addr, slot->peer, sizeof slot->peer);
  if (!ensure_rx(*slot, 512)) {
    close_client(*slot);
    return;
  }
  ESP_LOGI(TAG, "%s: connected", slot->peer);
}

void read_client(Client &c) {
  if (c.rx_len == c.rx_cap && !ensure_rx(c, c.rx_cap * 2)) {
    c.closing = true;
    return;
  }
  const int n = recv(c.fd, c.rx + c.rx_len, c.rx_cap - c.rx_len, MSG_DONTWAIT);
  if (n <= 0) {
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    c.closing = true;
    return;
  }
  c.rx_len += (size_t)n;
  c.last_rx_us = esp_timer_get_time();
  process_rx(c);
}

// --- Actions ---------------------------------------------------------------------

void drain_actions() {
  QueuedAction qa;
  while (xQueueReceive(s_actions, &qa, 0) == pdTRUE) {
    Client *target = nullptr;
    for (Client &c : s_clients) {
      if (c.fd >= 0 && c.sub_services && !c.closing) {
        target = &c;
        break;
      }
    }
    Pending *slot = nullptr;
    for (Pending &pd : s_pending) {
      if (pd.done == nullptr) {
        slot = &pd;
        break;
      }
    }
    bool sent = false;
    // Events (done == nullptr) get no answer from HA: no pending slot.
    const bool slot_ok = qa.done == nullptr || slot != nullptr;
    if (target != nullptr && slot_ok && qa.len <= kTxCap) {
      memcpy(s_tx + kTxRoom, qa.msg, qa.len);
      sent = send_msg(*target, kHaActionRequest, qa.len);
    }
    heap_caps_free(qa.msg);
    if (sent && qa.done == nullptr) {
      // event: nothing to wait for
    } else if (sent) {
      slot->call_id = qa.call_id;
      slot->done = qa.done;
      slot->ctx = qa.ctx;
      slot->deadline_us = esp_timer_get_time() + kActionTimeoutUs;
    } else if (qa.done != nullptr) {
      qa.done(qa.ctx, false, "not sent", nullptr, 0);
    }
  }
  const int64_t now = esp_timer_get_time();
  for (Pending &pd : s_pending) {
    if (pd.done != nullptr && now > pd.deadline_us) {
      const ActionDone done = pd.done;
      void *ctx = pd.ctx;
      pd = Pending{};
      ESP_LOGW(TAG, "action call timed out (is \"Allow the device to perform "
                    "Home Assistant actions\" on?)");
      done(ctx, false, "timeout", nullptr, 0);
    }
  }
}

// --- Logs ---------------------------------------------------------------------

uint8_t level_of(const char *line, size_t len) {
  // ESP-IDF lines start with the level letter, possibly after an ANSI colour
  // sequence ("\033[0;32mI (123) tag: ...").
  size_t i = 0;
  if (len > 0 && line[0] == '\033') {
    while (i < len && line[i] != 'm') {
      i++;
    }
    i++;
  }
  switch (i < len ? line[i] : 'I') {
  case 'E':
    return 1;
  case 'W':
    return 2;
  case 'D':
    return 5;
  case 'V':
    return 6;
  default:
    return 3;
  }
}

size_t ring_pop(uint8_t *out, size_t cap) {
  size_t n = 0;
  taskENTER_CRITICAL(&s_log_mux);
  if (s_log_used >= 2) {
    const size_t len = (size_t)s_log_ring[s_log_tail] << 8 |
                       s_log_ring[(s_log_tail + 1) % kLogRingBytes];
    s_log_tail = (s_log_tail + 2) % kLogRingBytes;
    for (size_t i = 0; i < len; i++) {
      if (i < cap) {
        out[i] = s_log_ring[s_log_tail];
      }
      s_log_tail = (s_log_tail + 1) % kLogRingBytes;
    }
    s_log_used -= 2 + len;
    n = len < cap ? len : cap;
  }
  taskEXIT_CRITICAL(&s_log_mux);
  return n;
}

void drain_logs() {
  if (s_log_subscribers.load() == 0) {
    return;
  }
  uint8_t line[256];
  // Bounded per loop pass: a log storm must not starve commands and pings.
  for (int k = 0; k < 32; k++) {
    const size_t n = ring_pop(line, sizeof line);
    if (n == 0) {
      return;
    }
    size_t len = n;
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
      len--;
    }
    const uint8_t lvl = level_of(reinterpret_cast<const char *>(line), len);
    esphome_pb::Writer w = writer();
    w.u32(1, lvl);
    w.bytes(3, line, len);
    if (!w.ok()) {
      continue;
    }
    for (Client &c : s_clients) {
      if (c.fd >= 0 && c.sub_logs && lvl <= c.log_level) {
        send_msg(c, kSubscribeLogsResponse, w.size());
      }
    }
  }
}

// --- Task --------------------------------------------------------------------------

int open_listener() {
  const int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) {
    return -1;
  }
  const int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(kPort);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 ||
      listen(fd, 2) != 0) {
    ESP_LOGE(TAG, "bind/listen %u: errno %d", kPort, errno);
    close(fd);
    return -1;
  }
  return fd;
}

void server_task(void *) {
  int lfd = -1;
  int64_t next_refresh = esp_timer_get_time() + kRefreshUs;
  for (;;) {
    if (lfd < 0) {
      lfd = open_listener();
      if (lfd < 0) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        continue;
      }
      ESP_LOGI(TAG, "listening on %u (%s)", kPort,
               s_has_psk ? "encrypted" : "no key yet");
    }

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(lfd, &rd);
    int maxfd = lfd;
    for (Client &c : s_clients) {
      if (c.fd >= 0) {
        FD_SET(c.fd, &rd);
        maxfd = c.fd > maxfd ? c.fd : maxfd;
      }
    }
    timeval tv = {0, 100 * 1000};
    const int n = select(maxfd + 1, &rd, nullptr, nullptr, &tv);
    if (n < 0) {
      ESP_LOGE(TAG, "select: errno %d, reopening", errno);
      close(lfd);
      lfd = -1;
      for (Client &c : s_clients) {
        close_client(c);
      }
      continue;
    }
    if (n > 0) {
      if (FD_ISSET(lfd, &rd)) {
        accept_client(lfd);
      }
      for (Client &c : s_clients) {
        if (c.fd >= 0 && FD_ISSET(c.fd, &rd)) {
          read_client(c);
        }
      }
    }

    drain_actions();
    flush_watches();
    drain_logs();

    const uint64_t dirty = s_dirty.exchange(0);
    for (size_t i = 0; dirty != 0 && i < s_cfg->entity_count; i++) {
      if ((dirty >> i) & 1) {
        push_state(i);
      }
    }

    const int64_t now = esp_timer_get_time();
    if (now >= next_refresh) {
      next_refresh = now + kRefreshUs;
      const bool busy = s_cfg->busy != nullptr && s_cfg->busy();
      for (size_t i = 0; i < s_cfg->entity_count; i++) {
        if (!busy || s_cfg->entities[i].refresh_when_busy) {
          push_state(i);
        }
      }
    }

    for (Client &c : s_clients) {
      if (c.fd >= 0 && now - c.last_rx_us > kIdleCloseUs) {
        ESP_LOGW(TAG, "%s: idle, closing", c.peer);
        c.closing = true;
      }
      if (c.fd >= 0 && c.closing) {
        close_client(c);
      }
    }
  }
}

} // namespace

bool start(const Config &cfg) {
  if (s_cfg != nullptr) {
    return true;
  }
  if (cfg.entity_count > kMaxEntities) {
    ESP_LOGE(TAG, "%u entities > %u", (unsigned)cfg.entity_count,
             (unsigned)kMaxEntities);
    return false;
  }
  s_tx = static_cast<uint8_t *>(
      heap_caps_malloc(kTxRoom + kTxCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  s_enc = static_cast<uint8_t *>(heap_caps_malloc(
      esphome_frame::kNoiseHeaderLen + esphome_frame::kInnerHeaderLen + kTxCap +
          noise::kTagLen,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  s_dec = static_cast<uint8_t *>(
      heap_caps_malloc(kRxMax + 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  s_actions = xQueueCreate(4, sizeof(QueuedAction));
  s_log_ring = static_cast<uint8_t *>(
      heap_caps_malloc(kLogRingBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_tx == nullptr || s_enc == nullptr || s_dec == nullptr ||
      s_actions == nullptr || s_log_ring == nullptr) {
    ESP_LOGE(TAG, "out of memory");
    return false;
  }
  s_cfg = &cfg;
  load_psk();
  if (!mdns_start()) {
    // Not fatal: HA can still add the device by IP.
    ESP_LOGW(TAG, "mDNS unavailable: add the device in HA by its IP");
  }
  // Core 0 with the other network tasks, below the MJPEG drain. Stack in
  // PSRAM: internal SRAM is what DTLS needs during a call; command callbacks
  // write NVS, hence 12 KB (FATFS/NVS want >= 8 KB, cf. CLAUDE.md).
  if (xTaskCreatePinnedToCoreWithCaps(server_task, "esphome_api", 12 * 1024,
                                      nullptr, 3, nullptr, 0,
                                      MALLOC_CAP_SPIRAM) != pdPASS) {
    ESP_LOGE(TAG, "task not created");
    s_cfg = nullptr;
    return false;
  }
  return true;
}

void publish(const char *object_id) {
  if (s_cfg == nullptr) {
    return;
  }
  for (size_t i = 0; i < s_cfg->entity_count; i++) {
    if (strcmp(s_cfg->entities[i].object_id, object_id) == 0) {
      s_dirty.fetch_or(1ULL << i);
      return;
    }
  }
}

bool connected() { return s_subscribed.load() > 0; }

bool provisioned() { return s_has_psk.load(); }

void log_line(const char *line, size_t len) {
  if (s_log_ring == nullptr || s_log_subscribers.load() == 0 || len == 0) {
    return;
  }
  if (len > 255) {
    len = 255;
  }
  taskENTER_CRITICAL(&s_log_mux);
  if (s_log_used + 2 + len <= kLogRingBytes) {
    s_log_ring[s_log_head] = (uint8_t)(len >> 8);
    s_log_ring[(s_log_head + 1) % kLogRingBytes] = (uint8_t)len;
    s_log_head = (s_log_head + 2) % kLogRingBytes;
    for (size_t i = 0; i < len; i++) {
      s_log_ring[s_log_head] = (uint8_t)line[i];
      s_log_head = (s_log_head + 1) % kLogRingBytes;
    }
    s_log_used += 2 + len;
  }
  taskEXIT_CRITICAL(&s_log_mux);
}

namespace {
// Encodes a HomeassistantActionRequest into a PSRAM buffer and queues it for
// the server task. call_id 0 = none (events).
bool queue_request(const char *service, const Kv *data, size_t n,
                   bool is_event, uint32_t call_id, bool wants_response,
                   const char *response_template, ActionDone done, void *ctx) {
  if (s_cfg == nullptr || s_actions == nullptr) {
    return false;
  }
  constexpr size_t kCap = 1024;
  auto *buf = static_cast<uint8_t *>(
      heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    return false;
  }
  esphome_pb::Writer w(buf, kCap);
  w.str(1, service);
  for (size_t i = 0; i < n; i++) {
    uint8_t entry[192];
    esphome_pb::Writer m(entry, sizeof entry);
    m.str(1, data[i].key);
    m.str(2, data[i].value);
    if (!m.ok()) {
      heap_caps_free(buf);
      return false;
    }
    w.bytes(2, m.data(), m.size());
  }
  w.boolean(5, is_event);
  w.u32(6, call_id);
  if (wants_response) {
    w.boolean(7, true);
    w.str(8, response_template);
  }
  if (!w.ok()) {
    heap_caps_free(buf);
    return false;
  }
  const QueuedAction qa = {buf, w.size(), call_id, done, ctx};
  if (xQueueSend(s_actions, &qa, 0) != pdTRUE) {
    heap_caps_free(buf);
    return false;
  }
  return true;
}
} // namespace

bool call_action(const char *service, const Kv *data, size_t n,
                 const char *response_template, ActionDone done, void *ctx,
                 bool wants_response) {
  const uint32_t id = s_next_call_id.fetch_add(1);
  return queue_request(service, data, n, false, id, wants_response,
                       response_template, done, ctx);
}

bool fire_event(const char *name, const Kv *data, size_t n) {
  if (name == nullptr || strncmp(name, "esphome.", 8) != 0) {
    return false;
  }
  return queue_request(name, data, n, true, 0, false, nullptr, nullptr,
                       nullptr);
}

void watch_ha_state(uint8_t slot, const char *entity_id) {
  if (slot >= kMaxHaWatches) {
    return;
  }
  char copy[sizeof(Watch::entity_id)];
  snprintf(copy, sizeof copy, "%s", entity_id != nullptr ? entity_id : "");
  taskENTER_CRITICAL(&s_watch_mux);
  memcpy(s_watches[slot].entity_id, copy, sizeof copy);
  s_watches[slot].pending = copy[0] != '\0';
  taskEXIT_CRITICAL(&s_watch_mux);
  s_watch_pending.store(true);
}

namespace {
struct SyncCall {
  SemaphoreHandle_t done;
  char *out;
  size_t cap;
  size_t len;
  bool ok;
};

void sync_done(void *ctx, bool ok, const char *error, const char *json,
               size_t len) {
  auto *sc = static_cast<SyncCall *>(ctx);
  sc->ok = false;
  if (!ok) {
    ESP_LOGW(TAG, "action failed: %s", error != nullptr ? error : "?");
  } else if (len >= sc->cap) {
    ESP_LOGE(TAG, "action response %u B > %u B buffer", (unsigned)len,
             (unsigned)sc->cap);
  } else {
    memcpy(sc->out, json, len);
    sc->out[len] = '\0';
    sc->len = len;
    sc->ok = true;
  }
  xSemaphoreGive(sc->done);
}
} // namespace

bool call_action_sync(const char *service, const Kv *data, size_t n,
                      const char *response_template, char *out, size_t cap,
                      size_t *len) {
  SyncCall sc = {xSemaphoreCreateBinary(), out, cap, 0, false};
  if (sc.done == nullptr) {
    return false;
  }
  if (!call_action(service, data, n, response_template, sync_done, &sc)) {
    vSemaphoreDelete(sc.done);
    return false;
  }
  // No timeout here on purpose: call_action guarantees exactly one callback
  // (answer, refusal, or the server's own 20 s timeout). Giving up earlier
  // would leave the server writing into this stack frame later.
  xSemaphoreTake(sc.done, portMAX_DELAY);
  vSemaphoreDelete(sc.done);
  if (len != nullptr) {
    *len = sc.len;
  }
  return sc.ok;
}

} // namespace esphome_api
