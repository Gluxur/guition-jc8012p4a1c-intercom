# Gate control from the tablet — design

Date: 2026-11-22. Target: v2.0.0. Status: awaiting approval.

## Intent

Open and close the gate (`cover.*` entity in Home Assistant) from the tablet:
during a call, to let the visitor in, and outside a call, e.g. for a delivery
after hanging up. The cover's state in HA is reliable (open / closed / moving,
in real time), so the tablet shows it and one button does the right thing.

Same change: the HA entity ids the tablet uses (gate, and the existing weather
and calendar ids) become editable from the device page in Home Assistant. On
2026-11-22 a wrong compiled-in weather id left the weather page stuck on
"Chargement"; fixing it took a rebuild.

## Goals

- One gate button on three screens: camera band (during a call), weather
  screen (idle screen), home screen (new tile).
- The label and colour follow the live cover state.
- Gate, weather and calendar entity ids are text fields on the HA device page.
  A change applies without reboot.
- HA gets an event when the gate is operated from the tablet.

## Non-goals

- Doorbell trigger by the tablet itself (idea 2, dropped: the door station
  fires a HA *event*, which the ESPHome API cannot subscribe to).
- "Call answered / missed" events (idea 3): the event plumbing is built here,
  those two events come later.
- Long-press or confirmation dialog: the user explicitly does not want one.
- Several gates / covers, position slider, garage door.

## Anti-goals

- A button that shows a state the gate is not in. If the state is unknown,
  say so; never guess from the last command.
- Any blocking of the MJPEG drain or of `peer_loop` while the gate is
  operated during a call.
- Settings that need a rebuild or a reboot to change an entity id.

## Constraints (from the project)

- HA link = our ESPHome native API server (`net/esphome_api`), API 1.13.
- Camera screen runs in `direct_mode`: only the 80 px band is repainted by
  LVGL (`band_blit`), recomposed at most every 5 s for stats. The band is
  full: Back (12..92), speaker slider (108..288), Answer (304..604), state chip
  (620..910), mic slider (926..1086), stats label (right, 170 px).
- Home screen: 3×2 grid of 378×300 tiles, full.
- Weather screen header, right block: Wi-Fi + HA indicators, clock badge
  (72 px high), back button (72×72).
- `prefs` = comfort settings applied live (namespace `ui_prefs`); `settings` =
  network, effective at reboot. Entity ids are applied live → `prefs`.
- UI callbacks to `app/` go through `ui::Callbacks` (plain function pointers).
- Large buffers in PSRAM; the API server task has a 12 KB PSRAM stack.

## Protocol facts (verified against esphome 2025.10 api.proto and HA core dev)

- **State import.** HA sends `SubscribeHomeAssistantStatesRequest` (38) after
  connecting. The device answers with one `SubscribeHomeAssistantStateResponse`
  (39: `entity_id` 1, `attribute` 2, `once` 3) per entity it wants. HA pushes
  the current state at once, then each change, as `HomeAssistantStateResponse`
  (40: `entity_id` 1, `state` 2, `attribute` 3). It needs no HA option. A
  subscription cannot be cancelled; it ends with the connection. An entity
  that does not exist sends nothing.
- **Actions without response.** `HomeassistantActionRequest` (35) with
  `call_id` set and `wants_response` false: HA runs the action and answers
  `HomeassistantActionResponse` (130) with success / error, no data. With
  `wants_response` true, an action that returns no data (`cover.open_cover`)
  fails. Needs "Allow the device to perform Home Assistant actions" (already
  required for weather).
- **Events.** Same message with `is_event` = true (field 5) and a service named
  `esphome.<name>`: HA fires that event on its bus with the device id added.
  Needs no option.
- **Text entity.** `ListEntitiesTextResponse` 97 (`object_id` 1, `key` 2,
  `name` 3, `icon` 5, `entity_category` 7, `min_length` 8, `max_length` 9,
  `pattern` 10, `mode` 11), `TextStateResponse` 98 (`key` 1, `state` 2,
  `missing_state` 3), `TextCommandRequest` 99 (`key` 1, `state` 2).
- Cover states: `open`, `closed`, `opening`, `closing`, plus `unavailable` /
  `unknown`.

## Approaches considered

**Where the entity id is configured**

1. Text entity on the HA device page, stored in NVS, applied live — **picked**.
   Editable where the user already manages entities; no on-screen keyboard;
   also fixes the weather/calendar id trap.
2. Field on the tablet's Configuration page: a long id typed on the touch
   keyboard, and `settings` applies at reboot only.
3. `app_secrets.h` at build time (today's weather/calendar mechanism): every
   change needs a rebuild.

**Where the gate is operated outside a call**

1. Button on the weather screen (the idle screen, one tap).
2. Seventh tile on the home screen.
3. Both — **picked** by the user.

## Design

### 1. API server (`net/esphome_api`)

- **`Kind::kText`**: list (97) with `max_length` (new `Entity::max_len`,
  default 64), `mode` TEXT, `category` CONFIG; state (98) via the existing
  `get_text`; command (99) via the existing `set_text`. Empty text is a valid
  state (not `missing_state`).
- **HA state import**:
  - `void watch_ha_state(uint8_t slot, const char *entity_id)` with up to 4
    slots, copied. It records the id and sends a 39 to every client that has
    sent 38. An empty id clears the slot (nothing sent).
  - On a client's 38: set `sub_ha_states`, then send a 39 for each non-empty
    slot.
  - On a 40: call `Config::on_ha_state(entity_id, attribute, state)` in the
    server task.
  - A replaced id stays subscribed in HA until the connection drops. The app
    ignores states for ids that are no longer watched (cheap, and avoids
    forcing a reconnect).
- **Action without response**: `call_action` gains a `wants_response` flag
  (default true, today's behaviour). Without it, `done` still gets exactly one
  callback (success / error / 20 s timeout), with no body.
- **Event**: `bool fire_event(const char *name, const Kv *data, size_t n)`
  sends 35 with `is_event`. `name` must start with `esphome.`. Fire and forget.

### 2. Entity ids (`sys/prefs` + `app/ha_api`)

- Three new `prefs` strings, max 63 chars + NUL each: `gate_entity`,
  `weather_entity`, `calendar_entity`. Applied live, behind the prefs mutex.
- The first boot without them falls back to the compiled defaults:
  `cfg::kHaWeatherEntity`, `cfg::kHaCalendarEntity` (still overridable in
  `app_secrets.h`), and `APP_SECRET_HA_GATE_ENTITY` (default empty = no gate).
  An installation that already works keeps working after the update.
- Three text entities, category CONFIG: "Entité portail", "Entité météo",
  "Entité agenda".
  - `set_text` trims spaces and accepts `""` or `<domain>.<object_id>` with the
    expected domain (`cover.`, `weather.`, `calendar.`). Otherwise the value is
    rejected: the stored value is kept and its state re-pushed, so HA's field
    snaps back.
- `weather` / `gcal` read the id from `prefs` at each fetch (instead of
  `cfg::`). A change triggers an immediate refetch.
- A gate id change calls `gate::set_entity()` (below).

### 3. Gate logic (`app/gate` + pure `app/gate_logic`)

- `gate_logic` is pure and host-tested:
  - `State parse(const char *ha_state)`: kOpen, kClosed, kOpening, kClosing,
    kUnavailable, kUnknown.
  - `Action next_action(State)`: closed → open; open → close;
    opening/closing → stop; unknown/unavailable → none.
  - Button label and colour per state:
    - "Ouvrir le portail" (green);
    - "Fermer le portail" (blue);
    - "Arrêter" (orange) while moving;
    - "Portail indisponible" (grey, disabled) when unknown, unavailable, HA
      disconnected, or the id is not set.
- `gate` (firmware only):
  - It holds the current id and an `std::atomic<State>`.
  - `set_entity(id)` → `esphome_api::watch_ha_state(kSlotGate, id)` and resets
    the state to kUnknown until HA answers.
  - The `on_ha_state` hook (wired in `ha_api`) updates the state if the id
    matches, then notifies the UI (posts a refresh; never `lv_*` from the API
    task).
  - `press()`: computes `next_action`, then calls
    `call_action("cover.open_cover" | "cover.close_cover" | "cover.stop_cover",
    {entity_id}, wants_response=false)`. On success it fires the event
    `esphome.visiophone_gate` with `{action: open|close|stop, during_call:
    true|false}`.
  - On error: log it, and show "Échec portail" for 3 s on the button.
  - On HA disconnect: state → kUnknown (button disabled).
  - It never blocks the caller: the UI calls `press()` from the LVGL task, and
    the work is queued to the API task.

### 4. UI

- **Camera band**:
  - The stats label leaves the band. Stats go to a second line, 16 px, inside
    the state chip (shown only when the "stats overlay" setting is on; the
    chip becomes 2 lines tall in that case).
  - The gate button takes the freed right-hand slot: ~250 px, same 64 px style
    as Answer. Sliders and chip are re-spaced if needed.
  - The button is part of `band_track`, so `band_blit` repaints it.
  - A state change marks the band dirty. It must reach the screen within
    ~200 ms, not wait for the 5 s stats recomposition.
- **Weather screen**: a gate button in the header's right block, before the
  clock badge, 72 px high (icon + short label "Ouvrir" / "Fermer" /
  "Arrêter"). It is hidden when no gate id is set.
- **Home screen**:
  - The grid goes to 4×2 tiles, about 280 px wide, same height. Order: Caméra,
    Météo, Agenda, Portail / Appels, Configuration, Éteindre.
  - The Portail tile shows the live label and colour.
  - When no gate id is set, the tile stays visible but disabled, with the hint
    "à configurer dans HA".
- All three share one helper fed by `gate_logic` (label, colour, enabled), so
  they never disagree.

### 5. Docs

- `docs/server-setup.md`: the three text entities; the gate needs the actions
  option; the event `esphome.visiophone_gate` with an automation example
  (notify "Portail ouvert depuis la tablette").
- `docs/getting-started.md`: entity ids now set from HA; `app_secrets.h` only
  gives first-boot defaults.

## Tests

- **Host**:
  - `gate_logic`: parse of every HA state, including unexpected strings;
    `next_action` table; label/colour/enabled table.
  - Entity id validation (domain check, trim, empty, too long).
  - Encoding of 39, 97/98 and the `is_event` 35, if the encoders are split out
    pure like `esphome_pb`.
- **On the tablet**:
  - Open / close / stop from each of the three screens, during a call and
    outside.
  - The state follows a command made from the HA app.
  - Change the id in HA and check the switch is live.
  - Set a wrong id: the button is disabled.
  - With HA disconnected: the button is disabled.
  - Check the MJPEG `recv=` rate during a gate command in a call
    (no regression).

## Left to the builder

- Exact pixel positions in the band and the weather header, within the rules
  above.
- Colours: from the existing theme (`ui::theme::active`).
- How the UI is notified (LVGL timer polling an atomic version counter, or an
  `lv_async_call`), as long as no `lv_*` runs outside the LVGL task.
- Whether the HA-side ids live in `prefs` or in a new small `ha_ids` module.
  `prefs` is the default.
