# Maintenance and remote diagnostics

Once installed on a wall the board can be updated and diagnosed entirely over
the network, from the entities of its ESPHome device in Home Assistant
(Settings → Devices → Visiophone). In automations or *Developer tools →
Actions*, the usual actions apply: `button.press`, `switch.turn_on`,
`number.set_value`, `select.select_option`, `light.turn_off`… The board's IP
is shown by the "Adresse IP" sensor.

## Firmware updates (OTA)

The image to deploy is **only** `build/esp32_p4_webrtc_cam.bin` (not the
bootloader nor the partition table). Two OTA slots + **automatic rollback**:
a new image that cannot reach Wi-Fi *and* get Home Assistant connected to its
API after boot is never marked valid, and the next reset returns to the
previous one.

Before building a release, `idf.py reconfigure` refreshes the version string
from the latest git commit; otherwise the "Version du firmware" sensor cannot
tell two builds apart.

Progress for every method shows in the **Statut OTA** sensor:
`start` → `downloading N%` → `verify` → `reboot` → `ready` (~30 s).

### From the microSD card

1. Copy the `.bin` to the root of the card (USB mode, file server, or card
   reader). Any name ending in `.bin`; the version shown comes from the image
   header.
2. Either pick it on the screen (Micro/Debug page → "Carte SD" tab), or name
   it `fw.bin` and press **Mettre à jour depuis la carte SD** in HA.

The on-screen path works even when Home Assistant is down.

### Over Wi-Fi with the built-in file server

```bash
# 1. switch on "Serveur de fichiers" in HA (persisted! switch it off afterwards)

# 2. upload, check the size, flash
curl -X POST --data-binary @build/esp32_p4_webrtc_cam.bin "http://<ip>/api/up?path=/sdcard/fw_new.bin"
curl "http://<ip>/api/ls?dir=/sdcard"
curl -X POST "http://<ip>/api/ota?path=/sdcard/fw_new.bin"

# 3. clean up, then switch "Serveur de fichiers" off
curl -X POST "http://<ip>/api/rm?path=/sdcard/fw_new.bin"
```

`http://<ip>/` also serves a small web page to browse, download and upload SD
files. The server has **no authentication**: keep it off outside maintenance.

### From Home Assistant

Copy the image to `/config/www/fw.bin` and press **Mettre à jour (Wi-Fi)**.
The board downloads `https://<host>:8123/local/fw.bin`; the URL is built from
the configured host, never received from the network.

## Logs

| Need | How |
|---|---|
| Live logs | ESPHome integration option "Subscribe to logs" (into the HA log), or `aioesphomeapi-logs <ip> --noise-psk <key>` from a PC — see [server-setup.md](server-setup.md#logs) |
| Survive a crash | Switch **Journal carte SD** on → `/sdcard/log/<date>.txt`, one file per day, 14 days kept. Each boot writes a banner with the reset reason. |
| On the device | Micro/Debug page: live log tab, SD log viewer |
| Over USB | `idf.py -p <PORT> monitor` on the USB-Serial-JTAG port |

The `MJPEG` line (once per second during video) and the `CPU:` line are the
first things to read for any performance question; see
[hardware-notes.md](hardware-notes.md).

## Testing without disturbing the house

- Open the camera **without ringing**: switch **Caméra sans sonnerie** on /
  off — no ringtone, no street-side ding-dong, no missed call recorded.
- Mute the speaker: **Volume haut-parleur** = 0 (persisted: note the
  previous value).
- Silent ring test: **Volume sonnerie** = 0, then press **Sonner** (the
  street-side ding-dong does not depend on it).

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
features (doorbell, OTA, watchdog) are paused meanwhile. Leaving the mode
remounts the card and reloads the ringtone.

## Connectivity watchdog

Reboots after 5 min without Wi-Fi or 30 min without Home Assistant connected
to the API (that second rule only applies once HA has adopted the board and
provisioned its key). Frozen during a call, an OTA, USB mode, and while the board has no
SSID or host configured.

## Re-adopting the board in Home Assistant

The API encryption key lives in the board's NVS (namespace `esphome_api`). If
NVS is erased (`idf.py erase-flash`, `tools/provision_nvs.ps1`), the board
forgets it and HA reports an invalid key: delete the device from the ESPHome
integration and add it again; HA provisions a new key.
