# Architecture

## Code layout

`main/` is split by **layer**, one folder per responsibility. Cross-folder
includes are qualified (`#include "net/weather.h"`) so the layer of a header is
visible where it is included.

```
main/
  main.cpp          wiring only: init order, callbacks, ui::Callbacks (keep it thin)
  app_config.h      compiled defaults + deployment constants (namespace cfg)
  app/              application logic
    camera_session    incoming WebRTC audio peer, signalling + retry, video start/stop
    mic_session       outgoing microphone peer (backchannel), street-side ding-dong
    audio_pipeline    PCM slots, render task, I2S DMA purge, ringtone mixing
    video_mjpeg       hardware JPEG engine, zero-copy mailbox, render task, snapshots
    call_history      one call = one ring ("Sonner" button); journal, missed-call snapshots, 30 s timeout
    presence          front-camera motion detection, published to HA
    ha_api            Home Assistant entity catalogue (ESPHome API) and command handlers
    *_parse, motion_detect, ephemeris   pure logic, host-tested
  ui/               LVGL screens, one file per screen
    ui                home screen, ui::Callbacks (the only bridge to app/)
    ui_camera_v2      full-panel camera screen + 80 px control band
    ui_weather_v3     weather page (pre-rendered Lottie icons), ui_agenda, ui_calls,
    ui_call_viewer, ui_settings, ui_mic (debug), ui_sdlog, ui_usb, ui_stats
  net/              wifi_net, signaling (go2rtc SDP exchange), mjpeg_client,
                    weather / gcal (+ *_parse), file_server,
                    esphome_api (ESPHome native API server + mDNS),
                    esphome_pb / esphome_frame / noise_nnpsk0 (its wire format and
                    encryption, host-tested)
  media/            aec (esp-sr), g711, ringtone (passive mixer), ringtone_sd, wav_parse
  sys/              settings (network config, NVS, applied at boot), prefs (comfort
                    settings, applied live), dbg_log, sd_log,
                    ota_ctrl, net_wdt, usb_msc, sys_stats, mutex.h
components/
  bsp/              bsp::Bsp singleton: display, touch, LVGL, PPA, audio codec,
                    microSD, front camera
  esp_lcd_jd9365, esp_lcd_touch_gsl3680   vendored drivers
test/               host unit tests (GoogleTest)
test_apps/          on-target tests (Unity, separate IDF project)
```

All hardware access goes through `bsp::Bsp` (`components/bsp/include/bsp.h`).
`ui::Callbacks` holds plain function pointers (no captures) and is the only
link from screens to application logic, which keeps the module graph acyclic.

## Configuration: two contracts

| | `settings::` | `prefs::` |
|---|---|---|
| Content | Wi-Fi, host | DND, volumes, ringtone, brightness, SD log, echo cancellation… |
| NVS namespace | `net_cfg` | `ui_prefs` |
| Applied | **At reboot** (URLs are formatted once in `settings::init()`) | **Live** |
| Fallback | `cfg::defaults::*` ← optional `main/app_secrets.h` | Compiled defaults |

Kept apart on purpose: reconfiguring the network live during a stream is a
reliable source of hard bugs. No module ever formats a URL itself.

## Video pipeline (go2rtc MJPEG → screen)

```
 go2rtc ──HTTP multipart──▶ mjpeg task (core 0)        mjpeg_rnd task (core 1)
                            drain socket, find SOI/EOI    HW JPEG decode
                            write into a mailbox slot ──▶ straight into the DSI back
                            (no frame buffer of its own)  buffer, then present_frame()
```

- **Hardware JPEG** (`driver/jpeg_decode.h`), ~5 ms per 800×1280 frame. The
  P4 has no H.264 decoder: its H.264 block only encodes.
- **Zero copy**: when the source is exactly as wide as the panel (800 px) and
  16-px aligned, `Bsp::direct_video_target()` returns an address inside the
  display back buffer and the decoder writes there. Otherwise
  `render_rgb565_to_panel()` scales/centres with the PPA (fallback).
- **Mailbox, two slots, "last frame wins"**: a late renderer overwrites the
  unconsumed frame instead of accumulating delay. It cannot protect against a
  *link* that is too slow — hence the go2rtc fps limit.
- The network task must drain the socket continuously (`memchr` + `memcpy`,
  never byte by byte): if the TCP window fills, go2rtc slows down. Throughput is
  delivery-bound (~1 MB/s sustained over the C6 SDIO link), not bandwidth-bound.
- A log line once per second (`MJPEG …`) reports `recv`, `drop`, `KB/s`,
  decode time; `CPU: C0=…% C1=…%` reports per-core load.

## Audio

- **Incoming**: `camera_session` opens an audio-only `esp_peer` (WebRTC,
  G.711 A-law, 8 kHz). `audio_pipeline` (task `aud_render`, core 1, prio 6)
  plays it through `Bsp::play_pcm()` and mixes the ringtone in passively.
- **Outgoing**: `mic_session` opens a **second** peer (`SEND_RECV`) with its
  own signalling channel. Task `mic_send` reads `Bsp::read_pcm()` (ES8311 over
  I2S), optionally runs the esp-sr AEC (8↔16 kHz resampling around it, 32 ms
  blocks), encodes A-law and sends.
- **Ringback**: when a call rings, the mic peer is opened *muted* with a
  synthesised ding-dong mixed in, so the visitor hears it. Answering just
  unmutes — no renegotiation. The ringback opens only after the audio peer is
  `CONNECTED`: two simultaneous DTLS handshakes exhaust internal SRAM.
- AEC reference = the DAC output looped back by the ES8311 on the right I2S
  slot, not a copy of what was queued to the speaker (that one is up to 180 ms
  early, beyond the AEC filter's reach).

## Tasks and cores

| Task | Core | Prio | Role |
|---|---|---|---|
| `mjpeg` | 0 | — | Drain MJPEG TCP stream |
| `presence` | 0 | 2 | ~1 analysis/s, outside calls only |
| `peer_loop` | 1 | 5 | WebRTC (SRTP audio) main loop |
| `mjpeg_rnd` | 1 | 4 | Decode + present |
| `aud_render` | 1 | 6 | Speaker output |
| `mic_send` | 1 | — | Capture, AEC, encode, send |
| `lvgl` | — | 5 | UI (PSRAM stack, 48 KB: ThorVG needs it) |

Don't move these affinities without measuring throughput: pinning lwIP's
`tcpip` task to core 1 was measured strictly worse (see
[hardware-notes.md](hardware-notes.md)).

## Boot order

`app_main` initialises in an order where each step has a written reason:
NVS → debug log ring → SD log ring → `settings` → `prefs` → **BSP** (owns the
SDMMC controller) → SD log → call history → ringtone from SD (*before* the
audio pipeline, its only reader) → G.711 → audio pipeline → **JPEG engine**
(needs internal DMA RAM early) → sessions → LVGL task → Wi-Fi → OTA (*before*
the API: HA may press an OTA button as soon as it connects) → API log sink →
**ESPHome API** (mDNS + server) → presence → OTA validation (once HA is
connected) → connectivity watchdog **last**.

## Home Assistant link (ESPHome native API)

The board implements the server side of the
[ESPHome native API](https://esphome.io/components/api/) itself — it does not
run ESPHome, which pins its own ESP-IDF release.

```
 HA ──TCP 6053──▶ esphome_api task (core 0, prio 3, 12 KB stack in PSRAM)
                   │ frames: plaintext (0x00) until a key is set, then only
                   │ Noise_NNpsk0_25519_ChaChaPoly_SHA256 (0x01)
                   ├─ ListEntities / states / commands  ◀──▶ app/ha_api table
                   ├─ SubscribeLogs  ◀── dbg_log sink (ring, any task)
                   └─ HomeassistantActionRequest ──▶ HA runs weather.get_forecasts /
                      calendar.get_events, answers with the (templated) response;
                      net/weather and net/gcal wait for it in their fetch tasks
```

- **Key provisioning**: an unprovisioned board accepts the all-zero Noise PSK
  (and plaintext). HA generates a key and sends it with
  `NoiseEncryptionSetKeyRequest`; the board stores it in NVS (`esphome_api`
  namespace), advertises `api_encryption` in mDNS and from then on rejects
  anything else.
- **Crypto** comes from PSA Crypto (mbedTLS 4, shipped with ESP-IDF 6);
  `CONFIG_MBEDTLS_CHACHAPOLY_C` is enabled for it. The Noise responder is
  checked on the host against the official test vector.
- **State pushes**: a 64-bit dirty mask (`esphome_api::publish`) from any task,
  drained by the server task within ~100 ms; plus a 30 s refresh, skipped
  during a call except for entities marked `refresh_when_busy`.
- The API version announced is 1.13 on purpose: from 1.15 clients send a
  capabilities request this server does not implement.

## Display

- Panel: physically 800×1280 portrait; LVGL is rotated 270° → logical 1280×800
  landscape. Touch is configured `mirror_x=true, mirror_y=false`.
- LVGL in **PARTIAL** render mode with two 32-line draw buffers in internal
  SRAM; each band is rotated by the PPA directly into the scanned-out
  framebuffer. Double framebuffering + `present_frame()` is reserved to video.
- During video, "direct mode" makes the LVGL flush a no-op; the 80 px control
  band is re-blitted after each decoded frame.
- Weather icons are Lottie animations pre-rendered once into PSRAM (15 fps)
  and then just flipped: core 0 load on that page went from 75-98 % to ~13 %.
