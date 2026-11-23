#include <gtest/gtest.h>

#include "sys/i18n.h"

using i18n::Str;

namespace {

// La langue est un état global du module : chaque test qui la change la
// remet au français, sans quoi l'ordre d'exécution des suites changerait le
// résultat des tests de gate_logic / weather_parse qui attendent le défaut.
struct LangGuard {
  explicit LangGuard(i18n::Lang l) { i18n::init(l); }
  ~LangGuard() { i18n::init(i18n::Lang::kFr); }
};

} // namespace

TEST(I18n, DefaultsToFrench) {
  EXPECT_EQ(i18n::lang(), i18n::Lang::kFr);
  EXPECT_STREQ(i18n::t(Str::kBack), "Retour");
}

TEST(I18n, EnglishPicksTheSecondString) {
  LangGuard g(i18n::Lang::kEn);
  EXPECT_EQ(i18n::lang(), i18n::Lang::kEn);
  EXPECT_STREQ(i18n::t(Str::kBack), "Back");
}

TEST(I18n, OutOfRangeLanguageFallsBackToFrench) {
  LangGuard g(static_cast<i18n::Lang>(7));
  EXPECT_EQ(i18n::lang(), i18n::Lang::kFr);
  EXPECT_STREQ(i18n::t(Str::kBack), "Retour");
}

// Les vecteurs sont écrits à la main ici, pas relus depuis les tables : un
// test qui recopierait fr.h validerait n'importe quelle coquille.
TEST(I18n, KeysPickTheirOwnText) {
  EXPECT_STREQ(i18n::t(Str::kSettingsSave), "Enregistrer");
  EXPECT_STREQ(i18n::t(Str::kWeatherMissedManyFmt), "%lu appels manqués");
  LangGuard g(i18n::Lang::kEn);
  EXPECT_STREQ(i18n::t(Str::kSettingsSave), "Save");
  EXPECT_STREQ(i18n::t(Str::kWeatherMissedManyFmt), "%lu missed calls");
  EXPECT_STREQ(i18n::t(Str::kHaEntScreen), "Screen");
}

TEST(I18n, OutOfRangeKeyIsEmptyNotNull) {
  EXPECT_STREQ(i18n::t(Str::kCount), "");
  EXPECT_STREQ(i18n::t(static_cast<Str>(0xFFFF)), "");
}

// Doublon du static_assert de i18n.cpp, mais qui NOMME la clé fautive.
TEST(I18n, EveryKeyHasATextInEveryLanguage) {
  for (auto l : {i18n::Lang::kFr, i18n::Lang::kEn}) {
    LangGuard g(l);
    for (uint16_t i = 0; i < static_cast<uint16_t>(Str::kCount); ++i) {
      const char *txt = i18n::t(static_cast<Str>(i));
      ASSERT_NE(txt, nullptr) << "clé " << i;
      EXPECT_NE(txt[0], '\0') << "langue " << static_cast<int>(l) << ", clé " << i;
    }
  }
}

TEST(I18n, DateOrderFollowsTheLanguage) {
  EXPECT_FALSE(i18n::month_before_day());
  LangGuard g(i18n::Lang::kEn);
  EXPECT_TRUE(i18n::month_before_day());
}

// struct tm : 0 = dimanche, 6 = samedi ; 0 = janvier, 11 = décembre.
TEST(I18n, CalendarNamesInFrench) {
  EXPECT_STREQ(i18n::weekday_long(0), "Dimanche");
  EXPECT_STREQ(i18n::weekday_long(1), "Lundi");
  EXPECT_STREQ(i18n::weekday_long(6), "Samedi");
  EXPECT_STREQ(i18n::weekday_short(3), "mer.");
  EXPECT_STREQ(i18n::weekday_abbr(4), "Jeu");
  EXPECT_STREQ(i18n::month_long(0), "janvier");
  EXPECT_STREQ(i18n::month_long(7), "août");
  EXPECT_STREQ(i18n::month_long(11), "décembre");
  EXPECT_STREQ(i18n::month_short(1), "févr.");
  EXPECT_STREQ(i18n::month_short(8), "sept.");
}

TEST(I18n, CalendarNamesInEnglish) {
  LangGuard g(i18n::Lang::kEn);
  EXPECT_STREQ(i18n::weekday_long(0), "Sunday");
  EXPECT_STREQ(i18n::weekday_long(3), "Wednesday");
  EXPECT_STREQ(i18n::weekday_long(6), "Saturday");
  EXPECT_STREQ(i18n::weekday_short(2), "Tue");
  EXPECT_STREQ(i18n::weekday_abbr(4), "Thu");
  EXPECT_STREQ(i18n::month_long(0), "January");
  EXPECT_STREQ(i18n::month_long(11), "December");
  EXPECT_STREQ(i18n::month_short(8), "Sep");
}

TEST(I18n, OutOfRangeCalendarIndexIsEmptyNotNull) {
  EXPECT_STREQ(i18n::weekday_long(-1), "");
  EXPECT_STREQ(i18n::weekday_long(7), "");
  EXPECT_STREQ(i18n::weekday_short(7), "");
  EXPECT_STREQ(i18n::weekday_abbr(-1), "");
  EXPECT_STREQ(i18n::month_long(12), "");
  EXPECT_STREQ(i18n::month_short(-1), "");
}

// Ces noms sont aussi les options du select Home Assistant : ASCII, et
// indépendants de la langue affichée.
TEST(I18n, LanguageNamesAreTheSameInEveryLanguage) {
  EXPECT_STREQ(i18n::language_name(i18n::Lang::kFr), "Francais");
  EXPECT_STREQ(i18n::language_name(i18n::Lang::kEn), "English");
  LangGuard g(i18n::Lang::kEn);
  EXPECT_STREQ(i18n::language_name(i18n::Lang::kFr), "Francais");
}
