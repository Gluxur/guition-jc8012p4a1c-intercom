#pragma once

// Frame layer of the ESPHome native API (TCP port 6053). Two encodings share
// the socket, told apart by the first byte of every frame:
//   0x00 plaintext : 0x00, varint(payload len), varint(msg type), payload
//   0x01 Noise     : 0x01, be16(len), len bytes (handshake data, or once
//                    encrypted: be16(type), be16(payload len), payload, MAC)
// Pure byte arithmetic, tested on the host (test/test_esphome_frame.cpp).

#include <cstddef>
#include <cstdint>

namespace esphome_frame {

constexpr uint8_t kPlainIndicator = 0x00;
constexpr uint8_t kNoiseIndicator = 0x01;
constexpr size_t kNoiseHeaderLen = 3;
constexpr size_t kInnerHeaderLen = 4; // type + len inside a Noise frame
constexpr size_t kPlainHeaderMax = 1 + 5 + 5;

enum class Parse : uint8_t {
  kNeedMore, // header incomplete: wait for more bytes
  kOk,
  kBad, // wrong indicator or malformed varint: close the connection
};

// Writes the plaintext header for a payload of `len` bytes of message `type`.
// Returns the header length (3..11), 0 if `cap` is too small.
size_t write_plain_header(uint8_t *out, size_t cap, uint32_t len, uint32_t type);

Parse parse_plain_header(const uint8_t *p, size_t n, size_t *header_len,
                         uint32_t *payload_len, uint32_t *type);

void write_noise_header(uint8_t *out, uint16_t len);
Parse parse_noise_header(const uint8_t *p, size_t n, uint16_t *len);

inline void put_be16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}
inline uint16_t get_be16(const uint8_t *p) {
  return (uint16_t)((p[0] << 8) | p[1]);
}

// ServerHello payload of the Noise transport: 0x01 (chosen protocol), node
// name, NUL, MAC as 12 lowercase hex digits, NUL. Clients use the name and MAC
// to pick the right key and to report "wrong device" instead of "wrong key".
// Returns the length, 0 if `cap` is too small.
size_t write_server_hello(uint8_t *out, size_t cap, const char *name,
                          const uint8_t mac[6]);

// Standard base64 (RFC 4648, with padding). NoiseEncryptionSetKeyRequest
// carries the new 32-byte key as base64 text. Returns the decoded length, or
// -1 on a bad character, bad padding or when `cap` is too small.
int decode_base64(const char *in, size_t n, uint8_t *out, size_t cap);

} // namespace esphome_frame
