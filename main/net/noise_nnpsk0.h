#pragma once

// Noise_NNpsk0_25519_ChaChaPoly_SHA256, responder side only: the protocol the
// ESPHome native API encrypts with (https://noiseprotocol.org/noise.html).
//
// Built on PSA Crypto (mbedTLS 4 on ESP-IDF 6, the same library on the host
// test build), so no extra crypto library is linked into the firmware.
// Requires CONFIG_MBEDTLS_CHACHA20_C + CONFIG_MBEDTLS_CHACHAPOLY_C (off by
// default in ESP-IDF) and Curve25519 (on by default).
//
// Verified on the host against the official cacophony test vector
// (test/test_noise_nnpsk0.cpp): handshake hash and every transport message.

#include <cstddef>
#include <cstdint>

namespace noise {

constexpr size_t kKeyLen = 32;
constexpr size_t kTagLen = 16;
constexpr size_t kDhLen = 32;

// One direction of the transport phase. Owns a PSA key slot.
class CipherState {
public:
  CipherState() = default;
  ~CipherState() { clear(); }
  CipherState(const CipherState &) = delete;
  CipherState &operator=(const CipherState &) = delete;

  [[nodiscard]] bool set_key(const uint8_t key[kKeyLen]);
  [[nodiscard]] bool has_key() const { return key_id_ != 0; }
  void clear();

  // out must hold n + kTagLen bytes. `in` and `out` must not overlap.
  [[nodiscard]] bool encrypt(const uint8_t *ad, size_t ad_len,
                             const uint8_t *in, size_t n, uint8_t *out);
  // n includes the tag; writes n - kTagLen bytes. Fails on a bad tag, and a
  // failed decrypt does NOT advance the nonce (Noise spec, section 5.1).
  [[nodiscard]] bool decrypt(const uint8_t *ad, size_t ad_len,
                             const uint8_t *in, size_t n, uint8_t *out);

private:
  uint32_t key_id_ = 0; // psa_key_id_t; 0 == PSA_KEY_ID_NULL
  uint64_t nonce_ = 0;
};

class Responder {
public:
  Responder() = default;
  ~Responder();
  Responder(const Responder &) = delete;
  Responder &operator=(const Responder &) = delete;

  // `ephemeral_private`: nullptr in production (a fresh random key is drawn);
  // a fixed key only for test vectors.
  [[nodiscard]] bool init(const uint8_t psk[kKeyLen], const uint8_t *prologue,
                          size_t prologue_len,
                          const uint8_t *ephemeral_private = nullptr);

  // Message 1 (-> psk, e). `payload_out` gets len - kDhLen - kTagLen bytes.
  // false = malformed or wrong PSK (the AEAD tag does not verify): the
  // caller reports "Handshake MAC failure", which clients read as a key error.
  [[nodiscard]] bool read_message1(const uint8_t *msg, size_t len,
                                   uint8_t *payload_out, size_t *payload_len);

  // Message 2 (<- e, ee). out needs kDhLen + payload_len + kTagLen bytes.
  [[nodiscard]] bool write_message2(const uint8_t *payload, size_t payload_len,
                                    uint8_t *out, size_t cap, size_t *out_len);

  // After message 2: the two transport ciphers. Responder sends with the
  // second key and receives with the first.
  [[nodiscard]] bool split(CipherState &send, CipherState &recv);

  // Handshake hash h, for test vectors (and channel binding if ever needed).
  [[nodiscard]] const uint8_t *handshake_hash() const { return h_; }

private:
  void mix_hash(const uint8_t *data, size_t n);
  void mix_key(const uint8_t *ikm, size_t n);
  void mix_key_and_hash(const uint8_t *ikm, size_t n);
  bool encrypt_and_hash(const uint8_t *in, size_t n, uint8_t *out);
  bool decrypt_and_hash(const uint8_t *in, size_t n, uint8_t *out);
  void wipe();

  uint8_t ck_[32] = {};
  uint8_t h_[32] = {};
  uint8_t psk_[kKeyLen] = {};
  uint8_t re_[kDhLen] = {};   // remote ephemeral public key
  uint32_t e_key_id_ = 0;     // local ephemeral key pair (PSA slot)
  CipherState cs_;            // handshake cipher (k, n)
  int step_ = 0;              // 0 = init, 1 = msg1 read, 2 = msg2 written
};

// HMAC-SHA256 based HKDF exactly as Noise defines it (section 4.3), exposed
// for the unit tests. out1/out2/out3 are 32 bytes each; out3 may be nullptr.
void hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
          uint8_t out1[32], uint8_t out2[32], uint8_t *out3);

// Overwrites `n` bytes in a way the optimiser cannot elide.
void secure_zero(void *p, size_t n);

} // namespace noise
