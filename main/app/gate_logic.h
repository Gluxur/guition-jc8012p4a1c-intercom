#pragma once

#include <cstdint>

// Pure logic of the gate button: HA cover state -> next action -> what the
// three screens show. No ESP-IDF header, host-tested (test/test_gate_logic).
// The firmware side (HA subscription, action call) lives in app/gate.
namespace gate_logic {

enum class State : uint8_t { kUnknown, kUnavailable, kClosed, kOpening, kOpen, kClosing };
enum class Action : uint8_t { kNone, kOpen, kClose, kStop };
// Button colour family: green, blue, orange, grey (mapped to the theme by
// ui/gate_style).
enum class Tone : uint8_t { kOpen, kClose, kStop, kOff };

// All strings are literals (static storage).
struct Look {
  const char *label;       // home tile and camera band
  const char *short_label; // weather header
  const char *hint;        // second line when unconfigured, "" otherwise
  Tone tone;
  bool enabled;
};

// HA cover state string. nullptr and anything unexpected -> kUnknown: a
// button must never claim a state the gate is not known to be in.
[[nodiscard]] State parse(const char *ha_state);

[[nodiscard]] Action next_action(State s);

// "cover.open_cover" / "cover.close_cover" / "cover.stop_cover"; nullptr for
// kNone.
[[nodiscard]] const char *service_for(Action a);

// Value of the `action` field of the esphome.visiophone_gate event.
[[nodiscard]] const char *event_name_for(Action a);

// Precedence: not configured > HA disconnected > last command failed > state.
[[nodiscard]] Look look(State s, bool configured, bool ha_connected, bool failed);

// True iff a state pushed by HA belongs to the watched gate: same id, and the
// state itself (no attribute). HA keeps sending a replaced id until it
// reconnects; those must not move the button.
[[nodiscard]] bool accept_state(const char *watched_id, const char *entity_id,
                                const char *attribute);

} // namespace gate_logic
