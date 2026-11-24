#include "sd_image.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sys/bmp_header.h"
#include "sys/usb_msc.h"

namespace sd_image {
namespace {

constexpr const char *TAG = "sd_image";

// Noms courts (horodatage + extension) : 32 suffit, et borne la liste.
constexpr size_t kNameMax = 32;
// Au-delà, le répertoire n'est plus le nôtre : on ne trie pas l'inconnu.
constexpr int kMaxEntries = 256;

bool has_ext(const char *name, const char *ext) {
  const size_t n = strlen(name);
  const size_t e = strlen(ext);
  return n > e && strcmp(name + n - e, ext) == 0;
}

} // namespace

bool sd_ready() {
  return bsp::Bsp::instance().sd_state() == bsp::SdState::kMounted &&
         usb_msc::status() != usb_msc::Status::kActive;
}

bool write_pgm(const char *path, const uint8_t *gray, int w, int h,
               const char *comment) {
  if (path == nullptr || gray == nullptr || w <= 0 || h <= 0) {
    return false;
  }
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    ESP_LOGE(TAG, "ouverture %s impossible (errno %d)", path, errno);
    return false;
  }
  char hdr[160];
  const int n =
      (comment != nullptr && comment[0] != '\0')
          ? snprintf(hdr, sizeof hdr, "P5\n# %s\n%d %d\n255\n", comment, w, h)
          : snprintf(hdr, sizeof hdr, "P5\n%d %d\n255\n", w, h);
  const size_t px = static_cast<size_t>(w) * h;
  bool ok = n > 0 && n < static_cast<int>(sizeof hdr) &&
            write(fd, hdr, n) == n;
  ok = ok && write(fd, gray, px) == static_cast<ssize_t>(px);
  close(fd);
  if (!ok) {
    ESP_LOGE(TAG, "écriture %s : échec", path);
  }
  return ok;
}

bool write_bmp565(const char *path, const uint8_t *px, int w, int h,
                  size_t stride) {
  if (path == nullptr || px == nullptr || w <= 0 || h <= 0 ||
      stride < static_cast<size_t>(w) * 2) {
    return false;
  }
  const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    ESP_LOGE(TAG, "ouverture %s impossible (errno %d)", path, errno);
    return false;
  }
  uint8_t hdr[bmp::kRgb565HeaderBytes];
  bmp::rgb565_header(hdr, w, h);
  bool ok = write(fd, hdr, sizeof hdr) == static_cast<ssize_t>(sizeof hdr);
  const size_t data = static_cast<size_t>(w) * 2;
  const size_t pad = bmp::rgb565_row_bytes(w) - data;
  static constexpr uint8_t kZeros[4] = {};
  for (int y = 0; ok && y < h; y++) {
    ok = write(fd, px + static_cast<size_t>(y) * stride, data) ==
         static_cast<ssize_t>(data);
    if (ok && pad != 0) {
      ok = write(fd, kZeros, pad) == static_cast<ssize_t>(pad);
    }
  }
  close(fd);
  if (!ok) {
    ESP_LOGE(TAG, "écriture %s : échec", path);
  }
  return ok;
}

bool make_room(const char *dir, const char *ext, int keep) {
  if (dir == nullptr || ext == nullptr || keep < 1) {
    return false;
  }
  struct stat st = {};
  if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0) {
    ESP_LOGE(TAG, "mkdir %s impossible (errno %d)", dir, errno);
    return false;
  }
  DIR *d = opendir(dir);
  if (d == nullptr) {
    return false;
  }
  // Liste en PSRAM : 256 x 32 o, hors de la pile de l'appelant.
  auto *names = static_cast<char(*)[kNameMax]>(
      heap_caps_malloc(kMaxEntries * kNameMax, MALLOC_CAP_SPIRAM));
  if (names == nullptr) {
    closedir(d);
    return false;
  }
  int count = 0;
  while (const dirent *e = readdir(d)) {
    if (count >= kMaxEntries) {
      break;
    }
    if (!has_ext(e->d_name, ext) || strlen(e->d_name) >= kNameMax) {
      continue;
    }
    memcpy(names[count], e->d_name, strlen(e->d_name) + 1);
    count++;
  }
  closedir(d);
  if (count >= keep) {
    qsort(names, count, kNameMax, [](const void *a, const void *b) {
      return strcmp(static_cast<const char *>(a), static_cast<const char *>(b));
    });
    char path[96];
    for (int i = 0; i <= count - keep; i++) {
      snprintf(path, sizeof path, "%s/%s", dir, names[i]);
      if (unlink(path) != 0) {
        ESP_LOGW(TAG, "suppression %s impossible", path);
      }
    }
  }
  heap_caps_free(names);
  return true;
}

} // namespace sd_image
