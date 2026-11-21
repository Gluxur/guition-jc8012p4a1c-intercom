#include "noise_nnpsk0.h"

#include <cstring>

#include "psa/crypto.h"

namespace noise {
namespace {

constexpr char kProtocolName[] = "Noise_NNpsk0_25519_ChaChaPoly_SHA256";
constexpr size_t kHashLen = 32;
constexpr size_t kBlockLen = 64;

bool sha256(const uint8_t *a, size_t an, const uint8_t *b, size_t bn,
            uint8_t out[kHashLen]) {
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  size_t olen = 0;
  bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
            psa_hash_update(&op, a, an) == PSA_SUCCESS &&
            (bn == 0 || psa_hash_update(&op, b, bn) == PSA_SUCCESS) &&
            psa_hash_finish(&op, out, kHashLen, &olen) == PSA_SUCCESS &&
            olen == kHashLen;
  if (!ok) {
    psa_hash_abort(&op);
  }
  return ok;
}

// HMAC-SHA256 over the concatenation a || b, key of 32 bytes (always the case
// in Noise: the key is ck or a HKDF temp key). Done on top of the hash rather
// than psa_mac_*, which would need a PSA key slot per call.
void hmac(const uint8_t key[kHashLen], const uint8_t *a, size_t an,
          const uint8_t *b, size_t bn, uint8_t out[kHashLen]) {
  uint8_t pad[kBlockLen];
  uint8_t inner[kHashLen];

  memset(pad, 0x36, sizeof pad);
  for (size_t i = 0; i < kHashLen; i++) {
    pad[i] ^= key[i];
  }
  psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
  size_t olen = 0;
  bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
            psa_hash_update(&op, pad, sizeof pad) == PSA_SUCCESS &&
            (an == 0 || psa_hash_update(&op, a, an) == PSA_SUCCESS) &&
            (bn == 0 || psa_hash_update(&op, b, bn) == PSA_SUCCESS) &&
            psa_hash_finish(&op, inner, sizeof inner, &olen) == PSA_SUCCESS;
  if (!ok) {
    psa_hash_abort(&op);
    memset(out, 0, kHashLen); // a zero key fails every later AEAD check
    secure_zero(pad, sizeof pad);
    return;
  }

  memset(pad, 0x5c, sizeof pad);
  for (size_t i = 0; i < kHashLen; i++) {
    pad[i] ^= key[i];
  }
  if (!sha256(pad, sizeof pad, inner, sizeof inner, out)) {
    memset(out, 0, kHashLen);
  }
  secure_zero(pad, sizeof pad);
  secure_zero(inner, sizeof inner);
}

void make_nonce(uint64_t n, uint8_t out[12]) {
  // ChaChaPoly nonce in Noise: 32 zero bits, then the counter little-endian.
  memset(out, 0, 4);
  for (int i = 0; i < 8; i++) {
    out[4 + i] = (uint8_t)(n >> (8 * i));
  }
}

bool import_x25519_private(const uint8_t priv[kDhLen], uint32_t *id) {
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&a, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
  psa_set_key_bits(&a, 255);
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_DERIVE);
  psa_set_key_algorithm(&a, PSA_ALG_ECDH);
  psa_key_id_t k = PSA_KEY_ID_NULL;
  // PSA clamps the scalar on import (RFC 7748 decodeScalar25519), so any 32
  // random bytes are a valid private key.
  const bool ok = psa_import_key(&a, priv, kDhLen, &k) == PSA_SUCCESS;
  *id = ok ? (uint32_t)k : 0;
  return ok;
}

} // namespace

void secure_zero(void *p, size_t n) {
  volatile uint8_t *v = static_cast<volatile uint8_t *>(p);
  while (n-- != 0) {
    *v++ = 0;
  }
}

void hkdf(const uint8_t ck[32], const uint8_t *ikm, size_t ikm_len,
          uint8_t out1[32], uint8_t out2[32], uint8_t *out3) {
  uint8_t temp[kHashLen];
  const uint8_t one = 0x01, two = 0x02, three = 0x03;
  hmac(ck, ikm, ikm_len, nullptr, 0, temp);
  hmac(temp, &one, 1, nullptr, 0, out1);
  hmac(temp, out1, kHashLen, &two, 1, out2);
  if (out3 != nullptr) {
    hmac(temp, out2, kHashLen, &three, 1, out3);
  }
  secure_zero(temp, sizeof temp);
}

// --- CipherState -----------------------------------------------------------

bool CipherState::set_key(const uint8_t key[kKeyLen]) {
  clear();
  psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_type(&a, PSA_KEY_TYPE_CHACHA20);
  psa_set_key_bits(&a, 256);
  psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
  psa_set_key_algorithm(&a, PSA_ALG_CHACHA20_POLY1305);
  psa_key_id_t k = PSA_KEY_ID_NULL;
  if (psa_import_key(&a, key, kKeyLen, &k) != PSA_SUCCESS) {
    return false;
  }
  key_id_ = (uint32_t)k;
  nonce_ = 0;
  return true;
}

void CipherState::clear() {
  if (key_id_ != 0) {
    psa_destroy_key((psa_key_id_t)key_id_);
    key_id_ = 0;
  }
  nonce_ = 0;
}

bool CipherState::encrypt(const uint8_t *ad, size_t ad_len, const uint8_t *in,
                          size_t n, uint8_t *out) {
  if (key_id_ == 0) {
    return false;
  }
  // ESPHome's handshake payloads are empty: hand PSA a valid pointer anyway.
  static const uint8_t kEmpty = 0;
  if (in == nullptr) {
    in = &kEmpty;
  }
  uint8_t nonce[12];
  make_nonce(nonce_, nonce);
  size_t olen = 0;
  const psa_status_t s = psa_aead_encrypt(
      (psa_key_id_t)key_id_, PSA_ALG_CHACHA20_POLY1305, nonce, sizeof nonce,
      ad, ad_len, in, n, out, n + kTagLen, &olen);
  if (s != PSA_SUCCESS || olen != n + kTagLen) {
    return false;
  }
  nonce_++;
  return true;
}

bool CipherState::decrypt(const uint8_t *ad, size_t ad_len, const uint8_t *in,
                          size_t n, uint8_t *out) {
  if (key_id_ == 0 || n < kTagLen) {
    return false;
  }
  uint8_t nonce[12];
  make_nonce(nonce_, nonce);
  size_t olen = 0;
  const psa_status_t s = psa_aead_decrypt(
      (psa_key_id_t)key_id_, PSA_ALG_CHACHA20_POLY1305, nonce, sizeof nonce,
      ad, ad_len, in, n, out, n - kTagLen, &olen);
  if (s != PSA_SUCCESS || olen != n - kTagLen) {
    return false;
  }
  nonce_++;
  return true;
}

// --- Responder -------------------------------------------------------------

Responder::~Responder() { wipe(); }

void Responder::wipe() {
  if (e_key_id_ != 0) {
    psa_destroy_key((psa_key_id_t)e_key_id_);
    e_key_id_ = 0;
  }
  cs_.clear();
  secure_zero(ck_, sizeof ck_);
  secure_zero(psk_, sizeof psk_);
  secure_zero(re_, sizeof re_);
  step_ = 0;
}

void Responder::mix_hash(const uint8_t *data, size_t n) {
  uint8_t out[kHashLen];
  if (!sha256(h_, sizeof h_, data, n, out)) {
    memset(out, 0, sizeof out);
  }
  memcpy(h_, out, sizeof h_);
}

void Responder::mix_key(const uint8_t *ikm, size_t n) {
  uint8_t k[kHashLen];
  hkdf(ck_, ikm, n, ck_, k, nullptr);
  if (!cs_.set_key(k)) {
    cs_.clear();
  }
  secure_zero(k, sizeof k);
}

void Responder::mix_key_and_hash(const uint8_t *ikm, size_t n) {
  uint8_t th[kHashLen];
  uint8_t k[kHashLen];
  hkdf(ck_, ikm, n, ck_, th, k);
  mix_hash(th, sizeof th);
  if (!cs_.set_key(k)) {
    cs_.clear();
  }
  secure_zero(k, sizeof k);
}

bool Responder::encrypt_and_hash(const uint8_t *in, size_t n, uint8_t *out) {
  // k is always set here: psk0 keys the cipher before any payload.
  if (!cs_.encrypt(h_, sizeof h_, in, n, out)) {
    return false;
  }
  mix_hash(out, n + kTagLen);
  return true;
}

bool Responder::decrypt_and_hash(const uint8_t *in, size_t n, uint8_t *out) {
  if (!cs_.decrypt(h_, sizeof h_, in, n, out)) {
    return false;
  }
  mix_hash(in, n);
  return true;
}

bool Responder::init(const uint8_t psk[kKeyLen], const uint8_t *prologue,
                     size_t prologue_len, const uint8_t *ephemeral_private) {
  wipe();
  static_assert(sizeof(kProtocolName) - 1 > kHashLen,
                "a name longer than HASHLEN is hashed, not padded");
  if (!sha256(reinterpret_cast<const uint8_t *>(kProtocolName),
              sizeof(kProtocolName) - 1, nullptr, 0, h_)) {
    return false;
  }
  memcpy(ck_, h_, sizeof ck_);
  mix_hash(prologue, prologue_len);
  memcpy(psk_, psk, kKeyLen);

  uint8_t priv[kDhLen];
  if (ephemeral_private != nullptr) {
    memcpy(priv, ephemeral_private, kDhLen);
  } else if (psa_generate_random(priv, sizeof priv) != PSA_SUCCESS) {
    return false;
  }
  const bool ok = import_x25519_private(priv, &e_key_id_);
  secure_zero(priv, sizeof priv);
  return ok;
}

bool Responder::read_message1(const uint8_t *msg, size_t len,
                              uint8_t *payload_out, size_t *payload_len) {
  if (step_ != 0 || e_key_id_ == 0 || len < kDhLen + kTagLen) {
    return false;
  }
  mix_key_and_hash(psk_, kKeyLen); // psk
  memcpy(re_, msg, kDhLen);        // e
  mix_hash(re_, kDhLen);
  mix_key(re_, kDhLen); // psk modifier: every e token also feeds MixKey
  const size_t clen = len - kDhLen;
  if (!decrypt_and_hash(msg + kDhLen, clen, payload_out)) {
    return false;
  }
  *payload_len = clen - kTagLen;
  step_ = 1;
  return true;
}

bool Responder::write_message2(const uint8_t *payload, size_t payload_len,
                               uint8_t *out, size_t cap, size_t *out_len) {
  if (step_ != 1 || cap < kDhLen + payload_len + kTagLen) {
    return false;
  }
  size_t plen = 0;
  if (psa_export_public_key((psa_key_id_t)e_key_id_, out, kDhLen, &plen) !=
          PSA_SUCCESS ||
      plen != kDhLen) {
    return false;
  }
  mix_hash(out, kDhLen); // e
  mix_key(out, kDhLen);

  uint8_t dh[kDhLen]; // ee
  size_t dlen = 0;
  const psa_status_t s =
      psa_raw_key_agreement(PSA_ALG_ECDH, (psa_key_id_t)e_key_id_, re_, kDhLen,
                            dh, sizeof dh, &dlen);
  if (s != PSA_SUCCESS || dlen != kDhLen) {
    secure_zero(dh, sizeof dh);
    return false;
  }
  mix_key(dh, sizeof dh);
  secure_zero(dh, sizeof dh);

  if (!encrypt_and_hash(payload, payload_len, out + kDhLen)) {
    return false;
  }
  *out_len = kDhLen + payload_len + kTagLen;
  step_ = 2;
  return true;
}

bool Responder::split(CipherState &send, CipherState &recv) {
  if (step_ != 2) {
    return false;
  }
  uint8_t k1[kHashLen];
  uint8_t k2[kHashLen];
  hkdf(ck_, nullptr, 0, k1, k2, nullptr);
  const bool ok = recv.set_key(k1) && send.set_key(k2);
  secure_zero(k1, sizeof k1);
  secure_zero(k2, sizeof k2);
  // The handshake state is no longer needed; h_ is kept for handshake_hash().
  if (e_key_id_ != 0) {
    psa_destroy_key((psa_key_id_t)e_key_id_);
    e_key_id_ = 0;
  }
  cs_.clear();
  secure_zero(ck_, sizeof ck_);
  secure_zero(psk_, sizeof psk_);
  return ok;
}

} // namespace noise
