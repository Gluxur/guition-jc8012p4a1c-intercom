#pragma once

// Catalogue of the Home Assistant entities, exposed through the ESPHome
// native API (net/esphome_api). Home Assistant adopts the board as an ESPHome
// device: no broker, no token, encryption key provisioned by HA itself.
//
// Why app/ and not sys/: the catalogue reads the BSP (brightness, volumes),
// prefs, call_history, camera_session, sys_stats, file_server, and triggers
// ota_ctrl. app/ is the layer allowed to know everyone; the transport in net/
// knows none of them.
namespace ha_api {

// The only dependency that cannot be resolved here: opening the camera
// screen lives in main.cpp (it mixes ui_camera_v2, ringtone, call_history and
// audio_pipeline, and depends on the mic session state).
struct Hooks {
  void (*ring)(); // doorbell: full ring path (ringtone, street ding-dong, log)
};

// Starts mDNS + the API server. Call after Wi-Fi init (the link may still be
// down: the server waits) and after ota_ctrl::init (buttons call it).
void init(const Hooks &hooks);

// Pushes the state of one entity now (object_id of the catalogue), without
// waiting for the 30 s refresh. Safe from any task.
void publish_now(const char *object_id);

// True while Home Assistant (or any API client) is subscribed to states.
[[nodiscard]] bool connected();

} // namespace ha_api
