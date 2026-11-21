// Noise_NNpsk0_25519_ChaChaPoly_SHA256 responder against the cacophony test
// vector (github.com/haskell-cryptography/cacophony, vectors/cacophony.txt).
// The vector was produced by an independent Haskell implementation: nothing
// here comes from running net/noise_nnpsk0.cpp.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "net/noise_nnpsk0.h"
#include "psa/crypto.h"

namespace {

std::vector<uint8_t> hex(const char *s) {
  std::vector<uint8_t> out;
  for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
    out.push_back((uint8_t)std::stoi(std::string(s + i, 2), nullptr, 16));
  }
  return out;
}

const char *kPrologue = "4a6f686e2047616c74";
const char *kPsk =
    "54686973206973206d7920417573747269616e20706572737065637469766521";
const char *kRespEphemeral =
    "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b";
const char *kHandshakeHash =
    "f4d03dc34495c95729ea6de9e1b59004b59733102488b3e24bc441e0be208eaf";

struct Msg {
  const char *payload;
  const char *ciphertext;
};
// Even index: initiator -> responder; odd index: responder -> initiator.
const Msg kMsgs[] = {
    {"4c756477696720766f6e204d69736573",
     "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944"
     "79b962b8aff8485742ac32f905ba45369e2465fb59e138a93d67a0d1266b6a54"},
    {"4d757272617920526f746862617264",
     "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843"
     "d6062704d5a9c422a8e834423f8c1feada7e8d0d910a1a2cd030fb584221e3"},
    {"462e20412e20486179656b",
     "e632c3763d7669067383433197a3baddf146e9e70ad4b4e9e59e0f"},
    {"4361726c204d656e676572",
     "64c6bee32ea91c8474bb4c21d7a700109ad45af77b29764ba5eb1e"},
    {"4a65616e2d426170746973746520536179",
     "e2fa0bed0603b62d3ccac2ecabbf3fe33f3e86514909b323361626266cb2471cc8"},
    {"457567656e2042f6686d20766f6e2042617765726b",
     "0c01dc9cec1fe4ddd692e8dd32188aa351088dc91183639a53b57aa4692b5ebdef8b8c"
     "a111"},
};

class NoiseTest : public ::testing::Test {
protected:
  void SetUp() override { ASSERT_EQ(psa_crypto_init(), PSA_SUCCESS); }
};

TEST_F(NoiseTest, CacophonyVectorResponder) {
  const auto prologue = hex(kPrologue);
  const auto psk = hex(kPsk);
  const auto eph = hex(kRespEphemeral);

  noise::Responder r;
  ASSERT_TRUE(r.init(psk.data(), prologue.data(), prologue.size(), eph.data()));

  // Message 1, written by the initiator.
  const auto m1 = hex(kMsgs[0].ciphertext);
  uint8_t pl[128];
  size_t pl_len = 0;
  ASSERT_TRUE(r.read_message1(m1.data(), m1.size(), pl, &pl_len));
  EXPECT_EQ(std::vector<uint8_t>(pl, pl + pl_len), hex(kMsgs[0].payload));

  // Message 2, ours: must match the vector byte for byte.
  const auto p2 = hex(kMsgs[1].payload);
  uint8_t m2[128];
  size_t m2_len = 0;
  ASSERT_TRUE(r.write_message2(p2.data(), p2.size(), m2, sizeof m2, &m2_len));
  EXPECT_EQ(std::vector<uint8_t>(m2, m2 + m2_len), hex(kMsgs[1].ciphertext));

  EXPECT_EQ(std::vector<uint8_t>(r.handshake_hash(), r.handshake_hash() + 32),
            hex(kHandshakeHash));

  noise::CipherState send, recv;
  ASSERT_TRUE(r.split(send, recv));

  for (size_t i = 2; i < sizeof(kMsgs) / sizeof(kMsgs[0]); i++) {
    const auto p = hex(kMsgs[i].payload);
    const auto c = hex(kMsgs[i].ciphertext);
    uint8_t buf[128];
    if (i % 2 == 0) {
      ASSERT_TRUE(recv.decrypt(nullptr, 0, c.data(), c.size(), buf)) << i;
      EXPECT_EQ(std::vector<uint8_t>(buf, buf + p.size()), p) << i;
    } else {
      ASSERT_TRUE(send.encrypt(nullptr, 0, p.data(), p.size(), buf)) << i;
      EXPECT_EQ(std::vector<uint8_t>(buf, buf + c.size()), c) << i;
    }
  }
}

TEST_F(NoiseTest, WrongPskFailsMessage1) {
  const auto prologue = hex(kPrologue);
  auto psk = hex(kPsk);
  psk[0] ^= 1;
  const auto eph = hex(kRespEphemeral);
  noise::Responder r;
  ASSERT_TRUE(r.init(psk.data(), prologue.data(), prologue.size(), eph.data()));
  const auto m1 = hex(kMsgs[0].ciphertext);
  uint8_t pl[128];
  size_t pl_len = 0;
  EXPECT_FALSE(r.read_message1(m1.data(), m1.size(), pl, &pl_len));
}

TEST_F(NoiseTest, TamperedTransportFrameIsRejectedWithoutBurningNonce) {
  const auto prologue = hex(kPrologue);
  const auto psk = hex(kPsk);
  const auto eph = hex(kRespEphemeral);
  noise::Responder r;
  ASSERT_TRUE(r.init(psk.data(), prologue.data(), prologue.size(), eph.data()));
  const auto m1 = hex(kMsgs[0].ciphertext);
  uint8_t buf[128];
  size_t n = 0;
  ASSERT_TRUE(r.read_message1(m1.data(), m1.size(), buf, &n));
  const auto p2 = hex(kMsgs[1].payload);
  ASSERT_TRUE(r.write_message2(p2.data(), p2.size(), buf, sizeof buf, &n));
  noise::CipherState send, recv;
  ASSERT_TRUE(r.split(send, recv));

  auto c = hex(kMsgs[2].ciphertext);
  c[3] ^= 0x40;
  EXPECT_FALSE(recv.decrypt(nullptr, 0, c.data(), c.size(), buf));
  // The genuine frame still decrypts: the failed attempt kept nonce 0.
  c[3] ^= 0x40;
  EXPECT_TRUE(recv.decrypt(nullptr, 0, c.data(), c.size(), buf));
}

// Noise's HKDF (section 4.3) on an all-zero chaining key and empty input.
// Reference values from Python's hmac/hashlib, following the spec's formula:
// t = HMAC(ck, ikm); o1 = HMAC(t, 01); o2 = HMAC(t, o1||02); o3 = HMAC(t, o2||03).
TEST_F(NoiseTest, HkdfZeroInputs) {
  uint8_t ck[32] = {};
  uint8_t o1[32], o2[32], o3[32];
  noise::hkdf(ck, nullptr, 0, o1, o2, o3);
  EXPECT_EQ(std::vector<uint8_t>(o1, o1 + 32),
            hex("eb70f01dede9afafa449eee1b1286504e1f62388b3f7dd4f956697b0e828fe18"));
  EXPECT_EQ(std::vector<uint8_t>(o2, o2 + 32),
            hex("1e59c2ec0fe6e7e7ac2613b6ab65342a83379969da234240cded3777914db907"));
  EXPECT_EQ(std::vector<uint8_t>(o3, o3 + 32),
            hex("5568c74fdb8fc92331d5c59e1e2dd77a8c2c63aba7cf2d3457f8ee8620462f8a"));
}

} // namespace
