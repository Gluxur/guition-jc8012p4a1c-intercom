# Security

This firmware is designed for a **trusted home LAN**. It is not hardened for
exposure to the internet, and its threat model is "people already on your
Wi-Fi are trusted". Known trade-offs, by design:

| Area | Behaviour | Mitigation |
|---|---|---|
| ESPHome API (TCP 6053) | Encrypted (Noise, ChaCha20-Poly1305) with the key Home Assistant provisions. **Until a key is set** the board also accepts plaintext and the publicly known all-zero key, exactly like ESPHome: whoever adopts it first gets it | Adopt the board in HA as soon as it is on the network. Once provisioned, anything without the key is rejected. The key sits in NVS (not encrypted) |
| HA actions | With "Allow the device to perform Home Assistant actions" on, the board may ask HA to run **any** action (it only asks for `weather.get_forecasts` and `calendar.get_events`) | Enable the option only for this device; a compromised board could abuse it, as any ESPHome device with the option could |
| API clients | Any client holding the key can press every button: ring, reboot, OTA from the SD card, or install the latest GitHub release through the update entity | Images must carry this firmware's project name; no signature check |
| OTA download | From the GitHub release assets of `cfg::kGithubRepo` only, over HTTPS **with certificate verification** (ESP-IDF bundle; `CONFIG_ESP_TLS_INSECURE` is off, so no TLS connection of the firmware skips it) | The download URL comes from the GitHub API but is accepted only under `https://github.com/<repo>/releases/download/`. A compromised GitHub account could still publish a malicious release: image signing is not implemented |
| File server (port 80) | **No authentication**: list, download, upload, delete files on the SD card and trigger an OTA from a file | **Off by default.** Enable it only for a maintenance session (switch "Serveur de fichiers") and switch it off afterwards — the setting is persisted |
| USB mass-storage mode | Exposes the whole SD card (call snapshots, logs) to the connected PC | Entered only from the touch screen |
| Call snapshots | JPEG pictures of visitors on the SD card | They never leave the device unless you download them; mind local privacy rules about recording a public street |
| Secrets in the source | None. Compiled defaults come from the git-ignored `main/app_secrets.h`; runtime values live in NVS (not encrypted) | Do not commit `app_secrets.h`; enable NVS encryption / flash encryption if physical access to the device is a concern |

## Reporting a vulnerability

Please open a GitHub issue **without** exploit details and ask for a private
contact, or use GitHub's private vulnerability reporting if it is enabled on
the repository.
