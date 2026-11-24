#include "net/github_release.h"

#include <cstdio>
#include <cstring>

#include "cJSON.h"

namespace github_release {
namespace {

const char *str_of(const cJSON *obj, const char *key) {
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
  return cJSON_IsString(v) ? v->valuestring : nullptr;
}

// Copies `src` without '\r', at most cap-1 bytes, never ending inside a
// multi-byte UTF-8 sequence (HA refuses invalid UTF-8 in the summary).
void copy_notes(const char *src, char *dst, size_t cap) {
  size_t n = 0;
  for (const char *p = src; *p != '\0' && n + 1 < cap; p++) {
    if (*p != '\r') {
      dst[n++] = *p;
    }
  }
  dst[n] = '\0';
  if (src[0] == '\0' || n == 0) {
    return;
  }
  // Was the copy cut? Only then can the last sequence be incomplete.
  size_t kept = 0;
  for (const char *p = src; *p != '\0'; p++) {
    if (*p != '\r') {
      kept++;
    }
  }
  if (kept <= n) {
    return;
  }
  // Back up over continuation bytes (10xxxxxx), then drop the lead byte if
  // its sequence does not fit entirely.
  size_t i = n;
  while (i > 0 && ((unsigned char)dst[i - 1] & 0xC0) == 0x80) {
    i--;
  }
  if (i > 0) {
    const unsigned char lead = (unsigned char)dst[i - 1];
    size_t need = 1;
    if ((lead & 0xE0) == 0xC0) {
      need = 2;
    } else if ((lead & 0xF0) == 0xE0) {
      need = 3;
    } else if ((lead & 0xF8) == 0xF0) {
      need = 4;
    }
    if (n - (i - 1) < need) {
      n = i - 1;
    }
  }
  dst[n] = '\0';
}

// One release object. Shared by /releases/latest (an object) and /releases
// (an array of them), so both channels apply the same URL prefix check.
bool parse_one(const cJSON *root, const char *repo, const char *asset,
               Release *out) {
  const char *tag = str_of(root, "tag_name");
  const cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
  if (tag == nullptr || tag[0] == '\0' || strlen(tag) >= sizeof out->tag ||
      !cJSON_IsArray(assets)) {
    return false;
  }
  char prefix[160];
  snprintf(prefix, sizeof prefix, "https://github.com/%s/releases/download/",
           repo);
  const cJSON *a = nullptr;
  cJSON_ArrayForEach(a, assets) {
    const char *name = str_of(a, "name");
    const char *url = str_of(a, "browser_download_url");
    if (name == nullptr || url == nullptr || strcmp(name, asset) != 0) {
      continue;
    }
    if (strncmp(url, prefix, strlen(prefix)) != 0 ||
        strlen(url) >= sizeof out->asset_url) {
      return false;
    }
    snprintf(out->tag, sizeof out->tag, "%s", tag);
    snprintf(out->asset_url, sizeof out->asset_url, "%s", url);
    const char *html = str_of(root, "html_url");
    snprintf(out->html_url, sizeof out->html_url, "%s",
             html != nullptr ? html : "");
    const char *body = str_of(root, "body");
    copy_notes(body != nullptr ? body : "", out->notes, sizeof out->notes);
    return true;
  }
  return false;
}

} // namespace

bool parse_latest(const char *json, const char *repo, const char *asset,
                  Release *out) {
  if (json == nullptr || repo == nullptr || asset == nullptr || out == nullptr) {
    return false;
  }
  cJSON *root = cJSON_Parse(json);
  if (root == nullptr) {
    return false;
  }
  const bool ok = parse_one(root, repo, asset, out);
  cJSON_Delete(root);
  return ok;
}

bool parse_newest(const char *json, const char *repo, const char *asset,
                  Release *out) {
  if (json == nullptr || repo == nullptr || asset == nullptr || out == nullptr) {
    return false;
  }
  cJSON *root = cJSON_Parse(json);
  if (root == nullptr) {
    return false;
  }
  bool ok = false;
  if (cJSON_IsArray(root)) {
    const cJSON *rel = nullptr;
    cJSON_ArrayForEach(rel, root) {
      if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(rel, "draft"))) {
        continue;
      }
      if (parse_one(rel, repo, asset, out)) {
        ok = true;
        break;
      }
    }
  }
  cJSON_Delete(root);
  return ok;
}

} // namespace github_release
