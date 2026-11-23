#include "gate_logic.h"

#include "sys/i18n.h"

#include <cstring>

using i18n::Str;

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
    return {i18n::t(Str::kHomeGate), "", i18n::t(Str::kGateSetUpInHa),
            Tone::kOff, false};
  }
  const Look unavailable = {i18n::t(Str::kGateUnavailable),
                            i18n::t(Str::kGateUnavailableShort), "", Tone::kOff,
                            false};
  if (!ha_connected) {
    return unavailable;
  }
  if (failed) {
    return {i18n::t(Str::kGateFailed), i18n::t(Str::kGateFailedShort), "",
            Tone::kOff, true};
  }
  switch (next_action(s)) {
  case Action::kOpen:
    return {i18n::t(Str::kGateOpen), i18n::t(Str::kGateOpenShort), "",
            Tone::kOpen, true};
  case Action::kClose:
    return {i18n::t(Str::kGateClose), i18n::t(Str::kClose), "",
            Tone::kClose, true};
  case Action::kStop:
    return {i18n::t(Str::kGateStop), i18n::t(Str::kGateStop), "", Tone::kStop, true};
  case Action::kNone:
    break;
  }
  return unavailable;
}

bool accept_state(const char *watched_id, const char *entity_id,
                  const char *attribute) {
  if (watched_id == nullptr || entity_id == nullptr || watched_id[0] == 0) {
    return false;
  }
  if (attribute != nullptr && attribute[0] != 0) {
    return false;
  }
  return strcmp(watched_id, entity_id) == 0;
}

} // namespace gate_logic
