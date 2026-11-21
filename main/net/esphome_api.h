#pragma once

// ESPHome native API server: makes the board a regular "ESPHome device" for
// Home Assistant, without ESPHome itself. HA's ESPHome integration discovers
// it over mDNS, lists its entities, receives their states and sends commands;
// the board can in turn call HA actions and read their responses (weather
// forecast, calendar events) without a long-lived access token.
//
// Transport: TCP 6053, Noise_NNpsk0_25519_ChaChaPoly_SHA256 once HA has
// provisioned a key (it does so by itself on first contact, then the key
// lives in NVS); plaintext is accepted only while no key is set, like ESPHome.
//
// This module knows nothing about the intercom: entities are described by
// the caller (app/ layer) as a table of plain function pointers.

#include <cstddef>
#include <cstdint>

namespace esphome_api {

enum class Kind : uint8_t {
  kBinarySensor,
  kSensor,
  kTextSensor,
  kSwitch,
  kNumber,
  kSelect,
  kButton,
  kLight, // brightness-only light (the screen backlight)
};

// Values of api.proto's EntityCategory.
enum class Category : uint8_t { kNone = 0, kConfig = 1, kDiagnostic = 2 };

// One HA entity. Only the members relevant to `kind` are read; the others
// stay at their defaults. Getters and setters run in the API server task
// (core 0, 12 KB stack in PSRAM): they may touch NVS, never block for long.
struct Entity {
  Kind kind;
  const char *object_id; // stable: HA derives the entity's unique_id from it
  const char *name;
  const char *icon = nullptr; // "mdi:..."
  const char *device_class = nullptr;
  const char *unit = nullptr;
  Category category = Category::kNone;
  // kNumber
  float min = 0, max = 0, step = 1;
  // kSensor
  int8_t accuracy = 0;
  // kSelect
  const char *const *options = nullptr;
  uint8_t option_count = 0;
  // State. kBinarySensor/kSwitch: get_bool; kSensor/kNumber: get_float (NAN
  // = unknown); kTextSensor/kSelect: get_text; kLight: get_bool (on) and
  // get_float (brightness 0..1). nullptr = no state pushed.
  bool (*get_bool)() = nullptr;
  float (*get_float)() = nullptr;
  void (*get_text)(char *out, size_t cap) = nullptr;
  // Commands from HA. kSwitch: set_bool; kNumber: set_float; kSelect:
  // set_text (one of `options`); kButton: press.
  void (*set_bool)(bool) = nullptr;
  void (*set_float)(float) = nullptr;
  void (*set_text)(const char *) = nullptr;
  void (*press)() = nullptr;
  // kLight: `state` -1 = unchanged, 0 = off, 1 = on; `brightness` 0..1 or
  // NAN = unchanged.
  void (*set_light)(int state, float brightness) = nullptr;
  // get_*() reads the SD card: skipped while `Config::busy` says so.
  bool sd_touching = false;
  // Still refreshed every 30 s while `Config::busy` (e.g. the in-call state).
  bool refresh_when_busy = false;
};

struct Config {
  const char *name;          // mDNS host and node name: [a-z0-9-], <= 31
  const char *friendly_name; // shown by HA
  const char *project_version;
  const Entity *entities;
  size_t entity_count; // <= kMaxEntities
  // During a call: no periodic refresh, no SD-touching getter. Explicit
  // publish() still goes out (presence, in-call state).
  bool (*busy)() = nullptr;
  // Called once per client when it subscribes to HA actions (in the server
  // task). Lets the app fire its first action calls as soon as possible.
  void (*on_ha_ready)() = nullptr;
};

inline constexpr size_t kMaxEntities = 64;

// Starts the mDNS advertisement and the server task. Call once, after
// Wi-Fi is initialised (it can still be down: the server simply waits).
// `cfg` and everything it points to must outlive the program.
bool start(const Config &cfg);

// Pushes the current state of `object_id` to every subscribed client within
// ~100 ms. Safe from any task; cheap (sets a bit).
void publish(const char *object_id);

// True while at least one client (normally HA) is subscribed to states.
[[nodiscard]] bool connected();

// dbg_log line sink: forwards log lines to clients that subscribed to logs
// (HA's "subscribe to logs" option, `aioesphomeapi-logs`, ...). Called from
// any task in the logging path: only copies into a ring, never blocks.
void log_line(const char *line, size_t len);

// --- Home Assistant actions -------------------------------------------------

struct Kv {
  const char *key;
  const char *value;
};

// `ok` false: HA reported an error, refused the call (the device option
// "Allow the device to perform Home Assistant actions" is off), or no answer
// within 20 s. `json`/`len`: the response body, valid during the callback
// only, NOT NUL-terminated: {"response": <action response or template>}.
using ActionDone = void (*)(void *ctx, bool ok, const char *error,
                            const char *json, size_t len);

// Queues `service` ("weather.get_forecasts") with string `data`, asking for
// its response. `response_template`: optional Jinja rendered by HA with the
// variable `response` (and the usual template functions: states(),
// state_attr()...), to reshape or shrink the payload. All strings are copied.
// Returns false if no client is subscribed to actions or the queue is full;
// otherwise `done(ctx, ...)` is called exactly once, in the server task.
bool call_action(const char *service, const Kv *data, size_t n,
                 const char *response_template, ActionDone done, void *ctx);

// Blocking variant for fetch tasks: waits for the answer and copies the body
// into `out` (NUL-terminated). Returns false on any failure, including a body
// of `cap` bytes or more. Never call it from the server task itself.
bool call_action_sync(const char *service, const Kv *data, size_t n,
                      const char *response_template, char *out, size_t cap,
                      size_t *len);

} // namespace esphome_api
