// Protobuf wire codec. Expected bytes come from the protobuf encoding guide
// (protobuf.dev/programming-guides/encoding): "08 96 01" for field 1 = 150,
// "12 07 74 65 73 74 69 6e 67" for field 2 = "testing", and the IEEE-754 bit
// pattern of 1.0f (0x3f800000) for fixed32.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "net/esphome_pb.h"

namespace {

using esphome_pb::Reader;
using esphome_pb::Wire;
using esphome_pb::Writer;

std::vector<uint8_t> bytes_of(const Writer &w) {
  return std::vector<uint8_t>(w.data(), w.data() + w.size());
}

TEST(EsphomePb, VarintFromEncodingGuide) {
  uint8_t buf[16];
  Writer w(buf, sizeof buf);
  w.u32(1, 150);
  ASSERT_TRUE(w.ok());
  EXPECT_EQ(bytes_of(w), (std::vector<uint8_t>{0x08, 0x96, 0x01}));
}

TEST(EsphomePb, StringFromEncodingGuide) {
  uint8_t buf[16];
  Writer w(buf, sizeof buf);
  w.str(2, "testing");
  ASSERT_TRUE(w.ok());
  EXPECT_EQ(bytes_of(w), (std::vector<uint8_t>{0x12, 0x07, 't', 'e', 's', 't',
                                               'i', 'n', 'g'}));
}

TEST(EsphomePb, DefaultsAreOmitted) {
  uint8_t buf[16];
  Writer w(buf, sizeof buf);
  w.u32(1, 0);
  w.boolean(2, false);
  w.str(3, "");
  w.str(4, nullptr);
  w.f32(5, 0.0f);
  w.fixed32(6, 0);
  EXPECT_TRUE(w.ok());
  EXPECT_EQ(w.size(), 0u);
}

TEST(EsphomePb, NegativeInt32IsTenBytes) {
  uint8_t buf[16];
  Writer w(buf, sizeof buf);
  w.i32(7, -1);
  ASSERT_TRUE(w.ok());
  EXPECT_EQ(bytes_of(w),
            (std::vector<uint8_t>{0x38, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                  0xff, 0xff, 0x01}));
}

TEST(EsphomePb, FloatAndFixed32AreLittleEndian) {
  uint8_t buf[16];
  Writer w(buf, sizeof buf);
  w.f32(2, 1.0f);
  w.fixed32(3, 0x11223344u);
  ASSERT_TRUE(w.ok());
  EXPECT_EQ(bytes_of(w), (std::vector<uint8_t>{0x15, 0x00, 0x00, 0x80, 0x3f,
                                               0x1d, 0x44, 0x33, 0x22, 0x11}));
}

TEST(EsphomePb, OverflowFailsStickily) {
  uint8_t buf[4];
  Writer w(buf, sizeof buf);
  w.str(1, "too long");
  EXPECT_FALSE(w.ok());
  w.u32(2, 1); // would fit, but the writer is already failed
  EXPECT_FALSE(w.ok());
}

TEST(EsphomePb, ReaderWalksAllWireTypes) {
  const uint8_t msg[] = {0x08, 0x96, 0x01,                          // 1: 150
                         0x12, 0x02, 'h',  'i',                     // 2: "hi"
                         0x1d, 0x00, 0x00, 0x80, 0x3f,              // 3: 1.0f
                         0x21, 1,    2,    3,    4, 5, 6, 7, 8,     // 4: fixed64
                         0x28, 0x01};                               // 5: true
  Reader r(msg, sizeof msg);
  ASSERT_TRUE(r.next());
  EXPECT_EQ(r.field(), 1u);
  EXPECT_EQ(r.as_u32(), 150u);
  ASSERT_TRUE(r.next());
  EXPECT_EQ(r.field(), 2u);
  size_t n = 0;
  const uint8_t *p = r.as_bytes(&n);
  ASSERT_EQ(n, 2u);
  EXPECT_EQ(p[0], 'h');
  ASSERT_TRUE(r.next());
  EXPECT_EQ(r.field(), 3u);
  EXPECT_FLOAT_EQ(r.as_f32(), 1.0f);
  ASSERT_TRUE(r.next());
  EXPECT_EQ(r.field(), 4u);
  EXPECT_EQ(r.wire(), Wire::kFixed64);
  ASSERT_TRUE(r.next());
  EXPECT_EQ(r.field(), 5u);
  EXPECT_TRUE(r.as_bool());
  EXPECT_FALSE(r.next());
  EXPECT_TRUE(r.ok());
}

TEST(EsphomePb, ReaderRejectsTruncatedLength) {
  const uint8_t msg[] = {0x12, 0x05, 'a', 'b'};
  Reader r(msg, sizeof msg);
  EXPECT_FALSE(r.next());
  EXPECT_FALSE(r.ok());
}

TEST(EsphomePb, ReaderRejectsFieldZeroAndGroups) {
  const uint8_t zero[] = {0x00, 0x01};
  Reader r0(zero, sizeof zero);
  EXPECT_FALSE(r0.next());
  EXPECT_FALSE(r0.ok());
  const uint8_t group[] = {0x0b}; // field 1, wire type 3 (start group)
  Reader r1(group, sizeof group);
  EXPECT_FALSE(r1.next());
  EXPECT_FALSE(r1.ok());
}

TEST(EsphomePb, DecodeVarintReportsIncompleteAndOverlong) {
  uint64_t v = 0;
  const uint8_t partial[] = {0x96};
  EXPECT_EQ(esphome_pb::decode_varint(partial, sizeof partial, &v), 0);
  uint8_t overlong[11];
  for (auto &b : overlong) {
    b = 0x80;
  }
  EXPECT_EQ(esphome_pb::decode_varint(overlong, sizeof overlong, &v), -1);
  const uint8_t ok[] = {0x96, 0x01};
  EXPECT_EQ(esphome_pb::decode_varint(ok, sizeof ok, &v), 2);
  EXPECT_EQ(v, 150u);
}

} // namespace
