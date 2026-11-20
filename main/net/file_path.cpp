#include "file_path.h"

#include "app_config.h"

#include <cstring>

namespace file_path {
namespace {

int hexval(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

} // namespace

bool sanitize(const char *raw, char *out, size_t out_cap) {
  if (raw == nullptr || out == nullptr || out_cap == 0) {
    return false;
  }

  size_t w = 0;
  for (const char *p = raw; *p != '\0'; ++p) {
    char c = *p;
    if (c == '%') {
      // hexval('\0') vaut -1 : le test sur `hi` protège la lecture de p[2].
      const int hi = hexval(p[1]);
      if (hi < 0) {
        return false;
      }
      const int lo = hexval(p[2]);
      if (lo < 0) {
        return false;
      }
      c = static_cast<char>((hi << 4) | lo);
      p += 2;
    }
    // Le caractère décodé est écrit tel quel, sans repasser par la détection
    // de '%' : un "%252e" donne le texte "%2e" et jamais un point. Un second
    // tour de décodage rouvrirait exactement la remontée qu'on bloque plus bas.
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\') {
      return false;
    }
    if (w + 1 >= out_cap) {
      return false;
    }
    out[w++] = c;
  }
  out[w] = '\0';

  const size_t root_len = strlen(cfg::sd::kMountPoint);
  if (w < root_len || memcmp(out, cfg::sd::kMountPoint, root_len) != 0) {
    return false;
  }
  // Sans ce test, "/sdcardXYZ" passerait le memcmp de préfixe.
  if (out[root_len] != '\0' && out[root_len] != '/') {
    return false;
  }

  for (const char *seg = out; *seg != '\0';) {
    if (*seg == '/') {
      ++seg;
      continue;
    }
    const char *end = seg;
    while (*end != '\0' && *end != '/') {
      ++end;
    }
    if (end - seg == 2 && seg[0] == '.' && seg[1] == '.') {
      return false;
    }
    seg = end;
  }
  return true;
}

} // namespace file_path
