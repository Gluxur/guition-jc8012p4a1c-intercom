#include <gtest/gtest.h>

#include "app/ha_discovery_fmt.h"

#include <cstring>
#include <string>

namespace {

std::string Payload(const ha_fmt::EntityDesc &e, size_t cap = 512) {
  char buf[1024] = {0};
  const int n = ha_fmt::discovery_payload(buf, cap, e);
  EXPECT_GE(n, 0);
  EXPECT_LT((size_t)n, cap);
  return std::string(buf, (size_t)n);
}

bool Has(const std::string &s, const char *needle) {
  return s.find(needle) != std::string::npos;
}

} // namespace

TEST(HaFmt, WrittenBorneLaLongueur) {
  EXPECT_EQ(ha_fmt::written(10, 64), 10);
  EXPECT_EQ(ha_fmt::written(-1, 64), 0);
  // snprintf a voulu écrire 200 dans 64 : on ne doit publier que 63.
  EXPECT_EQ(ha_fmt::written(200, 64), 63);
}

TEST(HaFmt, TopicDeDecouverte) {
  char t[128];
  ha_fmt::EntityDesc e = {"sensor", "rssi", "RSSI", "", nullptr, nullptr};
  ha_fmt::discovery_topic(t, sizeof t, e);
  EXPECT_STREQ(t, "homeassistant/sensor/visiophone/rssi/config");
}

TEST(HaFmt, TopicsParDefaut) {
  char t[128];
  ha_fmt::default_state_topic(t, sizeof t, "dnd");
  EXPECT_STREQ(t, "visio/dnd/state");
  ha_fmt::default_command_topic(t, sizeof t, "dnd");
  EXPECT_STREQ(t, "visio/dnd/set");
}

TEST(HaFmt, CapteurContientLesChampsAttendus) {
  ha_fmt::EntityDesc e = {
      "sensor", "rssi", "RSSI Wi-Fi",
      "\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\"",
      "visio/rssi/state", nullptr};
  const std::string p = Payload(e);
  EXPECT_TRUE(Has(p, "\"uniq_id\":\"visiophone_rssi\""));
  EXPECT_TRUE(Has(p, "\"stat_t\":\"visio/rssi/state\""));
  EXPECT_TRUE(Has(p, "\"avty_t\":\"visio/status\""));
  EXPECT_TRUE(Has(p, "\"dev\":"));
  EXPECT_TRUE(Has(p, "\"dev_cla\":\"signal_strength\""));
  EXPECT_FALSE(Has(p, "cmd_t"));
  EXPECT_EQ(p.front(), '{');
  EXPECT_EQ(p.back(), '}');
}

TEST(HaFmt, SurchargeDeTopicRespectee) {
  // Le topic historique doit survivre : des automatisations Home Assistant en
  // dépendent, et un topic dérivé les casserait silencieusement.
  ha_fmt::EntityDesc e = {"sensor", "fw_version", "Firmware version", "",
                          "visio/fw/version", nullptr};
  const std::string p = Payload(e);
  EXPECT_TRUE(Has(p, "\"stat_t\":\"visio/fw/version\""));
  EXPECT_FALSE(Has(p, "visio/fw_version/state"));
}

TEST(HaFmt, BoutonNaPasDEtat) {
  ha_fmt::EntityDesc e = {"button", "reboot", "Redemarrer",
                          "\"pl_prs\":\"PRESS\"", nullptr, "visio/reboot/set"};
  const std::string p = Payload(e);
  EXPECT_FALSE(Has(p, "stat_t"));
  EXPECT_TRUE(Has(p, "\"cmd_t\":\"visio/reboot/set\""));
}

TEST(HaFmt, LightSchemaJson) {
  ha_fmt::EntityDesc e = {
      "light", "screen", "Ecran",
      "\"schema\":\"json\",\"brightness\":true,\"bri_scl\":100",
      "visio/screen/state", "visio/screen/set"};
  const std::string p = Payload(e);
  EXPECT_TRUE(Has(p, "\"schema\":\"json\""));
  EXPECT_TRUE(Has(p, "\"bri_scl\":100"));
  EXPECT_TRUE(Has(p, "\"cmd_t\":\"visio/screen/set\""));
  EXPECT_TRUE(Has(p, "\"stat_t\":\"visio/screen/state\""));
}

TEST(HaFmt, TroncatureNeDepassePasLeTampon) {
  // Nom volontairement plus long que le tampon : le JSON produit doit rester
  // dans les clous et la longueur renvoyée être < cap.
  static char long_name[600];
  memset(long_name, 'x', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';
  ha_fmt::EntityDesc e = {"sensor", "x", long_name, "", nullptr, nullptr};

  char buf[256];
  memset(buf, 0x7F, sizeof buf);
  const int n = ha_fmt::discovery_payload(buf, 200, e);
  EXPECT_GE(n, 0);
  EXPECT_LT(n, 200);
  EXPECT_EQ(buf[n], '\0');
  // Rien n'a été écrit au-delà de la capacité annoncée.
  EXPECT_EQ((unsigned char)buf[220], 0x7F);
}
