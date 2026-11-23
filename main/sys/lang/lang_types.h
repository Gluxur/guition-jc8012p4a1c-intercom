#pragma once

// Forme commune des fichiers de langue (fr.h, en.h). Inclus seulement par
// sys/i18n.cpp : les tables n'existent qu'une fois en flash, et c'est là
// qu'elles sont vérifiées (cf. sys/i18n.h).
#include "sys/i18n.h"

namespace i18n::detail {

struct Entry {
  Str id;
  const char *text;
};

struct Language {
  const char *name; // dans sa propre langue, ASCII (option du select HA)
  bool month_before_day;
  const char *weekday_long[7]; // struct tm : 0 = dimanche
  const char *weekday_short[7];
  const char *weekday_abbr[7];
  const char *month_long[12];
  const char *month_short[12];
};

} // namespace i18n::detail
