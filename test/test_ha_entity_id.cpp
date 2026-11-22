#include <gtest/gtest.h>

#include "app/ha_entity_id.h"

#include <string>

namespace {

bool Norm(const char *in, const char *domain, std::string *out) {
  char buf[ha_entity_id::kMax] = "keep";
  const bool ok = ha_entity_id::normalize(in, domain, buf, sizeof buf);
  *out = buf;
  return ok;
}

} // namespace

TEST(HaEntityId, AcceptsAndTrims) {
  std::string out;
  EXPECT_TRUE(Norm(" cover.portail ", "cover", &out));
  EXPECT_EQ(out, "cover.portail");
}

TEST(HaEntityId, EmptyMeansUnset) {
  std::string out;
  EXPECT_TRUE(Norm("", "cover", &out));
  EXPECT_EQ(out, "");
  EXPECT_TRUE(Norm("   ", "cover", &out));
  EXPECT_EQ(out, "");
}

TEST(HaEntityId, RejectsWrongDomain) {
  std::string out;
  EXPECT_FALSE(Norm("light.x", "cover", &out));
  EXPECT_FALSE(Norm("coverx.y", "cover", &out));
}

TEST(HaEntityId, RejectsUppercaseAndBadChars) {
  std::string out;
  EXPECT_FALSE(Norm("Cover.Portail", "cover", &out));
  EXPECT_FALSE(Norm("cover.port ail", "cover", &out));
  EXPECT_FALSE(Norm("cover.port-ail", "cover", &out));
}

TEST(HaEntityId, RejectsNoObject) {
  std::string out;
  EXPECT_FALSE(Norm("cover.", "cover", &out));
  EXPECT_FALSE(Norm("cover", "cover", &out));
}

TEST(HaEntityId, RejectsTooLong) {
  std::string out;
  const std::string at128 = "cover." + std::string(122, 'a');
  const std::string at127 = "cover." + std::string(121, 'a');
  ASSERT_EQ(at128.size(), 128u);
  EXPECT_FALSE(Norm(at128.c_str(), "cover", &out));
  EXPECT_TRUE(Norm(at127.c_str(), "cover", &out));
  EXPECT_EQ(out, at127);
}

TEST(HaEntityId, KeepsOutOnFailure) {
  std::string out;
  EXPECT_FALSE(Norm("light.x", "cover", &out));
  EXPECT_EQ(out, "keep");
}
