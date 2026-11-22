# Getting started

From a bare JC8012P4A1C board to a working intercom. Set up the server first
([server-setup.md](server-setup.md)): the board is only useful once go2rtc
and Home Assistant answer.

## 1. Toolchain

- **ESP-IDF v6.0.1** exactly. Other versions are untested; 5.x will not build
  (mbedTLS 4, components moved to the registry — see
  [hardware-notes.md](hardware-notes.md#esp-idf-60)).
- Any install method works: the VS Code ESP-IDF extension, `install.sh` /
  `install.ps1`, or the `espressif/idf:v6.0.1` Docker image.
- The ESP-IDF component manager downloads the dependencies listed in
  `main/idf_component.yml` on the first build (network required). Versions are
  pinned by `dependencies.lock`.
- If configure stops with `Missing required kconfig option after retry`
  (seen with the `espressif/idf:v6.0.1` Docker image, which ships
  `esp-idf-kconfig` 3.8.1), upgrade the Python tools inside the IDF
  environment — these are the versions the project is built with:
  ```bash
  python -m pip install "idf-component-manager==3.0.3" "esp-idf-kconfig==3.11.1"
  ```

## 2. Build

From an ESP-IDF shell (where `idf.py` is on the `PATH`):

```bash
git clone <this repository>
cd esp32-p4-video-intercom
idf.py build
```

The target is already `esp32p4` and the project ships its tuned `sdkconfig`.

> ⚠️ **Never run `idf.py set-target`.** It regenerates `sdkconfig` from
> scratch: 137+ hand-tuned options are lost at once (PERF optimisation, 360 MHz
> CPU, esp_hosted SDIO pins, Lottie support, flash mode…). The resulting
> firmware may boot but will misbehave in subtle ways. If you need a fresh
> config, delete `sdkconfig` and let `sdkconfig.defaults` regenerate it, then
> compare with the committed file.

> ⚠️ `sdkconfig.defaults` does **not** override an option already present in
> `sdkconfig`. After editing the defaults, check the effective value in
> `sdkconfig` (or `idf.py menuconfig`).

On Windows with the VS Code extension's default layout
(`C:\esp\v6.0.1\esp-idf`, `C:\Espressif\tools`), `run.ps1` sets the environment
up for you:

```powershell
.\run.ps1              # build
.\run.ps1 -Flash       # build + flash + monitor (Ctrl+] to quit)
.\run.ps1 -Monitor     # monitor only
.\run.ps1 -Port COM12  # another port (default COM7)
```

## 3. Flash

The board has three USB-C connectors:

| Connector | Role |
|---|---|
| USB-UART bridge | Serial console on UART0 (not used by default) |
| **Unlabelled** | **USB OTG High-Speed** — used by the USB mass-storage mode |
| The remaining one | **USB-Serial-JTAG**: flashing and the console (`COMx` / `/dev/ttyACM0`) |

```bash
idf.py -p <PORT> flash monitor
```

The partition table (`partitions.csv`) has two 6 MB OTA slots and a 24 KB NVS.
Later updates can go over the network, see [maintenance.md](maintenance.md).

## 4. First configuration

Three ways, pick one:

1. **On the touch screen** (recommended). With no configuration the board boots
   normally, shows the home screen and keeps retrying Wi-Fi in the background.
   Open the **Configuration** page from the home screen and fill in:
   - Wi-Fi SSID and password,
   - **Host**: IP of the machine running go2rtc (and Home Assistant, which
     serves the optional OTA image).

   Save, then reboot: network settings are applied at boot only. Home
   Assistant needs nothing on the board: once it is on Wi-Fi, HA discovers it
   as an ESPHome device and sets up the encryption key itself — see
   [server-setup.md](server-setup.md#adopt-the-device).
2. **Compiled defaults**: copy `main/app_secrets.example.h` to
   `main/app_secrets.h` (git-ignored), fill it, build and flash. Values entered
   later on the screen override them.
3. **Over USB without the screen**: [`tools/provision_nvs.ps1`](../tools/README.md).

Installation constants that are not editable on screen live in
[`main/app_config.h`](../main/app_config.h) — adapt them before building:

| Constant | Default | Meaning |
|---|---|---|
| `kHaWeatherEntity` | `weather.forecast_home` | HA weather entity shown on the weather page — set `APP_SECRET_HA_WEATHER_ENTITY` in `main/app_secrets.h` |
| `kHaCalendarEntity` | `calendar.home` | HA calendar entity for the agenda — set `APP_SECRET_HA_CALENDAR_ENTITY` in `main/app_secrets.h` |
| `kTimezone` | `CET-1CEST,M3.5.0,M10.5.0/3` | POSIX TZ string (Europe/Paris) |
| `kGo2rtcAudioSrc`, `kGo2rtcMicSrc`, `kGo2rtcMjpeg2Src` | `vto_audio_only`, `vto_audio_input_only`, `vto_mjpeg2` | go2rtc stream names |
| `kGo2rtcPort`, `kHaPort` | 1984, 8123 | Service ports (`kHaPort`: OTA image only) |

The ESPHome API listens on TCP **6053**; the device name `visiophone` and the
entity table are in [`main/app/ha_api.cpp`](../main/app/ha_api.cpp). The
firmware update entity fetches `https://<host>:8123/local/fw.bin`; if
your HA only serves plain HTTP, change the scheme of `s_url_ota` in
`main/sys/settings.cpp`.

## 5. microSD card

Optional but recommended: call history and snapshots, persistent logs, custom
ringtones, OTA from file. Use a FAT32-formatted card (the "Carte SD" tab of the
Micro/Debug page can also format it). The ringtone file is read at boot.

| Path | Content |
|---|---|
| `/sdcard/calls.log`, `/sdcard/calls/` | Call journal and missed-call snapshots |
| `/sdcard/log/<date>.txt` | Persistent log (when enabled), 14-day retention |
| `/sdcard/*.wav` | Custom ringtones, selectable on the Settings page — see [assets/ringtone-test](../assets/ringtone-test/README.md) |
| `/sdcard/*.bin` | Firmware images for OTA from the card |

## 6. Test a call

In Home Assistant, press the device's **Sonner** button (or run the action
`button.press` on `button.visiophone_sonner`). The camera screen opens, the
ringtone plays and the street side hears a ding-dong. Tap **Répondre**
(Answer) to talk. Without an answer the call is marked missed after 30 s and a
snapshot is stored.

To open the camera without ringing (testing at night, measuring): the switch
**Caméra sans sonnerie**.

## Host unit tests

No board needed — see [test/README.md](../test/README.md):

```bash
cmake -S test -B test/build -G Ninja && cmake --build test/build && ctest --test-dir test/build
```
