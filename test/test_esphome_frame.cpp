// ESPHome API framing. Layouts from ESPHome's api_frame_helper_plaintext.cpp
// and api_frame_helper_noise.cpp (indicator byte, varint/be16 lengths).

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>

#include "net/esphome_frame.h"

namespace {

using esphome_frame::Parse;

TEST(EsphomeFrame, PlainHeaderRoundTrip) {
  uint8_t h[16];
  // len 300 = varint ac 02, type 49 = 0x31
  const size_t n = esphome_frame::write_plain_header(h, sizeof h, 300, 49);
  ASSERT_EQ(n, 4u);
  EXPECT_EQ(h[0], 0x00);
  EXPECT_EQ(h[1], 0xac);
  EXPECT_EQ(h[2], 0x02);
  EXPECT_EQ(h[3], 0x31);

  size_t hl = 0;
  uint32_t len = 0, type = 0;
  ASSERT_EQ(esphome_frame::parse_plain_header(h, n, &hl, &len, &type), Parse::kOk);
  EXPECT_EQ(hl, 4u);
  EXPECT_EQ(len, 300u);
  EXPECT_EQ(type, 49u);
}

TEST(EsphomeFrame, PlainHeaderNeedsMoreUntilComplete) {
  const uint8_t h[] = {0x00, 0xac, 0x02, 0x31};
  size_t hl;
  uint32_t len, type;
  for (size_t n = 0; n < sizeof h; n++) {
    EXPECT_EQ(esphome_frame::parse_plain_header(h, n, &hl, &len, &type),
              Parse::kNeedMore)
        << n;
  }
}

TEST(EsphomeFrame, PlainHeaderRejectsNoiseIndicator) {
  const uint8_t h[] = {0x01, 0x00, 0x00};
  size_t hl;
  uint32_t len, type;
  EXPECT_EQ(esphome_frame::parse_plain_header(h, sizeof h, &hl, &len, &type),
            Parse::kBad);
}

TEST(EsphomeFrame, NoiseHeader) {
  uint8_t h[3];
  esphome_frame::write_noise_header(h, 0x0130);
  EXPECT_EQ(h[0], 0x01);
  EXPECT_EQ(h[1], 0x01);
  EXPECT_EQ(h[2], 0x30);
  uint16_t len = 0;
  EXPECT_EQ(esphome_frame::parse_noise_header(h, 2, &len), Parse::kNeedMore);
  ASSERT_EQ(esphome_frame::parse_noise_header(h, 3, &len), Parse::kOk);
  EXPECT_EQ(len, 0x0130);
  const uint8_t bad[] = {0x00, 0, 0};
  EXPECT_EQ(esphome_frame::parse_noise_header(bad, 3, &len), Parse::kBad);
}

TEST(EsphomeFrame, ServerHelloLayout) {
  uint8_t out[64];
  const uint8_t mac[6] = {0xAC, 0xBC, 0x32, 0x89, 0x0E, 0xA9};
  const size_t n = esphome_frame::write_server_hello(out, sizeof out, "visio", mac);
  const char expected[] = "\x01visio\0acbc32890ea9"; // + implicit final NUL
  ASSERT_EQ(n, sizeof expected);
  EXPECT_EQ(memcmp(out, expected, n), 0);
  EXPECT_EQ(esphome_frame::write_server_hello(out, 5, "visio", mac), 0u);
}

// RFC 4648 section 10 test vectors.
TEST(EsphomeFrame, Base64Rfc4648) {
  struct V {
    const char *enc;
    const char *dec;
  } const vs[] = {{"", ""},         {"Zg==", "f"},     {"Zm8=", "fo"},
                  {"Zm9v", "foo"},  {"Zm9vYg==", "foob"}, {"Zm9vYmE=", "fooba"},
                  {"Zm9vYmFy", "foobar"}};
  for (const V &v : vs) {
    uint8_t out[16];
    const int n = esphome_frame::decode_base64(v.enc, strlen(v.enc), out, sizeof out);
    ASSERT_EQ(n, (int)strlen(v.dec)) << v.enc;
    EXPECT_EQ(memcmp(out, v.dec, n), 0) << v.enc;
  }
}

TEST(EsphomeFrame, Base64RejectsGarbage) {
  uint8_t out[16];
  EXPECT_EQ(esphome_frame::decode_base64("Zm9", 3, out, sizeof out), -1);
  EXPECT_EQ(esphome_frame::decode_base64("Zm=v", 4, out, sizeof out), -1);
  EXPECT_EQ(esphome_frame::decode_base64("Zm9*", 4, out, sizeof out), -1);
  EXPECT_EQ(esphome_frame::decode_base64("Zm9vYmFy", 8, out, 5), -1);
}

TEST(EsphomeFrame, Base64DecodesA32ByteKey) {
  // 32 bytes 0x00..0x1f, encoded with Python's base64.b64encode.
  const char *enc = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=";
  uint8_t out[32];
  ASSERT_EQ(esphome_frame::decode_base64(enc, strlen(enc), out, sizeof out), 32);
  for (int i = 0; i < 32; i++) {
    EXPECT_EQ(out[i], i);
  }
}

} // namespace
