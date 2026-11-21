# Host unit tests

A CMake project **separate** from the ESP-IDF firmware at the repository root.
It compiles the hardware-free application sources with your PC's compiler and
runs them locally: a feedback loop under one second, versus ~40 s for a
build → flash → monitor cycle on the board.

Framework: **GoogleTest**, driven by `ctest`.

## Prerequisites

- **CMake** and **Ninja**. `run.ps1` picks them up from the ESP-IDF tools
  folder (`C:\Espressif\tools`) if they are not on your `PATH`.
- A **host C++ compiler** (`g++`, `clang++` or MSVC `cl`). The ESP-IDF compiler
  is a RISC-V cross-compiler and cannot produce a PC executable.
- Git and network access on the first configure only: GoogleTest and cJSON are
  fetched with `FetchContent`.

> ⚠️ **On Windows, install MinGW-w64 at a short path** (e.g. `D:\mingw64`,
> which `run.ps1` adds to `PATH` automatically). GCC builds its internal include
> path without normalising `..` segments and is not long-path aware. Installed
> by winget, the package folder name pushes that prefix to ~245 characters, and
> every header with a name longer than ~14 characters goes past `MAX_PATH`:
> `fatal error: bits/error_constants.h: No such file or directory`, while
> `bits/c++config.h` in the *same folder* resolves. Junctions and `subst` do not
> help, GCC resolves the real path.

cJSON is compiled by this project (C target `cjson_host`) rather than through
its own `CMakeLists.txt`, which declares `cmake_minimum_required(VERSION 2.8.5)`
and is rejected by CMake 4. That is also why the project enables `C` on top of
`CXX`.

## Running

```powershell
cd test
.\run.ps1                    # build + run every test
.\run.ps1 -Filter "G711.*"   # subset (a ctest regex, not a glob)
.\run.ps1 -Clean             # start from scratch
```

On Linux/macOS, the same thing by hand:

```bash
cmake -S test -B test/build -G Ninja && cmake --build test/build && ctest --test-dir test/build
```

## Scope

Only **driver-free** code belongs here. `stubs/` fills three gaps of the host
and nothing else:

| Stub | Provides | Why |
|---|---|---|
| `esp_log.h` | `ESP_LOGx` → `stderr` | avoids pulling IDF's log component (which depends on FreeRTOS) |
| `esp_timer.h` | `esp_timer_get_time()` **driven by the test** | makes time-based guards testable (e.g. the ringtone 30 s auto-disarm) without waiting 30 s |
| `host_compat.h` | `strlcpy` (force-included) | BSD function present in IDF's libc, missing from MinGW |

If a module needs more than that to build on the host, it touches the hardware
and belongs on target. **The right move is not to grow the stubs but to split
the module**: `net/weather.cpp` keeps HTTP, the task and the cache;
`net/weather_parse.cpp` only decodes, and only the latter is compiled here.
Same pattern for `gcal` and `bsp/ppa_srm_quirk.cpp`.

Covered today: G.711 A-law, ringtone mixer, WAV header parsing, Home Assistant
weather and calendar parsers, call-log line parser, motion detection, Lottie
pre-multiplied blending, PPA DIG-734 band splitting, OTA image header check,
file-server path sanitising, saint-of-the-day table, and the ESPHome API wire
layer: protobuf codec, frame headers, base64, and the Noise_NNpsk0 responder
checked against the cacophony test vector. The crypto runs on the same
mbedTLS 4.0.0 release as ESP-IDF 6.0, fetched at configure time (the first
configure takes about a minute longer).

The Jinja response templates the firmware sends to Home Assistant are checked
separately, by rendering them like HA does:

```bash
pip install jinja2
python test/ha_templates/check_templates.py .
```

### Rules of thumb

- **Test vectors are never regenerated from the code under test** — a vector
  produced by running the implementation validates its bugs too. A-law values
  are computed by hand from RFC 3551; Unix epochs are derived by hand from a
  known anchor.
- **A suite that passes on the first run proves nothing.** On existing code
  there is no red phase: mutate the implementation (flip a timezone sign, drop
  a saturation), check that the expected tests fail, then restore.
- **No boot-time `self_test()` for pure logic.** A deterministic self-test gives
  the same verdict on every boot: boot time, flash and UART noise for no new
  information. The only one left, `sd_log::self_test()`, checks that a real SD
  card is mounted and writable *this boot*.

Hardware itself (JPEG engine, PPA, I2S, DSI) is covered by
[`../test_apps`](../test_apps/README.md), an ESP-IDF Unity project flashed on
the board.
