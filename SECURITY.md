# Security

This firmware is designed for a **trusted home LAN**. It is not hardened for
exposure to the internet, and its threat model is "people already on your
Wi-Fi are trusted". Known trade-offs, by design:

| Area | Behaviour | Mitigation |
|---|---|---|
| MQTT | Plain TCP (`mqtt://host:1883`), credentials sent in clear | Dedicated broker user with minimal ACLs; keep the broker off the internet |
| Home Assistant API | HTTPS, but the server certificate is **not verified** (`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`) — HA usually runs with a self-signed certificate | LAN only; use a dedicated HA user and a long-lived token you can revoke |
| OTA over MQTT | Only accepts firmware URLs on the configured host (`visio/ota/cmd`), images are checked for a valid app header; no signature check | Restrict who can publish on `visio/#` |
| File server (port 80) | **No authentication**: list, download, upload, delete files on the SD card and trigger an OTA from a file | **Off by default.** Enable it only for a maintenance session (`visio/file_server/set` = `ON`) and switch it off afterwards — the setting is persisted |
| USB mass-storage mode | Exposes the whole SD card (call snapshots, logs) to the connected PC | Entered only from the touch screen |
| Call snapshots | JPEG pictures of visitors on the SD card | They never leave the device unless you download them; mind local privacy rules about recording a public street |
| Secrets in the source | None. Compiled defaults come from the git-ignored `main/app_secrets.h`; runtime values live in NVS (not encrypted) | Do not commit `app_secrets.h`; enable NVS encryption / flash encryption if physical access to the device is a concern |

## Reporting a vulnerability

Please open a GitHub issue **without** exploit details and ask for a private
contact, or use GitHub's private vulnerability reporting if it is enabled on
the repository.
