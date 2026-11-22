# GitHub Release Updates Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The HA Firmware update entity installs the latest GitHub release over verified TLS. The TLS verification skip is removed.

**Architecture:**
- A pure parser picks the release and asset out of the GitHub API JSON and validates them.
- `ota_ctrl` fetches `/releases/latest` with the IDF certificate bundle and flashes the asset with `esp_https_ota`. Before writing, it checks the project name.
- The HA `fw.bin` path is deleted.

**Tech Stack:** ESP-IDF 6.0.1 (`esp_http_client`, `esp_https_ota`, `esp_crt_bundle`), cJSON (`espressif__cjson`), GoogleTest host tests.

**Spec:** `docs/superpowers/specs/2026-11-22-github-updates-design.md`

## Global Constraints

- **Repo:** `D:\HomeAssistant\esp32-p4-video-intercom`, branch `feat/github-updates`.
- **Commits:** author Gluxur (local config). Messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. No personal data.
- **Constants:**
  - `cfg::kGithubRepo = "Gluxur/guition-jc8012p4a1c-intercom"`;
  - `cfg::kGithubAsset = "esp32_p4_webrtc_cam.bin"`;
  - User-Agent `esp32-intercom`;
  - Accept `application/vnd.github+json`.
- **Allowed download URL prefix:** `https://github.com/<repo>/releases/download/`.
- **Release notes:** ≤ 255 bytes, cut on a UTF-8 boundary, `\r` removed.
- **Code rules:** CLAUDE.md (no `ESP_ERROR_CHECK`, PSRAM for buffers > 4 KB, atomics, no SD I/O in hot paths).
- **Escapes:** write C sources with backslash escapes via the Write tool (shell heredocs corrupt `\\0`).
- **Host tests:** `cd test ; pwsh -NoProfile -File run.ps1`.
- **Template check:** `python test/ha_templates/check_templates.py .`
- **Firmware build:** scratchpad `build.ps1`.

## Review Focus

1. **GitHub answers 403 with a rate-limit JSON, or 404 while the repository is private.** The check stores "no release", logs one line, and never crashes or retries in a loop. Pinned by: Task 1, `GithubRelease.RejectsNonReleaseJson` (`{"message":"Not Found"}` → false).
2. **The body contains a multi-byte character exactly at byte 255.** The summary is not cut mid-character, because HA would reject invalid UTF-8. Pinned by: `GithubRelease.NotesCutOnUtf8Boundary`.
3. **The asset URL points to another repository or host.** It is refused. Pinned by: `GithubRelease.RejectsForeignAssetUrl`.
4. **Install is pressed when no release is known.** Nothing is downloaded and the status says why. Code path in Task 2: `request_latest()` returns false.
5. **Release JSON larger than the buffer** (long notes). The read fails cleanly, with no overflow. The 32 KB cap is checked in Task 2 (`len >= cap` → fail).

---

### Task 1: Pure GitHub release parser

**Files:**
- Create: `main/net/github_release.h`, `main/net/github_release.cpp`, `test/test_github_release.cpp`
- Modify: `test/CMakeLists.txt` (test + `${APP_DIR}/net/github_release.cpp`), `main/CMakeLists.txt` (SRCS)

**Interfaces:**
- Produces, in namespace `github_release`:
  - `struct Release { char tag[32]; char notes[256]; char html_url[160]; char asset_url[256]; };`
  - `bool parse_latest(const char *json, const char *repo, const char *asset, Release *out);`

- [ ] **Step 1:** Write the tests, each with a hand-written fixture.
  - `PicksAssetByName`: assets `foo.bin` and `esp32_p4_webrtc_cam.bin` → the second one's URL.
  - `ReadsTagAndHtmlUrl`.
  - `RejectsForeignAssetUrl`: the URL is under `https://github.com/evil/x/releases/download/` → false.
  - `RejectsMissingAsset`, `RejectsMissingTag`.
  - `RejectsNonReleaseJson`: `{"message":"Not Found"}`, `not json`, `nullptr` → false.
  - `StripsCarriageReturns`: `"a\r\nb"` → `"a\nb"`.
  - `NotesCutOnUtf8Boundary`: 254 × `a` followed by `é` (2 bytes) → 254 bytes, no trailing partial byte.
- [ ] **Step 2:** Run the tests; expect them to fail.
- [ ] **Step 3:** Implement with cJSON.
- [ ] **Step 4:** Tests pass. Mutate the prefix check, see `RejectsForeignAssetUrl` fail, restore.
- [ ] **Step 5:** Commit `feat(ota): pure GitHub latest-release parser`.

### Task 2: OTA from GitHub, verified TLS

**Files:**
- Modify: `main/sys/ota_ctrl.{h,cpp}`, `main/app_config.h`, `main/sys/settings.{h,cpp}` (drop `ota_firmware_url`)
- Modify: `sdkconfig` (drop `ESP_TLS_INSECURE` / `SKIP_SERVER_CERT_VERIFY`), `sdkconfig.defaults` (add the not-set line with a comment)

**Interfaces:**
- Consumes: Task 1.
- Produces, in namespace `ota_ctrl`:
  - `void request_check();`: no argument; uses the GitHub API URL.
  - `bool request_latest();`: false if no release is known or an OTA is busy.
  - `bool latest(github_release::Release *out);`: false until a check succeeds. `tag` empty = no release.
  - `latest(ImageInfo*)` is removed, and so is `request_check(url)`.
- `do_ota(url)`:
  - attaches `esp_crt_bundle_attach` and sets the User-Agent through `http_client_init_cb`;
  - after `esp_https_ota_begin`, calls `esp_https_ota_get_img_desc`;
  - a `project_name` that differs from the running one → abort with `error: other project`.
- Check: a GET into a 32 KB PSRAM buffer. Results:
  - 200 → parse;
  - 404 → store an empty release;
  - anything else → keep the previous release and log the HTTP code;
  - `len >= cap` → fail.

- [ ] **Step 1:** Implement.
- [ ] **Step 2:** Build. `grep SKIP_SERVER_CERT build/config/sdkconfig.h` finds nothing. Host tests are green.
- [ ] **Step 3:** Commit `feat(ota): updates from GitHub releases over verified TLS`.

### Task 3: HA entity and docs

**Files:**
- Modify: `main/net/esphome_api.{h,cpp}` (`UpdateState.release_url[160]` → field 10, `summary[256]`)
- Modify: `main/app/ha_api.cpp` (`g_update` / `c_update` / `on_ha_ready` use Task 2)
- Modify: `docs/maintenance.md`, `docs/server-setup.md`, `docs/getting-started.md`, `SECURITY.md`, `README.md`

- [ ] **Step 1:** Implement, then build.
- [ ] **Step 2:** Commit `feat(ha): update entity offers the latest GitHub release` and `docs: firmware updates from GitHub releases`.

### Task 4: Flash and verify

- [ ] Reconfigure, build, and OTA via the file server.
- [ ] In the SD log: the check logs 404 (repository still private), or the tag once public.
- [ ] Push the branch, CI green on both workflows.
