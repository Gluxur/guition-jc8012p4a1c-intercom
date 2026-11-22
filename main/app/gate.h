#pragma once

#include <cstdint>

#include "app/gate_logic.h"

// The gate (HA cover.*) driven from the tablet: live state imported from HA
// through the ESPHome API, one press = the action gate_logic picks. The three
// screens read look() and poll version() from their LVGL timers.
namespace gate {

// ESPHome API watch slot used for the gate's state.
inline constexpr uint8_t kWatchSlot = 0;

// Reads the configured id from prefs and starts watching it. Call once, after
// ha_api::init (the API server must exist).
void init();

// New id from the HA device page (already validated; "" = no gate). Resets the
// state to unknown until HA pushes the new entity's state.
void set_entity(const char *normalized_id);

// Config::on_ha_state hook, server task.
void on_ha_state(const char *entity_id, const char *attribute, const char *state);

// Sends the action for the current state. False when nothing can be sent
// (unknown state, HA down, no gate configured, queue full). Never blocks:
// safe from the LVGL task.
bool press();

[[nodiscard]] gate_logic::Look look();

// Bumped on every change look() could reflect.
[[nodiscard]] uint32_t version();

} // namespace gate
