# ESP32-P4 Video Intercom

Indoor monitor firmware for an IP door station, running on the **Guition
JC8012P4A1C** ESP32-P4 touch panel (800×1280 MIPI-DSI). When someone rings, the
screen wakes up on the door camera, you hear the street, tap **Answer** and talk
back. The rest of the time it is a weather / calendar wall display.

Video and audio come from **[go2rtc](https://github.com/AlexxIT/go2rtc)**;
doorbell trigger, weather, calendar and supervision come from **Home
Assistant** (MQTT + REST API). Everything stays on your LAN, no cloud.

- **Target**: ESP32-P4 (dual-core RISC-V @ 360 MHz, 32 MB PSRAM). The P4 has no
  radio: Wi-Fi goes through the on-board ESP32-C6 (`esp_hosted` over SDIO).
- **SDK**: ESP-IDF **v6.0.1**, C++, LVGL 9.
- **Status**: in daily use on one installation. Shared as-is, as a working
  reference for anyone building an intercom on this board.

> The on-screen UI and code comments are in **French**; all documentation is in
> English. The comments explain *why* (hardware constraints, measurements) and
> are worth running through a translator before changing a magic number.

---

## Features

**Calls**
- MQTT doorbell trigger → camera screen opens + ringtone.
- Full-screen MJPEG video at 15-25 fps, decoded by the P4's **hardware JPEG
  engine** straight into the display framebuffer (zero copy).
- Incoming audio over WebRTC (G.711 A-law); **Answer** opens a second WebRTC
  connection for the microphone → street (two-way audio).
- Optional **acoustic echo cancellation** (Espressif esp-sr AEC).
- Live speaker volume and microphone gain sliders.
- "Ding-dong" played on the street side while the call is ringing, mic muted
  until you answer.

**Call history**
- Every ring is logged on the microSD card.
- Unanswered after 30 s → marked missed, a **JPEG snapshot** of the visitor is
  saved (FIFO, 100 calls), and a badge appears on the weather screen.
- Calls page with snapshot viewer.

**Idle display**
- **Weather**: today + 7 days, animated Lottie icons (meteocons), clock,
  today's events, saint of the day (French calendar).
- **Calendar**: week and month views (any HA `calendar.*` entity, e.g. Google
  Calendar through the native HA integration — no OAuth on the device).
- **Presence detection** with the tablet's own front camera (OV02C10),
  published to HA so an automation can turn the screen off when nobody is
  around.

**Operations**
- On-screen **Settings** page (Wi-Fi, server host, MQTT, HA token, brightness,
  ringtone, do-not-disturb, echo cancellation…), stored in NVS.
- **Home Assistant MQTT discovery**: ~30 entities (screen light, DND, volumes,
  page selector, reboot, OTA, RSSI, CPU, PSRAM, missed calls, presence, logs…).
- **OTA** from Home Assistant, from the microSD card, or over the built-in
  file server, with automatic rollback if the new image cannot reach the
  network.
- **microSD log** (dated files, 14-day retention), **live logs over MQTT**.
- **USB mass-storage mode**: expose the SD card to a PC over the OTG port.
- **Connectivity watchdog**: reboot if Wi-Fi is down 5 min or MQTT 30 min,
  never during a call, an OTA or USB mode.
- Custom ringtones: any 8 kHz mono WAV at the root of the SD card.

## Hardware

| Part | Detail |
|---|---|
| Board | Guition **JC8012P4A1C** (ESP32-P4 + ESP32-C6, 16 MB flash, 32 MB PSRAM) |
| Display | JD9365 MIPI-DSI, 800×1280 portrait, used in landscape by LVGL |
| Touch | GSL3680 (I2C) |
| Audio | ES8311 codec, on-board speaker and microphone |
| Camera | OV02C10 MIPI-CSI (front, presence detection only) |
| Storage | microSD (shares the P4's single SDMMC controller with the Wi-Fi link) |
| Door station | Any RTSP camera go2rtc can read; two-way audio needs an ONVIF/RTSP backchannel (developed against a Dahua VTO) |

Server side: **go2rtc ≥ 1.9.15** (the Home Assistant add-on works well),
**Home Assistant** reachable over HTTPS on port 8123, an **MQTT broker**
(e.g. Mosquitto add-on). The firmware assumes all three run on the same host.

## Quick start

1. **Server**: set up go2rtc from [`go2rtc.example.yaml`](go2rtc.example.yaml),
   an MQTT user, and a Home Assistant long-lived token →
   [docs/server-setup.md](docs/server-setup.md).
2. **Build and flash** with ESP-IDF v6.0.1 →
   [docs/getting-started.md](docs/getting-started.md).
   ```bash
   idf.py build
   idf.py -p <PORT> flash monitor
   ```
   ⚠️ Never run `idf.py set-target`: it resets the hand-tuned `sdkconfig`.
3. **Configure** on the touch screen (Settings page: Wi-Fi, host, MQTT, HA
   token), then reboot. Or pre-fill `main/app_secrets.h` from
   [`main/app_secrets.example.h`](main/app_secrets.example.h) — it is
   git-ignored.
4. **Ring**: publish `ON` to `visiophone/ring` (or wire your doorbell in an HA
   automation, see the server guide).

## Documentation

| Document | Content |
|---|---|
| [docs/getting-started.md](docs/getting-started.md) | Toolchain, build, flash, first configuration, SD card |
| [docs/server-setup.md](docs/server-setup.md) | go2rtc, Home Assistant, MQTT topics and automations |
| [docs/architecture.md](docs/architecture.md) | Code layout, tasks and cores, video / audio pipelines |
| [docs/hardware-notes.md](docs/hardware-notes.md) | **ESP32-P4 pitfalls measured on this board** — read before touching sdkconfig or the BSP |
| [docs/maintenance.md](docs/maintenance.md) | OTA, remote logs, file server, USB mode, diagnostics |
| [CLAUDE.md](CLAUDE.md) | Coding rules (also used as instructions by AI coding assistants) |
| [test/README.md](test/README.md), [test_apps/README.md](test_apps/README.md) | Host unit tests and on-target tests |

## Why some choices look odd

A few decisions that are easy to "fix" and expensive to rediscover:

- **No H.264.** The P4's H.264 block is an *encoder only*; software decoding
  saturated a whole core. MJPEG + the hardware JPEG decoder costs ~5 ms/frame.
- **The MJPEG stream is padded to exactly 800 px wide** so the decoder can write
  directly into the framebuffer, skipping a 22 ms/frame copy.
- **Flash in DIO, never QIO**: QIO lines switching at 80 MHz next to the C6
  antenna cost 25 dB of RSSI and dropped video from 1.2 MB/s to 72 KB/s,
  without a single error message.
- **L2 cache line stays at 64 B**: 128 B speeds up JPEG but corrupts the SDIO
  link (esp_hosted hard-codes a 64 B DMA alignment).

All of them, with measurements: [docs/hardware-notes.md](docs/hardware-notes.md).

## Security

This is a LAN appliance. Read [SECURITY.md](SECURITY.md) before exposing
anything: MQTT is plain TCP, the Home Assistant certificate is not verified,
and the optional file server has no authentication (off by default).

## License

[MIT](LICENSE) for the code of this repository. Bundled third-party material
(display/touch drivers, camera init table, fonts, weather icons) keeps its own
license — see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
