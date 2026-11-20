# Maintenance and remote diagnostics

Once installed on a wall the board can be updated and diagnosed entirely over
the network. Examples use `mosquitto_pub` / `mosquitto_sub`; any MQTT client
works. The board's IP is published by the "Adresse IP" sensor.

## Firmware updates (OTA)

The image to deploy is **only** `build/esp32_p4_webrtc_cam.bin` (not the
bootloader nor the partition table). Two OTA slots + **automatic rollback**:
a new image that cannot reach Wi-Fi *and* MQTT after boot is never marked valid,
and the next reset returns to the previous one.

Before building a release, `idf.py reconfigure` refreshes the version string
from the latest git commit; otherwise `visio/fw/version` cannot tell two
builds apart.

Progress for every method is published on `visio/ota/status`:
`start` → `downloading N%` → `verify` → `reboot` → `ready` (~30 s).

### From the microSD card

1. Copy the `.bin` to the root of the card (USB mode, file server, or card
   reader). Any name ending in `.bin`; the version shown comes from the image
   header.
2. Either pick it on the screen (Micro/Debug page → "Carte SD" tab), or:
   ```bash
   mosquitto_pub -h <host> -u <user> -P <pass> -t visio/ota/sd -m /sdcard/fw.bin
   ```

This path works even when Home Assistant is down.

### Over Wi-Fi with the built-in file server

```bash
# 1. enable the file server (persisted! switch it off afterwards)
mosquitto_pub ... -t visio/file_server/set -m ON

# 2. upload, check the size, flash
curl -X POST --data-binary @build/esp32_p4_webrtc_cam.bin "http://<ip>/api/up?path=/sdcard/fw_new.bin"
curl "http://<ip>/api/ls?dir=/sdcard"
curl -X POST "http://<ip>/api/ota?path=/sdcard/fw_new.bin"

# 3. clean up
curl -X POST "http://<ip>/api/rm?path=/sdcard/fw_new.bin"
mosquitto_pub ... -t visio/file_server/set -m OFF
```

`http://<ip>/` also serves a small web page to browse, download and upload SD
files. The server has **no authentication**: keep it off outside maintenance.

### From Home Assistant

Copy the image to `/config/www/fw.bin` and press the "Mettre à jour (Wi-Fi)"
button, or publish a URL on `visio/ota/cmd`. Only URLs on the configured host
are accepted (`fail: url not allowed` otherwise).

> Serving the image from a PC often fails: the PC's firewall blocks incoming
> connections (`esp-tls: select() timeout`). Prefer the SD card path.

## Logs

| Need | How |
|---|---|
| Live logs | `visio/log/enable` = `1` → batches on `visio/log` (sliding window, de-duplicate lines). Falls back to `0` at reboot. |
| Survive a crash | `visio/sd_log/set` = `ON` → `/sdcard/log/<date>.txt`, one file per day, 14 days kept. Each boot writes a banner with the reset reason. |
| On the device | Micro/Debug page: live log tab, SD log viewer |
| Over USB | `idf.py -p <PORT> monitor` on the USB-Serial-JTAG port |

The `MJPEG` line (once per second during video) and the `CPU:` line are the
first things to read for any performance question; see
[hardware-notes.md](hardware-notes.md).

## Testing without disturbing the house

- Open the camera **without ringing**: `visio/camera/set` = `ON` / `OFF` — no
  ringtone, no street-side ding-dong, no missed call recorded.
- Mute the speaker: `visio/speaker_volume/set` = `0` (persisted: note the
  previous value).
- Silent ring test: `visio/ring_volume/set` = `0` (the street-side ding-dong
  does not depend on it).
- Action topics ignore retained messages: publish without `-r`.

## Two-way audio checks

During a call:

```bash
curl "http://<host>:1984/api/streams?src=vto_audio_input_only"
```

Healthy: a door-station producer with a `sendonly` track, whose `senders`
follow the board's `receivers`. No `sendonly` → another client holds the door
station's only talk channel. Route every other client through go2rtc with
`#backchannel=0`.

## USB mass-storage mode

From the SD tab of the Micro/Debug page. The card is unmounted and exposed to
the PC on the **unlabelled USB-C connector** (OTG High-Speed); SD-dependent
features (MQTT ring, OTA, watchdog) are paused meanwhile. Leaving the mode
remounts the card and reloads the ringtone.

## Connectivity watchdog

Reboots after 5 min without Wi-Fi or 30 min without MQTT. Frozen during a
call, an OTA, USB mode, and while the board has no SSID or host configured.
