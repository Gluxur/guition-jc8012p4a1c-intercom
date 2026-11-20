# Server setup

The board talks to three services, expected on the **same host** (one "Host"
field in the settings):

| Service | Port | Used for |
|---|---|---|
| go2rtc | 1984 (HTTP API), 8555 (WebRTC) | MJPEG video, WebRTC audio in and out |
| Home Assistant | 8123 (HTTPS) | Weather, calendar (REST API, long-lived token) |
| MQTT broker | 1883 | Doorbell trigger, HA discovery, OTA, logs |

A typical setup is Home Assistant OS with the **go2rtc add-on (AlexxIT)** and
the **Mosquitto broker** add-on.

```
 door station ──RTSP──▶ go2rtc ──MJPEG (HTTP)──────────▶ ESP32-P4
   (camera +           │       ──WebRTC audio (G.711)──▶  (speaker)
    backchannel) ◀─────┘       ◀─WebRTC audio (G.711)───  (microphone)

 doorbell button ──▶ Home Assistant ──MQTT visiophone/ring──▶ ESP32-P4
                         ▲  ◀──MQTT discovery / states───────
                         └──── REST: weather, calendar ◀─────
```

## go2rtc

Start from [`go2rtc.example.yaml`](../go2rtc.example.yaml). The comments explain
each flag; the essentials:

| Stream | Role | Must keep |
|---|---|---|
| `vto_mjpeg2` | Video | Output **exactly 800×1280** (`transpose=1` + `pad`); frame rate under the delivered throughput (~1 MB/s); wall-clock `setpts` |
| `vto_audio_only` | Street → speaker | `#audio=pcma` (G.711 A-law, the only codec the firmware negotiates) |
| `vto_audio_input_only` | Microphone → street | `#backchannel=1` on the source |

Plus:

- `preload: vto_mjpeg2` → first image in ~0.2 s instead of ~3 s.
- `webrtc: ice_servers: []` and an IPv4 LAN filter → SDP answer in milliseconds.
  Without it go2rtc ≥ 1.9.15 takes ~5 s to answer, the firmware gives up after
  4 s and you get video without sound.

Checks from a browser on the LAN:

- `http://<host>:1984/api/stream.mjpeg?src=vto_mjpeg2` must show a portrait
  800×1280 picture with black bars.
- During a call, `http://<host>:1984/api/streams?src=vto_audio_input_only`
  must show the door station producer with a `sendonly` track. Without
  `sendonly`, another client holds the door station's single talk channel
  (seen in practice: Home Assistant's ONVIF integration). Any other client of
  the door station should go through go2rtc with `#backchannel=0`.

### Video and audio out of sync?

If the sound runs ahead of the picture and the gap grows, go2rtc produces more
frames than Wi-Fi delivers: frames queue in its TCP send buffer and get older
and older. Compare `recv=` in the board's `MJPEG` log line with the `fps=` of
the stream. Lower the fps or raise `-q:v` (smaller JPEGs). Frame size depends
on the scene: ~70 KB in daylight, ~20 KB at night in IR.

## MQTT

Create a broker user for the device. The firmware subscribes and publishes
under `visio/…`, `visiophone/ring` and the HA discovery prefix
`homeassistant/…`.

Main topics (the full list is in `main/app_config.h` and
`main/app/ha_entities.cpp`):

| Topic | Direction | Payload |
|---|---|---|
| `visiophone/ring` | → device | `ON` starts a call (ringtone + camera screen) |
| `visio/camera/set` | → device | `ON` / `OFF`: open/close the camera screen without ringing |
| `visio/status` | ← device | `online` / `offline` (MQTT last will) |
| `visio/fw/version` | ← device | Firmware version (retained) |
| `visio/ota/cmd` | → device | Firmware URL (must be on the configured host) |
| `visio/ota/sd` | → device | Path of a `.bin` on the SD card |
| `visio/ota/status` | ← device | `start`, `downloading N%`, `verify`, `reboot`, `ready`, `fail: …` |
| `visio/log/enable` | → device | `1` / `0`: stream logs to `visio/log` (not persisted) |
| `visio/log` | ← device | Log batches (sliding window: de-duplicate) |
| `visio/<entity>/set` | → device | Commands of the discovered entities (DND, volumes, page, reboot…) |

Retained messages on action topics (ring, camera, reboot, OTA…) are
**ignored** on purpose, so a stale retained `ON` does not ring at every boot.
Publish without `-r`.

## Home Assistant

### Access token

Profile → Security → **Long-lived access tokens** → create one, paste it on
the device's Settings page. A dedicated, non-admin HA user is a good idea.

HA must answer over **HTTPS** on port 8123 (self-signed certificate accepted).
For plain HTTP, see [getting-started.md](getting-started.md#4-first-configuration).

### Discovered device

With MQTT discovery enabled (default), a **Visiophone P4** device appears with
about thirty entities. Their display names are in French; entity IDs are
derived from them (check the device page). Among them:

- `light` Écran (screen on/off + brightness), `switch` Ne pas déranger (DND),
- `select` Page affichée (`accueil`, `meteo`, `agenda`, `appels`, `configuration`),
- `number` ring / speaker volume, mic gain,
- `switch` Caméra sans sonnerie, `button` reboot, OTA, acknowledge missed calls,
- `sensor` RSSI, IP, uptime, CPU per core, PSRAM, SD free space, missed calls,
  firmware version, OTA status,
- `binary_sensor` Appel en cours (in call), Présence devant l'écran (presence).

### Doorbell automation

Ring the device when your door station's button is pressed. The trigger depends
on your door station integration; for example with a Dahua VTO event:

```yaml
automation:
  - alias: "Doorbell → indoor screen"
    trigger:
      - platform: state
        entity_id: binary_sensor.door_station_button   # your doorbell entity
        to: "on"
    action:
      - service: mqtt.publish
        data:
          topic: visiophone/ring
          payload: "ON"
```

### Screen off when nobody is around

The front camera publishes presence; the firmware does not decide anything by
itself. An automation turns the screen light off/on:

```yaml
automation:
  - alias: "Indoor screen follows presence"
    trigger:
      - platform: state
        entity_id: binary_sensor.visiophone_p4_presence_devant_l_ecran  # check the real ID
        for: "00:02:00"
    action:
      - service: "light.turn_{{ 'on' if trigger.to_state.state == 'on' else 'off' }}"
        target:
          entity_id: light.visiophone_p4_ecran
```

During a call presence is forced to "present", so the screen never goes dark
while someone rings.

### Weather and calendar

- Weather: any `weather.*` entity that supports `weather.get_forecasts`
  (daily). Set its ID in `cfg::kHaWeatherEntity`.
- Calendar: any `calendar.*` entity (Google Calendar, CalDAV, local…). Set its
  ID in `cfg::kHaCalendarEntity`.

### Firmware served by Home Assistant (optional)

The "Update (Wi-Fi)" button proposes `https://<host>:8123/local/fw.bin`: copy
`build/esp32_p4_webrtc_cam.bin` to `/config/www/fw.bin` on HA. Other ways to
update are in [maintenance.md](maintenance.md).
