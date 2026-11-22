#include <gtest/gtest.h>

#include "app/ha_entity_id.h"

#include <regex>
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

// HA validates text.set_value against the entity's `pattern` (Python re.match)
// and refuses with an error; the firmware's normalize() is the second guard.
// Both must agree, or HA accepts what the board then silently drops.
TEST(HaEntityId, PatternAgreesWithNormalize) {
  struct Case {
    const char *domain;
    const char *pattern;
  };
  const Case cases[] = {
      {"cover", ha_entity_id::kCoverPattern},
      {"weather", ha_entity_id::kWeatherPattern},
      {"calendar", ha_entity_id::kCalendarPattern},
  };
  const char *inputs[] = {
      "",          "   ",          " cover.portail ", "cover.portail",
      "Cover.Portail", "light.x",  "cover.",          "cover",
      "cover.port-ail", "cover.a b", "weather.forecast_home",
      "calendar.home", "coverx.y", "cover.x_1",
  };
  for (const Case &c : cases) {
    const std::regex re(c.pattern);
    for (const char *in : inputs) {
      char buf[ha_entity_id::kMax];
      const bool norm = ha_entity_id::normalize(in, c.domain, buf, sizeof buf);
      EXPECT_EQ(std::regex_match(in, re), norm)
          << "domain " << c.domain << " input '" << in << "'";
    }
  }
}
