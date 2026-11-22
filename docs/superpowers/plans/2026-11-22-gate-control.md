# Gate Control Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Open, close and stop the gate (`cover.*` in Home Assistant) from three tablet screens. Show its live state. Make the gate, weather and calendar entity ids editable from the HA device page.

**Architecture:**
- The ESPHome API server learns three things: text entities, HA state import (messages 38/39/40), and actions without response plus events.
- A pure `gate_logic` module decides the action, label and colour. `app/gate` connects it to the API.
- The three screens share one look from `gate_logic`, through `ui::Callbacks`.
- Entity ids live in `prefs` (NVS, applied live) and are validated by a pure helper.

**Tech Stack:** ESP-IDF 6.0.1, C++17, LVGL 9, GoogleTest host tests (`test/`, MinGW), our ESPHome API (`main/net/esphome_api`).

**Spec:** `docs/superpowers/specs/2026-11-22-gate-control-design.md`

## Global Constraints

- **Repo:** `D:\HomeAssistant\esp32-p4-video-intercom`, branch `feat/gate-control`.
- **Commits:** author `Gluxur <5436326+Gluxur@users.noreply.github.com>` (repo-local config already set). Every message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **No personal data** in tracked files: entity ids are examples only. Real values go in git-ignored `main/app_secrets.h`.
- **Code rules:** `CLAUDE.md` of the repo:
  - no `ESP_ERROR_CHECK`;
  - `std::atomic` for cross-task flags;
  - no `lv_*` outside the LVGL task without `bsp::Bsp::LvglLock`;
  - no `ESP_LOGI` in hot paths;
  - new `.cpp` added to `main/CMakeLists.txt` SRCS (and `test/CMakeLists.txt` for pure ones);
  - qualified includes (`"app/gate_logic.h"`).
- **UI copy** (French, exact):
  - button labels: "Ouvrir le portail", "Fermer le portail", "Arrêter", "Portail indisponible", "Échec portail";
  - short forms (weather header): "Ouvrir", "Fermer", "Arrêter", "Indisponible", "Échec";
  - unconfigured: label "Portail", hint "à configurer dans HA".
- **HA entity names** (ASCII, like the existing catalogue): "Entite portail", "Entite meteo", "Entite agenda"; object_ids `gate_entity`, `weather_entity`, `calendar_entity`; category CONFIG.
- **Event:** `esphome.visiophone_gate`, data `action` = `open` | `close` | `stop`, `during_call` = `true` | `false`.
- **Protocol ids:**
  - text: list 97, state 98, command 99;
  - HA states: subscribe 38 (from HA), 39 (to HA: `entity_id` 1, `attribute` 2, `once` 3), 40 (from HA: `entity_id` 1, `state` 2, `attribute` 3);
  - action request 35: `is_event` = field 5, `call_id` 6, `wants_response` 7.
- **Build:** shell recipe in memory `build-esp-idf-from-shell` (PowerShell env + `python $env:IDF_PATH\tools\idf.py build`). **Never** `idf.py set-target`.
- **Host tests:** `cd test ; pwsh -NoProfile -File run.ps1` → `100% tests passed`.

## Review Focus

1. **Gate id changed while HA is connected.** The old id stays subscribed in HA until the connection drops. Its states must not move the button. Test: `GateLogic.StateForOtherEntityIgnored` (Task 5, via `gate::accept_state`).
2. **HA disconnected, then reconnected.** The button must go disabled at once, then come back with HA's fresh state, never the stale one. Test: `GateLogic.LookDisabledWhenHaDown` (Task 1), and the reconnection check on the tablet (Task 9).
3. **Values typed into the HA text field.** `" cover.portail "`, `"Cover.Portail"`, `"light.x"`, an id of 64 chars or more, `"cover."`, `""`. Expected: trimmed and accepted, rejected (HA ids are lowercase), rejected, rejected, rejected, accepted (= disabled). Tests in Task 2.
4. **Press while the state is unknown or HA is down.** Nothing is sent. Test: `GateLogic.NoActionWhenUnknown` (Task 1); `gate::press` returns false (Task 5).
5. **Gate command during a video call.** No blocking of the LVGL task or `peer_loop` (the action is queued). The camera band repaints within ~200 ms. Checked on the tablet, MJPEG `recv=` unchanged (Task 9).

---

### Task 1: Pure gate logic

**Files:**
- Create: `main/app/gate_logic.h`, `main/app/gate_logic.cpp`
- Create: `test/test_gate_logic.cpp`
- Modify: `test/CMakeLists.txt` (add test file + `${APP_DIR}/app/gate_logic.cpp`), `main/CMakeLists.txt` (SRCS)

**Interfaces:**
- Produces (namespace `gate_logic`, header includes only `<cstdint>`):
  - `enum class State : uint8_t { kUnknown, kUnavailable, kClosed, kOpening, kOpen, kClosing };`
  - `enum class Action : uint8_t { kNone, kOpen, kClose, kStop };`
  - `enum class Tone : uint8_t { kOpen, kClose, kStop, kOff };` (green, blue, orange, grey)
  - `struct Look { const char *label; const char *short_label; const char *hint; Tone tone; bool enabled; };` (`hint` = `""` unless unconfigured)
  - `State parse(const char *ha_state);` (`nullptr` and unknown strings → kUnknown)
  - `Action next_action(State s);`
  - `const char *service_for(Action a);` → `"cover.open_cover"` / `"cover.close_cover"` / `"cover.stop_cover"` / `nullptr`
  - `const char *event_name_for(Action a);` → `"open"` / `"close"` / `"stop"` / `nullptr`
  - `Look look(State s, bool configured, bool ha_connected, bool failed);`
    - Precedence: not configured > HA down > failed > state.

- [ ] **Step 1: Write the failing tests** in `test/test_gate_logic.cpp`:
  - `GateLogic.ParseEveryHaState`:
    - `"open"`→kOpen, `"closed"`→kClosed, `"opening"`→kOpening, `"closing"`→kClosing, `"unavailable"`→kUnavailable;
    - `"unknown"`, `""`, `nullptr`, `"OPEN"`, `"open "` → kUnknown.
  - `GateLogic.NextActionTable`: kClosed→kOpen, kOpen→kClose, kOpening→kStop, kClosing→kStop, kUnknown→kNone, kUnavailable→kNone.
  - `GateLogic.NoActionWhenUnknown`: `service_for(next_action(State::kUnknown)) == nullptr`.
  - `GateLogic.ServiceAndEventNames`: the 3 services and 3 event names above, exact strings.
  - `GateLogic.LookPerState`:
    - kClosed → {"Ouvrir le portail", "Ouvrir", "", kOpen, true};
    - kOpen → {"Fermer le portail", "Fermer", "", kClose, true};
    - kOpening and kClosing → {"Arrêter", "Arrêter", "", kStop, true};
    - kUnknown and kUnavailable → {"Portail indisponible", "Indisponible", "", kOff, false}.
  - `GateLogic.LookDisabledWhenHaDown`: `look(kOpen, true, false, false)` → "Portail indisponible", disabled.
  - `GateLogic.LookFailed`: `look(kClosed, true, true, true)` → {"Échec portail", "Échec", "", kOff, true}.
  - `GateLogic.LookUnconfigured`: `look(kOpen, false, true, true)` → {"Portail", "", "à configurer dans HA", kOff, false}.

- [ ] **Step 2: Run tests, expect build FAIL** (`gate_logic.h` missing): `cd test ; pwsh -NoProfile -File run.ps1`

- [ ] **Step 3: Implement** `gate_logic.cpp`: string compare with `strcmp`, `switch` tables. String literals are returned (static storage).

- [ ] **Step 4: Run tests** → `100% tests passed`. Then mutate (swap kOpen/kClose in `next_action`), confirm `NextActionTable` fails, and restore.

- [ ] **Step 5: Commit** `feat(gate): pure gate state/action/look logic`

---

### Task 2: Entity id validation (pure)

**Files:**
- Create: `main/app/ha_entity_id.h`, `main/app/ha_entity_id.cpp`, `test/test_ha_entity_id.cpp`
- Modify: `test/CMakeLists.txt`, `main/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `namespace ha_entity_id { inline constexpr size_t kMax = 64; bool normalize(const char *in, const char *domain, char *out, size_t cap); }`
  - Trims ASCII spaces. Empty after the trim → `out = ""`, returns true.
  - Otherwise valid iff the value is `<domain>.<object>`, where `<object>` is non-empty `[a-z0-9_]` and the total length is `< kMax`. `cap` must be `>= kMax`.
  - On false, `out` is left untouched.

- [ ] **Step 1: Write the failing tests** `HaEntityId.*`:
  - `AcceptsAndTrims`: `" cover.portail "` with `"cover"` → true, `"cover.portail"`.
  - `EmptyMeansUnset`: `""` and `"   "` → true, `""`.
  - `RejectsWrongDomain`: `"light.x"` with `"cover"` → false; `"coverx.y"` with `"cover"` → false.
  - `RejectsUppercaseAndBadChars`: `"Cover.Portail"`, `"cover.port ail"`, `"cover.port-ail"` → false.
  - `RejectsNoObject`: `"cover."`, `"cover"` → false.
  - `RejectsTooLong`: `"cover."` + 58 × `a` (64 chars) → false; 57 × `a` (63 chars) → true.
  - `KeepsOutOnFailure`: `out` pre-filled `"keep"`, input `"light.x"` → still `"keep"`.
- [ ] **Step 2: Run, expect FAIL.**
- [ ] **Step 3: Implement** `normalize`.
- [ ] **Step 4: Run** → all pass. Mutate (drop the domain check), see `RejectsWrongDomain` fail, restore.
- [ ] **Step 5: Commit** `feat(config): validate HA entity ids typed in Home Assistant`

---

### Task 3: Entity ids in prefs; weather and calendar read them

**Files:**
- Modify: `main/sys/prefs.h`, `main/sys/prefs.cpp`
- Modify: `main/app_config.h` (`APP_SECRET_HA_GATE_ENTITY`, default `""`; `cfg::kHaGateEntity`)
- Modify: `main/app_secrets.example.h` (add `#define APP_SECRET_HA_GATE_ENTITY "cover.my_gate"` with a comment)
- Modify: `main/net/weather.cpp`, `main/net/gcal.cpp` (read the id per fetch)
- Modify: `main/ui/ui_weather_v3.cpp` (refetch when the generation changes)

**Interfaces:**
- Consumes: `ha_entity_id::kMax` (Task 2).
- Produces (namespace `prefs`):
  - `enum class HaEntity : uint8_t { kGate, kWeather, kCalendar };`
  - `void ha_entity(HaEntity which, char *out, size_t cap);`: copy under the prefs mutex. Safe from any task.
  - `void set_ha_entity(HaEntity which, const char *normalized);`: stores to NVS and bumps the generation. The caller has already validated the value.
  - `uint32_t ha_entities_generation();`: `std::atomic<uint32_t>`, +1 on each `set_ha_entity`.
- NVS keys in `ui_prefs`: `ent_gate`, `ent_weather`, `ent_cal`.
  - Absent key → defaults: `cfg::kHaGateEntity`, `cfg::kHaWeatherEntity`, `cfg::kHaCalendarEntity`.
  - A stored `""` is honoured (explicitly unset).

- [ ] **Step 1: Implement prefs storage.** Follow the `ring_file` pattern (static buffer `char[ha_entity_id::kMax]`, `nvs_get_str` at `init`, `nvs_set_str` + `nvs_commit` on set). Add a mutex for the three buffers, because readers are fetch tasks and the writer is the API task.
- [ ] **Step 2: Switch `weather.cpp` / `gcal.cpp`** to `prefs::ha_entity(...)` into a local `char[ha_entity_id::kMax]` at the start of each `do_fetch`, for both the `entity_id` Kv and the template. An empty id → the fetch fails at once with `ESP_LOGW(TAG, "entité non configurée")`.
- [ ] **Step 3: Refetch on change.** In `ui_weather_v3`'s existing net timer, compare `prefs::ha_entities_generation()` with the last seen value. If it changed, call the same refetch as the HA 0→1 transition (`refresh_timer_cb(nullptr)`).
- [ ] **Step 4: Build firmware** → `Project build complete`. Host tests still `100% tests passed`.
- [ ] **Step 5: Commit** `feat(config): gate/weather/calendar entity ids stored in prefs`

---

### Task 4: API server — text entities, HA state import, actions without response, events

**Files:**
- Modify: `main/net/esphome_api.h`, `main/net/esphome_api.cpp`

**Interfaces:**
- Produces:
  - **Text entities:** `Kind::kText`, and `uint8_t max_len = 64;` in `Entity` (declared after `update_cmd`). Read through `get_text` (state 98) and `set_text` (command 99).
    - List 97: `object_id` 1, `key` 2, `name` 3, `icon` 5, `entity_category` 7, `max_length` 9 = `max_len`, `mode` 11 = 0.
    - State 98: `key` 1, `state` 2 (always written, empty allowed: use `bytes_always`).
  - **HA state import:**
    - `inline constexpr size_t kMaxHaWatches = 4;` and `void watch_ha_state(uint8_t slot, const char *entity_id);`
      - It copies the id (≤ 63 chars) under a portMUX and marks it pending.
      - The server task sends a 39 (`entity_id` 1 only) to every client with `sub_ha_states`.
      - An empty id clears the slot; nothing is sent.
    - On a client's 38: set `sub_ha_states = true` (new `Client` field, reset on close), then send a 39 for each non-empty slot.
    - `Config::on_ha_state`, of type `void (*)(const char *entity_id, const char *attribute, const char *state)`, default nullptr. It is called from the server task on each 40, with NUL-terminated copies (≤ 63 / 63 / 127 chars, truncated).
  - **Actions without response:** `call_action(service, data, n, response_template, done, ctx, bool wants_response = true)`.
    - When false: field 7 and field 8 are not written, `call_id` is still set.
    - `done` gets `(ok, error, nullptr, 0)` on HA's 130, or the existing 20 s timeout.
    - `call_action_sync` is unchanged (it always wants a response).
  - **Events:** `bool fire_event(const char *name, const Kv *data, size_t n);`
    - It returns false unless `name` starts with `"esphome."`.
    - It sends 35 with `is_event` = true (field 5) and no `call_id`, through the same queue; there is no callback. The queue entry's `done` is nullptr, and the server must not register it as pending.

- [ ] **Step 1: Implement text entities**: `encode_list_entity`, `encode_state`, the `on_command` branch for 99 (field 2 → `text`; widen the `text` buffer to 128), and the dispatch case 99.
- [ ] **Step 2: Implement HA state import** (`kSubscribeHaStates` case, `kHaStateResponse = 40`, `kSubscribeHaStateResponse = 39`, pending-slot flush in the server loop).
- [ ] **Step 3: Implement `wants_response` and `fire_event`.** `fire_event` enqueues a `QueuedAction` with `done = nullptr`. The sender skips the pending registration when `done == nullptr`.
- [ ] **Step 4: Build firmware** → `Project build complete`. Host tests unchanged and green.
- [ ] **Step 5: Commit** `feat(api): text entities, HA state import, actions without response, events`

---

### Task 5: `app/gate` — live state and press

**Files:**
- Create: `main/app/gate.h`, `main/app/gate.cpp`
- Modify: `main/app/gate_logic.h`, `main/app/gate_logic.cpp`, `test/test_gate_logic.cpp` (add `accept_state`)
- Modify: `main/CMakeLists.txt`

**Interfaces:**
- Consumes: Task 1 (`gate_logic::*`), Task 3 (`prefs::ha_entity`), Task 4 (`watch_ha_state`, `call_action(..., false)`, `fire_event`, `connected()`).
- Produces:
  - `bool gate_logic::accept_state(const char *watched_id, const char *entity_id, const char *attribute);` → true iff `watched_id` is non-empty, both ids are equal, and `attribute` is empty or nullptr.
  - In namespace `gate`:
    - `inline constexpr uint8_t kWatchSlot = 0;`
    - `void init();`: reads `prefs::ha_entity(kGate)` and calls `set_entity`.
    - `void set_entity(const char *normalized_id);`: stores the id (under a mutex), sets the state to kUnknown, calls `watch_ha_state(kWatchSlot, id)`, bumps the version.
    - `void on_ha_state(const char *entity_id, const char *attribute, const char *state);`: if `accept_state(...)`, stores `parse(state)` (`std::atomic<gate_logic::State>`) and bumps the version.
    - `bool press();`
      - It computes `next_action(state)`. If it is kNone, or `!esphome_api::connected()`, or the id is unset → false.
      - Otherwise `call_action(service_for(a), {{"entity_id", id}}, 1, nullptr, on_done, nullptr, false)`.
      - `on_done` (server task):
        - ok → `fire_event("esphome.visiophone_gate", {{"action", event_name_for(a)}, {"during_call", camera_session::active() ? "true" : "false"}}, 2)`;
        - error → `ESP_LOGW` + `failed_until_us = now + 3 s`.
        - It bumps the version in both cases.
    - `gate_logic::Look look();` → `gate_logic::look(state, id set, esphome_api::connected(), now < failed_until_us)`.
    - `uint32_t version();`: `std::atomic<uint32_t>`. The UI polls it.
      - Also bumped by a 1 s check when `connected()` flips, and when `failed_until_us` expires, so the UI reflects both.

- [ ] **Step 1: Write the failing test** `GateLogic.StateForOtherEntityIgnored`:
  - `accept_state("cover.a", "cover.b", "")` → false;
  - `accept_state("cover.a", "cover.a", "")` → true;
  - `accept_state("cover.a", "cover.a", "current_position")` → false;
  - `accept_state("", "", "")` → false.
- [ ] **Step 2: Run, expect FAIL**; implement `accept_state`; run → pass.
- [ ] **Step 3: Implement `gate.cpp`** as specified. The 1 s connection/expiry check is an `esp_timer` (periodic, 1 s) that only compares and bumps atomics.
- [ ] **Step 4: Build firmware** → OK.
- [ ] **Step 5: Commit** `feat(gate): live cover state from HA and press -> cover action + event`

---

### Task 6: Wire the HA side (`app/ha_api`, `main.cpp`)

**Files:**
- Modify: `main/app/ha_api.cpp`, `main/main.cpp`

**Interfaces:**
- Consumes: Tasks 2-5.
- Produces:
  - three `Kind::kText` entities (Global Constraints names/ids, category kConfig, icons `mdi:gate`, `mdi:weather-partly-cloudy`, `mdi:calendar`);
  - `s_cfg.on_ha_state = gate::on_ha_state;`.
  - `set_text` for each: `ha_entity_id::normalize(in, "cover" | "weather" | "calendar", buf, sizeof buf)`.
    - On true: `prefs::set_ha_entity(...)`, and for the gate also `gate::set_entity(buf)`.
    - On false: `ESP_LOGW("entité refusée: %s", in)` and change nothing.
    - In both cases the existing echo (`push_state` after a command) re-sends the stored value, so HA's field snaps back on refusal.
  - `get_text` → `prefs::ha_entity(...)`.
  - `gate::init()` is called in `main.cpp` after `ha_api::init` (the slot then exists before HA's first 38).

- [ ] **Step 1: Implement** the entities and the wiring.
- [ ] **Step 2: Build firmware** → OK; host tests green.
- [ ] **Step 3: Commit** `feat(ha): gate/weather/calendar entity ids editable from the HA device page`

---

### Task 7: UI — shared gate button, home tile, weather header

**Files:**
- Modify: `main/ui/ui.h` (`ui::Callbacks` gains `bool (*gate_press)(); gate_logic::Look (*gate_look)(); uint32_t (*gate_version)();`, and includes `"app/gate_logic.h"`, which is pure)
- Create: `main/ui/gate_style.h`, `main/ui/gate_style.cpp`: `uint32_t gate_tone_color(gate_logic::Tone t);` mapped from `ui::theme::active` (green/blue/orange/grey; add theme fields `gate_open`, `gate_close`, `gate_stop`, `gate_off` to both palettes)
- Modify: `main/ui/ui.cpp`:
  - grid becomes 4×2: `kTileW` = 282, `kTileGap` = 24, `kTileLeft` = 48 → 48 + 4×282 + 3×24 = 1248 ≤ 1280;
  - order: Caméra, Météo, Agenda, Portail / Appels, Configuration, Éteindre;
  - the Portail tile shows `look.label`, plus `look.hint` on a second line when non-empty; colour = tone; disabled when `!look.enabled`;
  - tap → `cbs.gate_press()`.
- Modify: `main/ui/ui_weather_v3.cpp`: the gate button goes in `hright` before `clock_badge`, height 72, showing `look.short_label`. It is hidden while the gate id is unset (`look.hint[0] != '\0'`).
- Modify: `main/main.cpp`: fill the three callbacks with `gate::press`, `gate::look`, `gate::version`.

- [ ] **Step 1: Implement.** Each screen polls `cbs.gate_version()` in its existing LVGL timer (home: `net_timer_cb`; weather: its net timer) and repaints the button only when the version changed. No `lv_*` call happens outside these timers and event callbacks.
- [ ] **Step 2: Build firmware** → OK.
- [ ] **Step 3: Commit** `feat(ui): gate button on the home screen and the weather header`

---

### Task 8: UI — camera band (stats into the state chip, gate button)

**Files:**
- Modify: `main/ui/ui_camera_v2.cpp`

**Interfaces:**
- Consumes: Task 7 (`ui::Callbacks` gate fields, `gate_tone_color`).
- Produces:
  - `s_stats_label` moves to a child of `s_state_chip`, as a 2nd line, `montserrat_fr_16`.
    - With stats on, the chip is 64 px tall and its 1st line moves up. With stats off, it is 48 px and its single line is centred, as today.
    - `update_stats` writes to the same label, which is still hidden when stats are off.
  - Gate button: `make_button(s_band, ..., x = 1018, w = 250, ...)`, right edge 12 px from the screen.
    - Re-space within 1280: back 12/80, speaker slider 104/170, Answer 286/250, chip 548/300, mic slider 860/146, gate 1018/250.
    - `band_track(gate_btn)`.
  - A `gate_version()` change seen in `band_timer_cb` repaints the label and colour, then sets `s_band_dirty = true`. The repaint must not wait for the 5 s stats throttle: apply the throttle to stats text only.

- [ ] **Step 1: Implement.**
- [ ] **Step 2: Build firmware** → OK.
- [ ] **Step 3: Commit** `feat(ui): gate button in the camera band; stats move into the state chip`

---

### Task 9: Docs, flash, verify on the tablet

**Files:**
- Modify: `docs/server-setup.md`:
  - a new "Gate" section: set "Entite portail" on the device page; the actions option is needed;
  - the event `esphome.visiophone_gate` with an automation example:
    ```yaml
    trigger: {trigger: event, event_type: esphome.visiophone_gate}
    action: {action: notify.notify, data: {message: "Portail {{ trigger.event.data.action }} depuis la tablette"}}
    ```
  - the entity list.
- Modify: `docs/getting-started.md`: entity ids are now set in HA; `app_secrets.h` only gives first-boot defaults.
- Modify: `README.md` feature list: gate control.
- Modify: local git-ignored `main/app_secrets.h`: add `APP_SECRET_HA_GATE_ENTITY "<your cover.* id>"` (never committed).

- [ ] **Step 1: Write docs.** Commit `docs: gate control and entity ids from Home Assistant`.
- [ ] **Step 2: Build** with `idf.py reconfigure build`, then OTA via the file server (`/api/up` + `/api/ota`; tablet at the IP shown by HA's "Adresse IP" sensor). Confirm the SD log shows `BOOT … app=<new hash>`.
- [ ] **Step 3: Verify on the tablet:**
  - open, close and stop from home, weather and camera (during a call opened with "Caméra sans sonnerie");
  - the state follows a command made from the HA app;
  - set the text field to `light.x`: the field snaps back;
  - set it to an empty value: the weather button is hidden and the tile is disabled;
  - restore the id;
  - restart HA: the button greys out, then comes back with the live state;
  - in the SD log, MJPEG `recv=` stays unchanged during a gate command in a call.
- [ ] **Step 4: Push** the branch (Gluxur token, `GH_TOKEN=$(gh auth token -u Gluxur)`), then wait for CI green on both workflows.
