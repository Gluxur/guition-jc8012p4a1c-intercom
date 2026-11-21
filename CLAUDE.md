# CLAUDE.md — Project rules (ESP32-P4 video intercom)

Instructions for anyone changing this code — humans and AI coding assistants
alike. Target: **ESP32-P4** (RISC-V dual-core, PSRAM) + **ESP-IDF v6.0.1**.
Wi-Fi comes from the ESP32-C6 through `esp_wifi_remote` / `esp_hosted`.

These rules are **mandatory**. When in doubt about an API, **open the real
header in `managed_components/` before writing code. Never invent a function
or type name.**

Background reading: [docs/architecture.md](docs/architecture.md) (how it fits
together) and [docs/hardware-notes.md](docs/hardware-notes.md) (measured
pitfalls — most "arbitrary" constants are justified there).

---

## 1. Architecture and includes (C++)

- **Strict separation**: all hardware logic lives in `components/bsp`
  (singleton `bsp::Bsp`, `include/bsp.h` / `bsp.cpp`). Application logic is
  split by layer under `main/` (`app/`, `ui/`, `net/`, `media/`, `sys/`).
  `main.cpp` only **wires** things. New responsibility = new module in the
  folder of its layer, never loose global functions or code added to `main.cpp`.
- Cross-folder includes are qualified (`#include "net/weather.h"`); inside a
  folder, short includes. `INCLUDE_DIRS` only contains `.`.
- `#pragma once` at the top of every header. Minimal includes in headers,
  forward declarations when a pointer/reference is enough.
- **Never `#include` inside a class or namespace.**
- **`extern "C"`**: never wrap ESP-IDF includes (they have their own guards).
  Only for C-ABI definitions or assembler symbols (e.g. the embedded Lottie
  `_binary_*_json_start`), placed after the includes, never inside a namespace.
- **File integrity**: never truncate a file while editing. `bsp.h` must end
  with `};` then `} // namespace bsp`. Check brace balance after editing any
  header.
- `bsp::Bsp::instance()`; copy/move deleted; `init()` is `[[nodiscard]]`.
- Every new `.cpp` in `main/` goes explicitly into `SRCS` of
  `main/CMakeLists.txt` (no glob). New dependencies go into
  `main/idf_component.yml` **and** `REQUIRES` of the relevant `CMakeLists.txt`.
  `bsp` has `PRIV_REQUIRES`: a new include in `bsp.h` may need a public
  `REQUIRES`.
- **Never edit `managed_components/`** (managed by `idf_component.yml` +
  `dependencies.lock`). Vendored drivers (`esp_lcd_jd9365`,
  `esp_lcd_touch_gsl3680`): only for a proven driver bug.

## 2. Hardware rules (summary — details in docs/hardware-notes.md)

- Video = **MJPEG + hardware JPEG decoder**. Do not reintroduce H.264 (no
  hardware decoder on the P4) without a measurement proving 25 fps.
- JPEG engine created **at boot** (`video_mjpeg::init()`); buffers aligned on
  16 px; `rgb_order = BGR`.
- Colour conversion, rotation, scaling: **PPA**, never CPU loops. Never
  `PPA_TRANS_MODE_BLOCKING`. Two PPA clients, never shared between tasks.
- Big buffers (> 4 KB) in **PSRAM**, 64-byte aligned for DMA/PPA. Internal SRAM
  is reserved for what needs it (DTLS, DMA descriptors, LVGL draw buffers).
- Never block `esp_peer_main_loop()` on rendering or on the LVGL mutex.
- Core 0 = MJPEG TCP drain; core 1 = `peer_loop` (prio 5), `mjpeg_rnd` (4),
  `aud_render` (6). Don't move affinities without measuring `recv`/`drop`/KB/s.
- Microphone: `Bsp::read_pcm()`, speaker: `Bsp::play_pcm()`.
  `play_audio()` / `record_audio()` **do not exist**.
- No SD I/O on a hot path (shared SDMMC controller with Wi-Fi). Never call
  `host.deinit` on the SDMMC host.
- Keep: `WIFI_PS_NONE`, flash **DIO**, L2 cache line **64 B**, FreeRTOS tick
  1000 Hz, `kLcdVfp = 120`, LVGL `RENDER_MODE_PARTIAL`, PSRAM XIP, LVGL PSRAM
  allocator. Never propose 400 MHz (hidden on this pre-v3 silicon).
- **Never run `idf.py set-target`** (resets `sdkconfig`). `sdkconfig.defaults`
  does not override an option already in `sdkconfig`.

### Real esp_peer API (v1.2.x) — do not invent

- Types: `esp_peer_cfg_t`, `esp_peer_default_cfg_t`, `esp_peer_handle_t`,
  `esp_peer_state_t`, `esp_peer_audio_frame_t`, `esp_peer_video_frame_t`.
- Functions: `esp_peer_open()`, `esp_peer_new_connection()`,
  `esp_peer_main_loop()`, `esp_peer_close()`, `esp_peer_send_audio()`,
  `esp_peer_send_msg()`, `esp_peer_set_rtp_transformer()`,
  `esp_peer_get_default_impl()`.
- **Do not exist**: `esp_peer_config_t`, `esp_peer_create()`,
  `esp_peer_delete()`, `esp_peer_get_local_sdp()`,
  `esp_peer_rtp_transform_register()`, `ESP_PEER_CONN_TYPE_*`.
- Signalling: HTTP POST of the SDP offer to `/api/webrtc?src=...`, the answer
  is the response body. One module, `net/signaling`. Each peer connection
  (`camera_session`, `mic_session`) owns its own `signaling::Channel`,
  semaphore and stop flag — a shared buffer would race.

## 3. Coding practices

- `constexpr` for hardware constants, `std::atomic` for inter-task flags,
  private members suffixed `_`, light `[[nodiscard]]` accessors.
- **RAII**: `std::unique_ptr` with custom deleters for C handles. Mutexes:
  `bsp::Bsp::LvglLock` and `sys::MutexLock` (`main/sys/mutex.h`) — never
  manual `xSemaphoreTake`/`Give` around a block with several exits.
- **Comments explain the why only**: hardware constraint, SRAM vs PSRAM, core
  choice, ABI, measurement. Never paraphrase the code. Existing comments are in
  French and record costly diagnoses: keep them.
- **Errors**: assertions are disabled for performance — **never
  `ESP_ERROR_CHECK()`**. Check return values, `ESP_LOGE`, release resources.
- **No `ESP_LOGI`/`printf` on hot paths** (touch callback, per frame, per RTP
  packet): UART saturation caused lag before. Use `ESP_LOGD` (compiled out:
  `CONFIG_LOG_MAXIMUM_LEVEL=3`).
- **FreeRTOS**: tasks pinned with `xTaskCreatePinnedToCore`, keep stack sizes,
  stop via `std::atomic<bool>` + a `*_task_stopped` semaphore. Never
  `volatile bool` as an inter-task flag. **Never `vTaskDelete` a task that may
  be blocked in a driver** (I2S). Any task touching FATFS or NVS: ≥ 8 KB stack.
- ISR callbacks: `IRAM_ATTR`, no `ESP_LOG*`, only `...FromISR()` APIs.
- **LVGL**: every `lv_*` call outside the LVGL task holds `bsp::Bsp::LvglLock`,
  **in an explicit block** — a one-shot task ending in `vTaskDelete(nullptr)`
  runs no destructor, and the mutex would stay held forever (frozen screen,
  logs still flowing). During streaming `direct_mode` is on: the flush is a
  no-op and `present_frame()` does not take the LVGL mutex.
- Sliders (volume, gain, brightness): applied on every change, **persisted in
  NVS on release only**.

## 4. Configuration

- `main/app_config.h` (namespace `cfg`): compiled defaults (`cfg::defaults::*`,
  filled from the git-ignored `main/app_secrets.h` if present) and deployment
  constants (ports, go2rtc stream names, HA entities).
  `inline constexpr`, no config `#define` anywhere else.
- `cfg::defaults::*` is read **only** by `sys/settings.cpp`. Everywhere else:
  `settings::…`. URLs are formatted once in `settings::init()`; modules never
  compose a URL.
- `settings` (network, NVS `net_cfg`, applied at reboot) and `prefs` (comfort,
  NVS `ui_prefs`, applied live) are two distinct contracts. Do not merge them.
- **Home Assistant link = ESPHome native API** (`net/esphome_api`, catalogue in
  `app/ha_api.cpp`). No MQTT, no HA token. HA data (weather, calendar) is read
  through HA actions with a response template; a template change must keep
  `test/ha_templates/check_templates.py` green. Keep the announced API version
  at 1.13 unless the server implements DeviceCapabilitiesRequest (1.15+).
  Changing an entity's `object_id` creates a new entity in HA.
- **Never commit secrets.** No Wi-Fi password, token, IP-specific credential
  or camera URL in tracked files.
- The init order in `app_main` is deliberate; each step has its reason written
  in place. Read it before moving anything.

## 5. Tests

- **`test/` (host, GoogleTest)**: pure logic only. Stubs limited to
  `esp_log.h`, `esp_timer.h` (test-driven clock), `strlcpy`. If a module needs
  more, split it (`net/<x>.cpp` I/O vs `net/<x>_parse.cpp` decoding). Add new
  sources to `SRCS` in `test/CMakeLists.txt` (no glob).
- **`test_apps/` (on target, Unity)**: what only exists on silicon (JPEG, PPA,
  DSI, ES8311). `WHOLE_ARCHIVE` is mandatory in `test_apps/main/CMakeLists.txt`.
- Test vectors are **never** generated by running the code under test.
- A suite that passes on the first run proves nothing: mutate the
  implementation, check the expected tests fail, restore.
- No boot-time `self_test()` for pure logic.

## 6. Change discipline

- Surgical changes. Do not rewrite a module "cleanly" without an explicit
  request: its comments hold diagnoses a rewrite would lose.
- Every API you call must exist in a header of the project or of
  `managed_components/`.
- Performance claims need a measurement (`MJPEG` and `CPU:` log lines), not an
  intuition.
