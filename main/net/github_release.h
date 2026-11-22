#pragma once

// Decoding of GET /repos/{owner}/{repo}/releases/latest (GitHub REST API).
// Pure: cJSON only, host-tested (test/test_github_release). The HTTP side
// lives in sys/ota_ctrl.
namespace github_release {

struct Release {
  char tag[32];        // "v2.1.0"; also the version HA compares
  char notes[256];     // release body, <= 255 bytes, cut on a UTF-8 boundary
  char html_url[160];  // release page, shown by HA
  char asset_url[256]; // firmware image to flash
};

// True iff `json` is a release with a non-empty tag_name and an asset named
// `asset` whose browser_download_url starts with
// "https://github.com/<repo>/releases/download/". That prefix check is what
// makes a URL received from the network safe to flash from: anything else
// (another repository, another host, plain http) is refused. `out` is only
// meaningful on true.
[[nodiscard]] bool parse_latest(const char *json, const char *repo,
                                const char *asset, Release *out);

} // namespace github_release
