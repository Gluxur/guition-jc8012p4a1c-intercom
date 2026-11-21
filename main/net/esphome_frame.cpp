#include "esphome_frame.h"

#include <cstdio>
#include <cstring>

#include "net/esphome_pb.h"

namespace esphome_frame {

size_t write_plain_header(uint8_t *out, size_t cap, uint32_t len, uint32_t type) {
  if (cap < 1) {
    return 0;
  }
  out[0] = kPlainIndicator;
  const size_t a = esphome_pb::encode_varint(len, out + 1, cap - 1);
  if (a == 0) {
    return 0;
  }
  const size_t b = esphome_pb::encode_varint(type, out + 1 + a, cap - 1 - a);
  if (b == 0) {
    return 0;
  }
  return 1 + a + b;
}

Parse parse_plain_header(const uint8_t *p, size_t n, size_t *header_len,
                         uint32_t *payload_len, uint32_t *type) {
  if (n < 1) {
    return Parse::kNeedMore;
  }
  if (p[0] != kPlainIndicator) {
    return Parse::kBad;
  }
  uint64_t len = 0, t = 0;
  const int a = esphome_pb::decode_varint(p + 1, n - 1, &len);
  if (a < 0 || len > UINT32_MAX) {
    return Parse::kBad;
  }
  if (a == 0) {
    return Parse::kNeedMore;
  }
  const int b = esphome_pb::decode_varint(p + 1 + a, n - 1 - a, &t);
  if (b < 0 || t > UINT16_MAX) {
    return Parse::kBad;
  }
  if (b == 0) {
    return Parse::kNeedMore;
  }
  *header_len = 1 + (size_t)a + (size_t)b;
  *payload_len = (uint32_t)len;
  *type = (uint32_t)t;
  return Parse::kOk;
}

void write_noise_header(uint8_t *out, uint16_t len) {
  out[0] = kNoiseIndicator;
  put_be16(out + 1, len);
}

Parse parse_noise_header(const uint8_t *p, size_t n, uint16_t *len) {
  if (n < 1) {
    return Parse::kNeedMore;
  }
  if (p[0] != kNoiseIndicator) {
    return Parse::kBad;
  }
  if (n < kNoiseHeaderLen) {
    return Parse::kNeedMore;
  }
  *len = get_be16(p + 1);
  return Parse::kOk;
}

size_t write_server_hello(uint8_t *out, size_t cap, const char *name,
                          const uint8_t mac[6]) {
  const size_t nl = strlen(name);
  const size_t total = 1 + nl + 1 + 12 + 1;
  if (cap < total) {
    return 0;
  }
  out[0] = 0x01;
  memcpy(out + 1, name, nl + 1);
  char *m = reinterpret_cast<char *>(out + 1 + nl + 1);
  for (int i = 0; i < 6; i++) {
    snprintf(m + 2 * i, 3, "%02x", mac[i]);
  }
  return total;
}

namespace {
int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') {
    return c - 'A';
  }
  if (c >= 'a' && c <= 'z') {
    return c - 'a' + 26;
  }
  if (c >= '0' && c <= '9') {
    return c - '0' + 52;
  }
  if (c == '+') {
    return 62;
  }
  if (c == '/') {
    return 63;
  }
  return -1;
}
} // namespace

int decode_base64(const char *in, size_t n, uint8_t *out, size_t cap) {
  if (n % 4 != 0) {
    return -1;
  }
  size_t o = 0;
  for (size_t i = 0; i < n; i += 4) {
    const bool last = i + 4 == n;
    int v[4];
    int pad = 0;
    for (int j = 0; j < 4; j++) {
      const char c = in[i + j];
      if (c == '=' && last && j >= 2) {
        v[j] = 0;
        pad++;
        continue;
      }
      if (pad != 0) {
        return -1; // data after '='
      }
      v[j] = b64_value(c);
      if (v[j] < 0) {
        return -1;
      }
    }
    const uint32_t triple = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12) |
                            ((uint32_t)v[2] << 6) | (uint32_t)v[3];
    const int bytes = 3 - pad;
    if (o + (size_t)bytes > cap) {
      return -1;
    }
    out[o++] = (uint8_t)(triple >> 16);
    if (bytes > 1) {
      out[o++] = (uint8_t)(triple >> 8);
    }
    if (bytes > 2) {
      out[o++] = (uint8_t)triple;
    }
  }
  return (int)o;
}

} // namespace esphome_frame
