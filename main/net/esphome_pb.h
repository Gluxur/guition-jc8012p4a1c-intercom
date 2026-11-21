#pragma once

// Minimal protobuf (proto3) wire codec for the ESPHome native API.
//
// Only what api.proto needs from us: varint, fixed32/float, and
// length-delimited fields (strings, bytes, nested messages). No reflection,
// no allocation: the writer fills a caller buffer, the reader walks one.
// Pure logic, compiled and tested on the host (test/test_esphome_pb.cpp).

#include <cstddef>
#include <cstdint>

namespace esphome_pb {

enum class Wire : uint8_t {
  kVarint = 0,
  kFixed64 = 1,
  kLength = 2,
  kFixed32 = 5,
};

// Appends fields to a fixed-capacity buffer. Once a write does not fit, the
// writer is marked failed and every later call is a no-op: callers check ok()
// once at the end instead of after every field.
class Writer {
public:
  Writer(uint8_t *buf, size_t cap) : buf_(buf), cap_(cap) {}

  // proto3 semantics: a field equal to its default (0, false, "") is not
  // written. HA's decoder fills the default back in.
  void u32(uint32_t field, uint32_t v);
  void i32(uint32_t field, int32_t v); // int32: negative = 10-byte varint
  void boolean(uint32_t field, bool v);
  void fixed32(uint32_t field, uint32_t v);
  void f32(uint32_t field, float v);
  void str(uint32_t field, const char *s); // nullptr == ""
  void bytes(uint32_t field, const void *p, size_t n);
  // Written even when empty: used for repeated strings (select options),
  // where an empty element is still an element.
  void bytes_always(uint32_t field, const void *p, size_t n);

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] size_t size() const { return len_; }
  [[nodiscard]] const uint8_t *data() const { return buf_; }

private:
  void tag(uint32_t field, Wire w);
  void raw_varint(uint64_t v);
  void raw(const void *p, size_t n);

  uint8_t *buf_;
  size_t cap_;
  size_t len_ = 0;
  bool ok_ = true;
};

// Iterates the fields of one encoded message. Typical use:
//   Reader r(data, len);
//   while (r.next()) switch (r.field()) { case 1: x = r.as_u32(); break; ... }
//   if (!r.ok()) -> malformed
// next() consumes each field's value, so a field the caller's switch does not
// handle is simply ignored: newer clients can add fields without breaking us.
class Reader {
public:
  Reader(const uint8_t *p, size_t n) : p_(p), end_(p + n) {}

  [[nodiscard]] bool next();
  [[nodiscard]] uint32_t field() const { return field_; }
  [[nodiscard]] Wire wire() const { return wire_; }

  // Value accessors for the field next() just stopped on. A type mismatch
  // (e.g. as_u32 on a length field) returns 0/empty: proto3 tolerates it.
  [[nodiscard]] uint64_t as_u64() const;
  [[nodiscard]] uint32_t as_u32() const { return (uint32_t)as_u64(); }
  [[nodiscard]] bool as_bool() const { return as_u64() != 0; }
  [[nodiscard]] uint32_t as_fixed32() const;
  [[nodiscard]] float as_f32() const;
  // Length-delimited payload (NOT NUL-terminated).
  [[nodiscard]] const uint8_t *as_bytes(size_t *len) const;

  [[nodiscard]] bool ok() const { return ok_; }

private:
  bool read_varint(uint64_t *out);

  const uint8_t *p_;
  const uint8_t *end_;
  uint32_t field_ = 0;
  Wire wire_ = Wire::kVarint;
  uint64_t value_ = 0;         // varint / fixed32 / fixed64
  const uint8_t *lp_ = nullptr; // length-delimited start
  size_t ln_ = 0;
  bool ok_ = true;
};

// Raw varint helpers for the plaintext frame header.
// encode: returns bytes written (1..10), 0 if `cap` is too small.
size_t encode_varint(uint64_t v, uint8_t *out, size_t cap);
// decode: returns bytes consumed, 0 if incomplete, -1 if longer than 10 bytes.
int decode_varint(const uint8_t *p, size_t n, uint64_t *out);

} // namespace esphome_pb
