#include "gate_logic.h"

#include <cstring>

namespace gate_logic {

State parse(const char *ha_state) {
  if (ha_state == nullptr) {
    return State::kUnknown;
  }
  if (strcmp(ha_state, "open") == 0) {
    return State::kOpen;
  }
  if (strcmp(ha_state, "closed") == 0) {
    return State::kClosed;
  }
  if (strcmp(ha_state, "opening") == 0) {
    return State::kOpening;
  }
  if (strcmp(ha_state, "closing") == 0) {
    return State::kClosing;
  }
  if (strcmp(ha_state, "unavailable") == 0) {
    return State::kUnavailable;
  }
  return State::kUnknown;
}

Action next_action(State s) {
  switch (s) {
  case State::kClosed:
    return Action::kOpen;
  case State::kOpen:
    return Action::kClose;
  case State::kOpening:
  case State::kClosing:
    return Action::kStop;
  case State::kUnknown:
  case State::kUnavailable:
    break;
  }
  return Action::kNone;
}

const char *service_for(Action a) {
  switch (a) {
  case Action::kOpen:
    return "cover.open_cover";
  case Action::kClose:
    return "cover.close_cover";
  case Action::kStop:
    return "cover.stop_cover";
  case Action::kNone:
    break;
  }
  return nullptr;
}

const char *event_name_for(Action a) {
  switch (a) {
  case Action::kOpen:
    return "open";
  case Action::kClose:
    return "close";
  case Action::kStop:
    return "stop";
  case Action::kNone:
    break;
  }
  return nullptr;
}

Look look(State s, bool configured, bool ha_connected, bool failed) {
  if (!configured) {
    return {"Portail", "", "à configurer dans HA", Tone::kOff, false};
  }
  constexpr Look kUnavailableLook = {"Portail indisponible", "Indisponible", "",
                                     Tone::kOff, false};
  if (!ha_connected) {
    return kUnavailableLook;
  }
  if (failed) {
    return {"Échec portail", "Échec", "", Tone::kOff, true};
  }
  switch (next_action(s)) {
  case Action::kOpen:
    return {"Ouvrir le portail", "Ouvrir", "", Tone::kOpen, true};
  case Action::kClose:
    return {"Fermer le portail", "Fermer", "", Tone::kClose, true};
  case Action::kStop:
    return {"Arrêter", "Arrêter", "", Tone::kStop, true};
  case Action::kNone:
    break;
  }
  return kUnavailableLook;
}

} // namespace gate_logic
