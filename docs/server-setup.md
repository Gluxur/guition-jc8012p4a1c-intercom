# Server setup

The board talks to two services, expected on the **same host** (one "Host"
field in the settings):

| Service | Port | Used for |
|---|---|---|
| go2rtc | 1984 (HTTP API), 8555 (WebRTC) | MJPEG video, WebRTC audio in and out |
| Home Assistant | — (HA connects **to** the board on 6053) | Entities, doorbell, weather, calendar, logs |
| Home Assistant | 8123 (HTTPS) | Only to serve a firmware image to the firmware update entity (optional) |

A typical setup is Home Assistant OS with the **go2rtc add-on (AlexxIT)**.
No MQTT broker and no access token are needed: the board speaks the
**ESPHome native API**, so Home Assistant adopts it like any ESPHome device.

```
 door station ──RTSP──▶ go2rtc ──MJPEG (HTTP)──────────▶ ESP32-P4
   (camera +           │       ──WebRTC audio (G.711)──▶  (speaker)
    backchannel) ◀─────┘       ◀─WebRTC audio (G.711)───  (microphone)

 doorbell button ──▶ Home Assistant ──ESPHome API (TCP 6053, encrypted)──▶ ESP32-P4
                       button press, entities, logs ◀──▶ states, action calls
                       weather.get_forecasts / calendar.get_events run for the board
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

## Home Assistant

### Adopt the device

1. Once the board is on Wi-Fi, it announces itself over mDNS
   (`_esphomelib._tcp`, name `visiophone`). **Settings → Devices & services**
   shows a discovered **ESPHome** device "Visiophone": click *Add*. (If
   discovery does not show up — mDNS blocked between VLANs, for instance — add
   the ESPHome integration by hand with the board's IP, port 6053.)
2. Home Assistant generates an encryption key and sends it to the board on
   that first contact; the board stores it in NVS and from then on only
   accepts encrypted connections with that key. Nothing to type.
3. Open the device → **Configure** and tick **"Allow the device to perform
   Home Assistant actions"**. Without it the weather and calendar pages stay
   empty: the board asks HA to run `weather.get_forecasts` and
   `calendar.get_events` on its behalf, and HA silently drops those calls
   unless the option is on (the board logs a timeout after 20 s).

> Older Home Assistant releases may connect in plaintext first and then send
> the key over that connection; the board accepts both. Plaintext is refused
> as soon as a key is set.

### Entities

About thirty entities, display names in French. Entity IDs are derived from
the device and entity names (e.g. `button.visiophone_sonner`) — check the
device page for the exact IDs. Among them:

- `button` **Sonner** — the doorbell: ringtone, street-side ding-dong, call log,
  missed-call snapshot after 30 s.
- `light` Écran (screen on/off + brightness), `switch` Ne pas déranger (DND),
- `select` Page affichée (`accueil`, `meteo`, `agenda`, `appels`, `configuration`),
- `number` ring / speaker volume, mic gain,
- `switch` Caméra sans sonnerie (open the camera without ringing),
  `switch` SD log, file server, stats overlay,
- `update` Firmware (Settings → Updates, image served by HA),
- `button` reboot, OTA from SD card, acknowledge missed calls,
  presence snapshot,
- `sensor` RSSI, uptime, CPU per core, PSRAM, internal SRAM, SD free space,
  missed calls; `text_sensor` IP, firmware version, OTA status,
- `binary_sensor` Appel en cours (in call), Présence devant l'écran (presence).

### Doorbell automation

Press the **Sonner** button when your door station's button is pressed. The
trigger depends on your door station integration; for example:

```yaml
automation:
  - alias: "Doorbell → indoor screen"
    trigger:
      - platform: state
        entity_id: binary_sensor.door_station_button   # your doorbell entity
        to: "on"
    action:
      - action: button.press
        target:
          entity_id: button.visiophone_sonner           # check the real ID
```

### Screen off when nobody is around

The front camera publishes presence; the firmware does not decide anything by
itself. An automation turns the screen light off/on:

```yaml
automation:
  - alias: "Indoor screen follows presence"
    trigger:
      - platform: state
        entity_id: binary_sensor.visiophone_presence_devant_l_ecran  # check the real ID
        for: "00:02:00"
    action:
      - action: "light.turn_{{ 'on' if trigger.to_state.state == 'on' else 'off' }}"
        target:
          entity_id: light.visiophone_ecran
```

During a call presence is forced to "present", so the screen never goes dark
while someone rings.

### Weather and calendar

- Weather: any `weather.*` entity that supports `weather.get_forecasts`
  (daily). Set its ID with `APP_SECRET_HA_WEATHER_ENTITY` in
  `main/app_secrets.h`.
- Calendar: any `calendar.*` entity (Google Calendar, CalDAV, local…). Set its
  ID with `APP_SECRET_HA_CALENDAR_ENTITY`.

A wrong ID shows up in the logs as `action failed: Service call requested
response data but did not match any entities`, and the weather page stays on
"Chargement".

Both are fetched as **HA actions with a response**, through the API
connection. The board sends a small Jinja *response template* with each call
so that HA returns only what the screen shows (7 days of forecast plus the
current conditions, or the list of events). Both templates are checked in CI
by [`test/ha_templates/check_templates.py`](../test/ha_templates/check_templates.py).

### Logs

Enable *Settings → ESPHome → (device) → Configure → "Subscribe to logs"* to
copy the board's logs into the Home Assistant log, or read them from any PC
with [aioesphomeapi](https://github.com/esphome/aioesphomeapi):

```bash
pip install aioesphomeapi
aioesphomeapi-logs <board-ip> --noise-psk <key from HA>
```

The key is the `noise_psk` of the ESPHome entry in Home Assistant's
`/config/.storage/core.config_entries` (diagnostics downloads redact it).
Treat it like a password.

### Firmware served by Home Assistant (optional)

Copy `build/esp32_p4_webrtc_cam.bin` to `/config/www/fw.bin` on HA: the
"Firmware" update entity reads its version and offers it in *Settings →
Updates*; Install downloads `https://<host>:8123/local/fw.bin`. Details and the
other ways to update are in [maintenance.md](maintenance.md#from-home-assistant-update-entity).
