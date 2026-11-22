#include <gtest/gtest.h>

#include "app/gate_logic.h"

#include <cstring>

using gate_logic::Action;
using gate_logic::Look;
using gate_logic::State;
using gate_logic::Tone;

namespace {

void ExpectLook(const Look &l, const char *label, const char *short_label,
                const char *hint, Tone tone, bool enabled) {
  EXPECT_STREQ(l.label, label);
  EXPECT_STREQ(l.short_label, short_label);
  EXPECT_STREQ(l.hint, hint);
  EXPECT_EQ(l.tone, tone);
  EXPECT_EQ(l.enabled, enabled);
}

} // namespace

TEST(GateLogic, ParseEveryHaState) {
  EXPECT_EQ(gate_logic::parse("open"), State::kOpen);
  EXPECT_EQ(gate_logic::parse("closed"), State::kClosed);
  EXPECT_EQ(gate_logic::parse("opening"), State::kOpening);
  EXPECT_EQ(gate_logic::parse("closing"), State::kClosing);
  EXPECT_EQ(gate_logic::parse("unavailable"), State::kUnavailable);
  EXPECT_EQ(gate_logic::parse("unknown"), State::kUnknown);
  EXPECT_EQ(gate_logic::parse(""), State::kUnknown);
  EXPECT_EQ(gate_logic::parse(nullptr), State::kUnknown);
  EXPECT_EQ(gate_logic::parse("OPEN"), State::kUnknown);
  EXPECT_EQ(gate_logic::parse("open "), State::kUnknown);
}

TEST(GateLogic, NextActionTable) {
  EXPECT_EQ(gate_logic::next_action(State::kClosed), Action::kOpen);
  EXPECT_EQ(gate_logic::next_action(State::kOpen), Action::kClose);
  EXPECT_EQ(gate_logic::next_action(State::kOpening), Action::kStop);
  EXPECT_EQ(gate_logic::next_action(State::kClosing), Action::kStop);
  EXPECT_EQ(gate_logic::next_action(State::kUnknown), Action::kNone);
  EXPECT_EQ(gate_logic::next_action(State::kUnavailable), Action::kNone);
}

TEST(GateLogic, NoActionWhenUnknown) {
  EXPECT_EQ(gate_logic::service_for(gate_logic::next_action(State::kUnknown)),
            nullptr);
}

TEST(GateLogic, ServiceAndEventNames) {
  EXPECT_STREQ(gate_logic::service_for(Action::kOpen), "cover.open_cover");
  EXPECT_STREQ(gate_logic::service_for(Action::kClose), "cover.close_cover");
  EXPECT_STREQ(gate_logic::service_for(Action::kStop), "cover.stop_cover");
  EXPECT_STREQ(gate_logic::event_name_for(Action::kOpen), "open");
  EXPECT_STREQ(gate_logic::event_name_for(Action::kClose), "close");
  EXPECT_STREQ(gate_logic::event_name_for(Action::kStop), "stop");
  EXPECT_EQ(gate_logic::event_name_for(Action::kNone), nullptr);
}

TEST(GateLogic, LookPerState) {
  ExpectLook(gate_logic::look(State::kClosed, true, true, false),
             "Ouvrir le portail", "Ouvrir", "", Tone::kOpen, true);
  ExpectLook(gate_logic::look(State::kOpen, true, true, false),
             "Fermer le portail", "Fermer", "", Tone::kClose, true);
  ExpectLook(gate_logic::look(State::kOpening, true, true, false), "Arrêter",
             "Arrêter", "", Tone::kStop, true);
  ExpectLook(gate_logic::look(State::kClosing, true, true, false), "Arrêter",
             "Arrêter", "", Tone::kStop, true);
  ExpectLook(gate_logic::look(State::kUnknown, true, true, false),
             "Portail indisponible", "Indisponible", "", Tone::kOff, false);
  ExpectLook(gate_logic::look(State::kUnavailable, true, true, false),
             "Portail indisponible", "Indisponible", "", Tone::kOff, false);
}

TEST(GateLogic, LookDisabledWhenHaDown) {
  ExpectLook(gate_logic::look(State::kOpen, true, false, false),
             "Portail indisponible", "Indisponible", "", Tone::kOff, false);
}

TEST(GateLogic, LookFailed) {
  ExpectLook(gate_logic::look(State::kClosed, true, true, true),
             "Échec portail", "Échec", "", Tone::kOff, true);
}

TEST(GateLogic, LookUnconfigured) {
  ExpectLook(gate_logic::look(State::kOpen, false, true, true), "Portail", "",
             "à configurer dans HA", Tone::kOff, false);
}
