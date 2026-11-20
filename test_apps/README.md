# On-target tests

An ESP-IDF project **independent** from the root firmware. It builds its own
binary, flashed *instead of* the application. Nothing here is linked into the
production firmware.

Framework: **Unity** (shipped with ESP-IDF), with an interactive console menu.

## Scope

Only what **cannot** be tested on a PC — the silicon. Pure logic goes to
[`../test`](../test/README.md) (GoogleTest on host, < 1 s loop). Here each
iteration costs a build + flash + monitor, so it is not a TDD loop.

| Tag | Covers |
|---|---|
| `[jpeg]` | Hardware JPEG engine: header, RGB565 output, BGR order, 16 px alignment, time budget |
| `[bsp]` | PPA (scale/centre, lost transactions), zero-copy path, DSI double framebuffer, volume/backlight bounds |
| `[audio]` | ES8311 in I2S duplex at 8 kHz: mic read, speaker write |

`main/assets/quad_*.jpg` are two four-quadrant test cards (red / green / blue /
white). `200x120` has a width that is **not a multiple of 16**, on purpose: it
exercises the decoder's output alignment.

## Running

> ⚠️ Flashing this firmware **replaces the application** on the board. Flash
> the root project again to get the intercom back.

```powershell
cd test_apps
.\run.ps1              # build only
.\run.ps1 -Flash       # build + flash + monitor
```

Or, from an ESP-IDF shell: `idf.py -C test_apps -p <PORT> flash monitor`.

At boot the menu waits on the console. Press ENTER to list the tests.

| Input | Effect |
|---|---|
| `0` or `*` | run the whole suite |
| `7` | one test by number |
| `3 7 12` | several tests |
| `[jpeg]` | every test of a tag |
| `![audio]` | everything **except** that tag |
| `"BSP: ..."` | one test by exact name |
| `-` | show the last summary again |

ESP-IDF's `unity_run_menu()` only knows `*` (`0` fell into the "test number"
branch as −1 and ran nothing), so the menu lives in `main/test_menu.cpp` and
uses only the public API of the `unity` component.

## What these tests protect

Each case encodes a diagnosis that was paid for once:

- **BGR order** — `JPEG_DEC_RGB_ELEMENT_ORDER_RGB` outputs byte-swapped RGB565:
  rainbow screen. The test reads the four quadrants and checks R5/G6/B5.
- **16 px alignment** — the engine writes the *aligned* rectangle, not the
  useful one: 208 × 128 × 2 bytes for a 200 × 120 image, and a buffer sized for
  200 × 120 must be rejected.
- **`ppa_video_timeouts()`** must stay at 0. Any non-zero value is the early
  sign of an SRM engine about to lock up, i.e. a fully frozen device.
- **Double framebuffer** — in PARTIAL mode LVGL only writes to the scanned-out
  buffer; both must exist and stay distinct.
- **`present_frame()` only arms the flip** — it takes effect at the end of the
  current DSI frame, hence `wait_frame_flip()`.
- **Volume / backlight** — clamped to 100 and persisted in NVS. Tests restore
  the user's value afterwards.

## Implementation notes

- `WHOLE_ARCHIVE` in `main/CMakeLists.txt` is **mandatory**: `TEST_CASE`s
  register through static constructors that no symbol references. Without it
  the menu lists zero tests.
- `main/idf_component.yml` does **not** declare `esp_peer` or `esp_wifi_remote`:
  no network is brought up, which avoids pulling `esp_hosted`.
- `sdkconfig.defaults` keeps the production PSRAM settings (a test on another
  memory configuration proves nothing about the product) but **enables
  assertions**, which production disables for speed.
- The JPEG engine is created at boot in `app_main`, as in production: it needs
  internal DMA-capable RAM that is gone later on.
