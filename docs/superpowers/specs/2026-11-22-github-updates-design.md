# Firmware updates from GitHub releases — design

Date: 2026-11-22. Target: v2.1.0. Status: approved in conversation ("ok parfait go").

## Intent

The repository is going public. The tablet should find and install new
firmware by itself, from the project's GitHub releases. Today the user has
to copy a `.bin` to `/config/www/fw.bin` on Home Assistant. That HA download
was also the last reason for `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`: HA
serves TLS on an IP address with a certificate that cannot be verified.
Moving to GitHub lets every TLS connection be verified again, which matters
once the code is public.

## Goals

- HA's "Firmware" update entity shows the latest GitHub release: version,
  release notes and link. "Install" downloads it and flashes it, with
  progress.
- Every TLS connection the firmware makes verifies the server certificate:
  remove `CONFIG_ESP_TLS_INSECURE` / `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`
  and use the IDF certificate bundle.
- A fork gets its own releases by changing one compile-time constant.

## Non-goals

- Signed images (secure boot v2 style). Possibly later.
- Pre-releases or release channels: `/releases/latest` only.
- Downloading to the SD card first. The image streams straight into the
  inactive OTA partition, as `esp_https_ota` already does. The previous image
  stays in the other slot, and rollback stays as it is.

## Anti-goals

- Installing an image from anything other than this repository's release
  assets over verified TLS.
- A download URL taken from the network without checking it.
- Any GitHub traffic during a call: TLS handshakes cost internal SRAM.

## Approaches considered

1. **GitHub releases only, verified TLS** — **picked**. The HA `fw.bin`
   source and the TLS skip both go. Local test builds still go through the SD
   card / file server.
2. **Signed images.** Strongest, but it needs a signing key in CI and on the
   PC, forks need their own key, and a lost key blocks updates. Deferred.
3. **Download to SD, then flash.** Rejected: the SD card shares the SDMMC
   controller with the Wi-Fi link (no SD I/O during heavy traffic), and it
   adds nothing to rollback.

## Design

### Source (`app_config.h`)

- `cfg::kGithubRepo = "Gluxur/guition-jc8012p4a1c-intercom"` (forks change
  it).
- `cfg::kGithubAsset = "esp32_p4_webrtc_cam.bin"`.
- `cfg::kGithubApiLatest` is built from the repo:
  `https://api.github.com/repos/<repo>/releases/latest`.
- Every request sends `User-Agent: esp32-intercom` (GitHub rejects requests
  without one) and `Accept: application/vnd.github+json`. No token: the
  repository is public, and the unauthenticated limit is 60 requests/hour.

### Parser (`net/github_release.{h,cpp}`, pure, host-tested)

- `bool parse_latest(const char *json, const char *repo, const char *asset,
  Release *out)`.
- It reads `tag_name`, `html_url`, `body`, and the `browser_download_url` of
  the asset named `asset`.
- It accepts the download URL only if it starts with
  `https://github.com/<repo>/releases/download/`. Anything else → false.
  This is how a URL from the network is checked.
- Release notes: the first 255 bytes of `body` at most, cut on a UTF-8
  boundary (HA's summary limit). `\r` is removed.
- Missing `tag_name`, a missing asset, or bad JSON → false.

### OTA controller (`sys/ota_ctrl`)

- **Check** (same triggers as today: HA connects, every 6 h, and
  `homeassistant.update_entity`; skipped during a call):
  - GET the API URL with the certificate bundle and the two headers, into a
    32 KB PSRAM buffer;
  - `parse_latest`, then store the `Release` under the status mutex;
  - HTTP 404 (no release yet, or repository private) → "no release" is
    stored, with no error.
- **Install** (`request_latest()`): `do_ota(asset_url)` with
  `crt_bundle_attach` and the same `User-Agent`. GitHub answers with a
  redirect to its asset storage, which `esp_https_ota` follows.
- **Project check before writing**: `esp_https_ota_get_img_desc` after
  `begin`. It aborts if `project_name` differs from the running image's.
  This replaces the 144-byte Range probe.
- The HA `fw.bin` path is removed: `settings::ota_firmware_url`,
  `cfg::kOtaFirmwarePath`, `cfg::kHaPort`, and the Range check.
- `request_file` (SD card) is unchanged.

### HA entity (`app/ha_api`, `net/esphome_api`)

- `UpdateState` gains `release_url[160]`, sent as field 10. `summary` grows
  to 256.
- The update entity's fields:
  - `latest` = release tag;
  - `summary` = release notes;
  - `release_url` = release page;
  - `title` = "Visiophone firmware".
- Install → `ota_ctrl::request_latest()`. Check →
  `ota_ctrl::request_check()`.
- Version comparison stays HA's job: `v2.1.0` vs `v2.0.0` compares as semver.
  A local dev build (`v2.0.0-13-gabc`) sorts below its release, so the
  release is offered over it, which is correct.

### Build config

- `sdkconfig`: drop `CONFIG_ESP_TLS_INSECURE` and
  `CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`. Keep
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL` (already on).
- `sdkconfig.defaults`: add `# CONFIG_ESP_TLS_INSECURE is not set` with the
  reason, so a fresh config cannot bring it back silently.

### Docs

- `maintenance.md`: the OTA section — updates from GitHub, with SD / file
  server for local builds.
- `server-setup.md`: no `/config/www` step. HA port 8123 is no longer needed
  by the board.
- `SECURITY.md`: TLS verified everywhere. OTA sources: GitHub release assets
  of `kGithubRepo`, SD card, file server.
- `getting-started.md`: `kGithubRepo` for forks.

## Tests

- **Host:** `GithubRelease.*`. Use a hand-written JSON fixture shaped like the
  GitHub API: two assets, a long UTF-8 body with `\r\n`. Cases:
  - asset picked by name;
  - wrong-repo URL rejected;
  - missing asset;
  - missing `tag_name`;
  - body cut at 255 bytes without splitting a multi-byte character;
  - `\r` stripped;
  - invalid JSON.
- **On the tablet:** the check logs the latest tag (after the repository is
  public; while private it logs 404 = no release). HA offers the update
  above a dev build, and Install flashes it, with progress. The SD-card OTA
  still works. No TLS skip is left: `grep` the built `sdkconfig`.
