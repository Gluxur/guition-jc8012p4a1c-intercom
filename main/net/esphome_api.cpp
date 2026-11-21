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
constexpr const char *kProjectName = "gluxur.esp32-p4-video-intercom";
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
  kListSensor = 16,
  kListSwitch = 17,
  kListTextSensor = 18,
  kListEntitiesDone = 19,
  kSubscribeStatesRequest = 20,
  kBinarySensorState = 21,
  kSensorState = 25,
  kSwitchState = 26,
  kTextSensorState = 27,
  kSwitchCommand = 33,
  kSubscribeHaServices = 34,
  kHaActionRequest = 35,
  kSubscribeHaStates = 38,
  kListNumber = 49,
  kNumberState = 50,
  kNumberCommand = 51,
  kListSelect = 52,
  kSelectState = 53,
  kSelectCommand = 54,
  kListButton = 61,
  kButtonCommand = 62,
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
  bool closing = false; // close once the current batch is processed
  int64_t last_rx_us = 0;
  char peer[16] = {};
};

struct QueuedAction {
  uint8_t *msg; // encoded HomeassistantActionRequest, heap_caps_malloc'd
  size_t len;
  uint32_t call_id;
  ActionDone done;
};

struct Pending {
  uint32_t call_id = 0;
  ActionDone done = nullptr;
  int64_t deadline_us = 0;
};

const Config *s_cfg = nullptr;
Client s_clients[kMaxClients];
Pending s_pending[4];
QueueHandle_t s_actions = nullptr;
std::atomic<uint64_t> s_dirty{0};
std::atomic<int> s_subscribed{0};
std::atomic<uint32_t> s_next_call_id{1};

uint8_t s_psk[noise::kKeyLen] = {};
bool s_has_psk = false;

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
  case Kind::kSelect: {
    if (e.get_text == nullptr) {
      return 0;
    }
    char buf[128];
    buf[0] = '\0';
    e.get_text(buf, sizeof buf);
    w.str(2, buf);
    return e.kind == Kind::kSelect ? kSelectState : kTextSensorState;
  }
  case Kind::kButton:
    return 0;
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
    return;
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
  char text[64] = {};
  while (r.next()) {
    if (r.field() == 1) {
      key = r.as_fixed32();
    } else if (r.field() == 2) {
      if (type == kSwitchCommand) {
        b = r.as_bool();
      } else if (type == kNumberCommand) {
        f = r.as_f32();
      } else if (type == kSelectCommand) {
        size_t n;
        const uint8_t *p = r.as_bytes(&n);
        if (p != nullptr) {
          snprintf(text, sizeof text, "%.*s", (int)n, (const char *)p);
        }
      }
    }
  }
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
    if (e->set_text) {
      e->set_text(text);
    }
    break;
  case kButtonCommand:
    if (e->press) {
      e->press();
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
      pd = Pending{};
      done(success, error, (const char *)data, data_len);
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
    break; // no imported HA state yet: nothing to send
  case kSwitchCommand:
  case kNumberCommand:
  case kSelectCommand:
  case kButtonCommand:
    on_command(type, r);
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
      size_t hl;
      uint32_t plen, type;
      const auto st = esphome_frame::parse_plain_header(p, avail, &hl, &plen, &type);
      if (st == esphome_frame::Parse::kBad || plen > kRxMax) {
        c.closing = true;
        break;
      }
      if (st == esphome_frame::Parse::kNeedMore || avail < hl + plen) {
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
  c.hello = c.sub_states = c.sub_services = c.closing = false;
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
    if (target != nullptr && slot != nullptr && qa.len <= kTxCap) {
      memcpy(s_tx + kTxRoom, qa.msg, qa.len);
      sent = send_msg(*target, kHaActionRequest, qa.len);
    }
    heap_caps_free(qa.msg);
    if (sent) {
      slot->call_id = qa.call_id;
      slot->done = qa.done;
      slot->deadline_us = esp_timer_get_time() + kActionTimeoutUs;
    } else if (qa.done != nullptr) {
      qa.done(false, "not sent", nullptr, 0);
    }
  }
  const int64_t now = esp_timer_get_time();
  for (Pending &pd : s_pending) {
    if (pd.done != nullptr && now > pd.deadline_us) {
      const ActionDone done = pd.done;
      pd = Pending{};
      ESP_LOGW(TAG, "action call timed out (is \"Allow the device to perform "
                    "Home Assistant actions\" on?)");
      done(false, "timeout", nullptr, 0);
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

    const uint64_t dirty = s_dirty.exchange(0);
    for (size_t i = 0; dirty != 0 && i < s_cfg->entity_count; i++) {
      if ((dirty >> i) & 1) {
        push_state(i);
      }
    }

    const int64_t now = esp_timer_get_time();
    if (now >= next_refresh) {
      next_refresh = now + kRefreshUs;
      if (s_cfg->busy == nullptr || !s_cfg->busy()) {
        for (size_t i = 0; i < s_cfg->entity_count; i++) {
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
  if (s_tx == nullptr || s_enc == nullptr || s_dec == nullptr ||
      s_actions == nullptr) {
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

bool call_action(const char *service, const Kv *data, size_t n,
                 const char *response_template, ActionDone done) {
  if (s_cfg == nullptr || s_actions == nullptr) {
    return false;
  }
  constexpr size_t kCap = 1024;
  auto *buf = static_cast<uint8_t *>(
      heap_caps_malloc(kCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    return false;
  }
  const uint32_t id = s_next_call_id.fetch_add(1);
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
  w.u32(6, id);
  w.boolean(7, true); // wants_response
  w.str(8, response_template);
  if (!w.ok()) {
    heap_caps_free(buf);
    return false;
  }
  const QueuedAction qa = {buf, w.size(), id, done};
  if (xQueueSend(s_actions, &qa, 0) != pdTRUE) {
    heap_caps_free(buf);
    return false;
  }
  return true;
}

} // namespace esphome_api
