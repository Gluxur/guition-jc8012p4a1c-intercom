#include "gate.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "app/camera_session.h"
#include "app/ha_entity_id.h"
#include "net/esphome_api.h"
#include "sys/crumb.h"
#include "sys/prefs.h"

namespace gate {
namespace {

constexpr const char *TAG = "gate";
constexpr int64_t kFailShowUs = 3 * 1000 * 1000;

// Written by the API task (set_entity from the HA text field), read by the
// LVGL task (press, look) and the server task (on_ha_state): copied under a
// spinlock, the string is not a machine word.
char s_id[ha_entity_id::kMax] = {};
portMUX_TYPE s_id_mux = portMUX_INITIALIZER_UNLOCKED;

std::atomic<gate_logic::State> s_state{gate_logic::State::kUnknown};
std::atomic<int64_t> s_failed_until_us{0};
std::atomic<uint32_t> s_version{0};
// Action of the call in flight, for the event fired on success.
std::atomic<gate_logic::Action> s_last_action{gate_logic::Action::kNone};

// Inputs of look() that change without any message: the HA link and the end
// of the "failed" window. Sampled by a 1 s timer that only bumps s_version.
std::atomic<bool> s_seen_connected{false};
std::atomic<bool> s_seen_failed{false};
esp_timer_handle_t s_poll = nullptr;

void bump() { s_version.fetch_add(1); }

void copy_id(char *out, size_t cap) {
  char tmp[ha_entity_id::kMax];
  taskENTER_CRITICAL(&s_id_mux);
  memcpy(tmp, s_id, sizeof tmp);
  taskEXIT_CRITICAL(&s_id_mux);
  snprintf(out, cap, "%s", tmp);
}

bool failed_now() { return esp_timer_get_time() < s_failed_until_us.load(); }

void poll_cb(void *) {
  const bool c = esphome_api::connected();
  const bool f = failed_now();
  if (c != s_seen_connected.exchange(c) || f != s_seen_failed.exchange(f)) {
    if (!c) {
      // Reconnection resends the state; until then it is not known.
      s_state.store(gate_logic::State::kUnknown);
    }
    bump();
  }
}

// Server task.
void on_done(void *ctx, bool ok, const char *error, const char *json, size_t len) {
  (void)ctx;
  (void)json;
  (void)len;
  const gate_logic::Action a = s_last_action.exchange(gate_logic::Action::kNone);
  crumb::mark(crumb::Id::kGateDone, ok ? 1 : 0);
  if (ok) {
    const char *name = gate_logic::event_name_for(a);
    if (name != nullptr) {
      const esphome_api::Kv data[] = {
          {"action", name},
          {"during_call", camera_session::active() ? "true" : "false"},
      };
      if (!esphome_api::fire_event("esphome.visiophone_gate", data, 2)) {
        ESP_LOGW(TAG, "événement portail non envoyé");
      }
    }
  } else {
    ESP_LOGW(TAG, "commande portail refusée : %s", error != nullptr ? error : "?");
    s_failed_until_us.store(esp_timer_get_time() + kFailShowUs);
  }
  bump();
}

} // namespace

void init() {
  char id[ha_entity_id::kMax];
  prefs::ha_entity(prefs::HaEntity::kGate, id, sizeof id);
  set_entity(id);
  if (s_poll == nullptr) {
    const esp_timer_create_args_t args = {
        .callback = poll_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "gate_poll",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_poll) != ESP_OK ||
        esp_timer_start_periodic(s_poll, 1000 * 1000) != ESP_OK) {
      ESP_LOGE(TAG, "timer portail non créé : l'état HA ne se rafraîchira pas");
    }
  }
}

void set_entity(const char *normalized_id) {
  char copy[ha_entity_id::kMax];
  snprintf(copy, sizeof copy, "%s", normalized_id != nullptr ? normalized_id : "");
  taskENTER_CRITICAL(&s_id_mux);
  memcpy(s_id, copy, sizeof copy);
  taskEXIT_CRITICAL(&s_id_mux);
  s_state.store(gate_logic::State::kUnknown);
  esphome_api::watch_ha_state(kWatchSlot, copy);
  ESP_LOGI(TAG, "entité portail : %s", copy[0] ? copy : "(aucune)");
  bump();
}

void on_ha_state(const char *entity_id, const char *attribute, const char *state) {
  char id[ha_entity_id::kMax];
  copy_id(id, sizeof id);
  if (!gate_logic::accept_state(id, entity_id, attribute)) {
    return;
  }
  const gate_logic::State s = gate_logic::parse(state);
  if (s_state.exchange(s) != s) {
    bump();
  }
}

bool press() {
  crumb::mark(crumb::Id::kGatePress);
  char id[ha_entity_id::kMax];
  copy_id(id, sizeof id);
  const gate_logic::Action a = gate_logic::next_action(s_state.load());
  const char *service = gate_logic::service_for(a);
  if (id[0] == '\0' || service == nullptr || !esphome_api::connected()) {
    return false;
  }
  // One command at a time: a second tap while HA has not answered would
  // otherwise overwrite the action the event reports.
  gate_logic::Action expected = gate_logic::Action::kNone;
  if (!s_last_action.compare_exchange_strong(expected, a)) {
    return false;
  }
  const esphome_api::Kv data[] = {{"entity_id", id}};
  if (!esphome_api::call_action(service, data, 1, nullptr, on_done, nullptr,
                                false)) {
    s_last_action.store(gate_logic::Action::kNone);
    return false;
  }
  crumb::mark(crumb::Id::kGateQueued, static_cast<uint32_t>(a));
  return true;
}

gate_logic::Look look() {
  char id[ha_entity_id::kMax];
  copy_id(id, sizeof id);
  return gate_logic::look(s_state.load(), id[0] != '\0',
                          esphome_api::connected(), failed_now());
}

uint32_t version() { return s_version.load(); }

} // namespace gate
