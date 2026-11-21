#pragma once

// Entities exposed to Home Assistant over the ESPHome native API
// (net/esphome_api). PROTOTYPE: three entities and one action call, to
// validate the transport next to the MQTT catalogue (app/ha_entities), which
// stays the reference until the migration is complete.
//
// Lives in app/ for the same reason as ha_entities: it reads the BSP, prefs
// and presence; the transport in net/ knows none of them.
namespace ha_api {

struct Hooks {
  void (*ring)(); // same path as the MQTT doorbell
};

// Call after Wi-Fi init. Starts mDNS + the API server.
void init(const Hooks &hooks);

// Pushes the state of one entity now (any task).
void publish_now(const char *object_id);

} // namespace ha_api
