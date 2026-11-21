#include "esphome_pb.h"

#include <cstring>

namespace esphome_pb {

size_t encode_varint(uint64_t v, uint8_t *out, size_t cap) {
  size_t n = 0;
  do {
    if (n >= cap) {
      return 0;
    }
    uint8_t b = v & 0x7F;
    v >>= 7;
    if (v != 0) {
      b |= 0x80;
    }
    out[n++] = b;
  } while (v != 0);
  return n;
}

int decode_varint(const uint8_t *p, size_t n, uint64_t *out) {
  uint64_t v = 0;
  for (size_t i = 0; i < 10; i++) {
    if (i >= n) {
      return 0;
    }
    v |= (uint64_t)(p[i] & 0x7F) << (7 * i);
    if ((p[i] & 0x80) == 0) {
      *out = v;
      return (int)(i + 1);
    }
  }
  return -1;
}

// --- Writer ----------------------------------------------------------------

void Writer::raw(const void *p, size_t n) {
  if (!ok_) {
    return;
  }
  if (cap_ - len_ < n) {
    ok_ = false;
    return;
  }
  if (n != 0) {
    memcpy(buf_ + len_, p, n);
  }
  len_ += n;
}

void Writer::raw_varint(uint64_t v) {
  uint8_t tmp[10];
  const size_t n = encode_varint(v, tmp, sizeof tmp);
  raw(tmp, n);
}

void Writer::tag(uint32_t field, Wire w) {
  raw_varint(((uint64_t)field << 3) | (uint64_t)w);
}

void Writer::u32(uint32_t field, uint32_t v) {
  if (v == 0) {
    return;
  }
  tag(field, Wire::kVarint);
  raw_varint(v);
}

void Writer::i32(uint32_t field, int32_t v) {
  if (v == 0) {
    return;
  }
  tag(field, Wire::kVarint);
  // proto3 int32: sign-extended to 64 bits, as protoc does.
  raw_varint((uint64_t)(int64_t)v);
}

void Writer::boolean(uint32_t field, bool v) {
  if (!v) {
    return;
  }
  tag(field, Wire::kVarint);
  raw_varint(1);
}

void Writer::fixed32(uint32_t field, uint32_t v) {
  if (v == 0) {
    return;
  }
  tag(field, Wire::kFixed32);
  const uint8_t le[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                         (uint8_t)(v >> 24)};
  raw(le, sizeof le);
}

void Writer::f32(uint32_t field, float v) {
  uint32_t bits;
  static_assert(sizeof bits == sizeof v, "float must be 32 bits");
  memcpy(&bits, &v, sizeof bits);
  // +0.0f is the default; -0.0f has the sign bit and is written.
  fixed32(field, bits);
}

void Writer::str(uint32_t field, const char *s) {
  if (s == nullptr || s[0] == '\0') {
    return;
  }
  bytes(field, s, strlen(s));
}

void Writer::bytes(uint32_t field, const void *p, size_t n) {
  if (n == 0) {
    return;
  }
  bytes_always(field, p, n);
}

void Writer::bytes_always(uint32_t field, const void *p, size_t n) {
  tag(field, Wire::kLength);
  raw_varint(n);
  raw(p, n);
}

// --- Reader ----------------------------------------------------------------

bool Reader::read_varint(uint64_t *out) {
  const int n = decode_varint(p_, (size_t)(end_ - p_), out);
  if (n <= 0) {
    ok_ = false;
    return false;
  }
  p_ += n;
  return true;
}

bool Reader::next() {
  if (!ok_ || p_ >= end_) {
    return false;
  }
  uint64_t key;
  if (!read_varint(&key)) {
    return false;
  }
  field_ = (uint32_t)(key >> 3);
  wire_ = (Wire)(key & 7);
  if (field_ == 0) {
    ok_ = false; // field number 0 is invalid in protobuf
    return false;
  }
  switch (wire_) {
  case Wire::kVarint:
    return read_varint(&value_);
  case Wire::kFixed32:
    if (end_ - p_ < 4) {
      ok_ = false;
      return false;
    }
    value_ = (uint64_t)p_[0] | ((uint64_t)p_[1] << 8) |
             ((uint64_t)p_[2] << 16) | ((uint64_t)p_[3] << 24);
    p_ += 4;
    return true;
  case Wire::kFixed64:
    if (end_ - p_ < 8) {
      ok_ = false;
      return false;
    }
    value_ = 0;
    for (int i = 7; i >= 0; i--) {
      value_ = (value_ << 8) | p_[i];
    }
    p_ += 8;
    return true;
  case Wire::kLength: {
    uint64_t n;
    if (!read_varint(&n)) {
      return false;
    }
    if (n > (uint64_t)(end_ - p_)) {
      ok_ = false;
      return false;
    }
    lp_ = p_;
    ln_ = (size_t)n;
    p_ += n;
    return true;
  }
  default:
    // Groups (3/4) are not used by api.proto: anything else is corruption.
    ok_ = false;
    return false;
  }
}

uint64_t Reader::as_u64() const {
  return wire_ == Wire::kVarint ? value_ : 0;
}

uint32_t Reader::as_fixed32() const {
  return wire_ == Wire::kFixed32 ? (uint32_t)value_ : 0;
}

float Reader::as_f32() const {
  const uint32_t bits = as_fixed32();
  float f;
  memcpy(&f, &bits, sizeof f);
  return f;
}

const uint8_t *Reader::as_bytes(size_t *len) const {
  if (wire_ != Wire::kLength) {
    *len = 0;
    return nullptr;
  }
  *len = ln_;
  return lp_;
}

} // namespace esphome_pb
