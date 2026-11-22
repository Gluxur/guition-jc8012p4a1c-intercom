#include <gtest/gtest.h>

#include "net/github_release.h"

#include <cstring>
#include <string>

namespace {

constexpr const char *kRepo = "Gluxur/guition-jc8012p4a1c-intercom";
constexpr const char *kAsset = "esp32_p4_webrtc_cam.bin";

// Hand-written, shaped like GET /repos/{owner}/{repo}/releases/latest
// (fields the parser ignores kept so it is exercised against real noise).
std::string Fixture(const std::string &tag, const std::string &body,
                    const std::string &asset_url) {
  return std::string(R"({
  "url": "https://api.github.com/repos/Gluxur/guition-jc8012p4a1c-intercom/releases/1",
  "html_url": "https://github.com/Gluxur/guition-jc8012p4a1c-intercom/releases/tag/)") +
         tag + R"(",
  "id": 1, "draft": false, "prerelease": false,
  "tag_name": ")" + tag + R"(",
  "name": "release",
  "assets": [
    {"name": "bootloader.bin", "size": 21360,
     "browser_download_url": "https://github.com/Gluxur/guition-jc8012p4a1c-intercom/releases/download/)" +
         tag + R"(/bootloader.bin"},
    {"name": "esp32_p4_webrtc_cam.bin", "size": 3157296,
     "browser_download_url": ")" + asset_url + R"("}
  ],
  "body": ")" + body + R"("
})";
}

std::string Url(const std::string &tag) {
  return "https://github.com/Gluxur/guition-jc8012p4a1c-intercom/releases/download/" +
         tag + "/esp32_p4_webrtc_cam.bin";
}

} // namespace

TEST(GithubRelease, PicksAssetByName) {
  github_release::Release r = {};
  ASSERT_TRUE(github_release::parse_latest(
      Fixture("v2.1.0", "notes", Url("v2.1.0")).c_str(), kRepo, kAsset, &r));
  EXPECT_STREQ(r.asset_url, Url("v2.1.0").c_str());
}

TEST(GithubRelease, ReadsTagAndHtmlUrl) {
  github_release::Release r = {};
  ASSERT_TRUE(github_release::parse_latest(
      Fixture("v2.1.0", "notes", Url("v2.1.0")).c_str(), kRepo, kAsset, &r));
  EXPECT_STREQ(r.tag, "v2.1.0");
  EXPECT_STREQ(r.html_url,
               "https://github.com/Gluxur/guition-jc8012p4a1c-intercom/releases/tag/v2.1.0");
  EXPECT_STREQ(r.notes, "notes");
}

TEST(GithubRelease, RejectsForeignAssetUrl) {
  github_release::Release r = {};
  EXPECT_FALSE(github_release::parse_latest(
      Fixture("v2.1.0", "n",
              "https://github.com/evil/x/releases/download/v2.1.0/esp32_p4_webrtc_cam.bin")
          .c_str(),
      kRepo, kAsset, &r));
  EXPECT_FALSE(github_release::parse_latest(
      Fixture("v2.1.0", "n", "http://github.com/Gluxur/guition-jc8012p4a1c-intercom/releases/download/v2.1.0/esp32_p4_webrtc_cam.bin")
          .c_str(),
      kRepo, kAsset, &r));
}

TEST(GithubRelease, RejectsMissingAsset) {
  github_release::Release r = {};
  EXPECT_FALSE(github_release::parse_latest(
      Fixture("v2.1.0", "n", Url("v2.1.0")).c_str(), kRepo, "other.bin", &r));
}

TEST(GithubRelease, RejectsMissingTag) {
  github_release::Release r = {};
  EXPECT_FALSE(github_release::parse_latest(
      Fixture("", "n", Url("x")).c_str(), kRepo, kAsset, &r));
}

TEST(GithubRelease, RejectsNonReleaseJson) {
  github_release::Release r = {};
  EXPECT_FALSE(github_release::parse_latest(R"({"message":"Not Found"})", kRepo,
                                            kAsset, &r));
  EXPECT_FALSE(github_release::parse_latest("not json", kRepo, kAsset, &r));
  EXPECT_FALSE(github_release::parse_latest(nullptr, kRepo, kAsset, &r));
}

TEST(GithubRelease, StripsCarriageReturns) {
  github_release::Release r = {};
  ASSERT_TRUE(github_release::parse_latest(
      Fixture("v2.1.0", "a\\r\\nb", Url("v2.1.0")).c_str(), kRepo, kAsset, &r));
  EXPECT_STREQ(r.notes, "a\nb");
}

TEST(GithubRelease, NotesCutOnUtf8Boundary) {
  github_release::Release r = {};
  // 254 ASCII bytes then U+00E9 (2 bytes, C3 A9): byte 255 would be the
  // first half of the character, so the cut must happen before it.
  const std::string body = std::string(254, 'a') + "\xC3\xA9" + "tail";
  ASSERT_TRUE(github_release::parse_latest(
      Fixture("v2.1.0", body, Url("v2.1.0")).c_str(), kRepo, kAsset, &r));
  EXPECT_EQ(strlen(r.notes), 254u);
  EXPECT_EQ(std::string(r.notes), std::string(254, 'a'));
}
