#include "sys/i18n.h"

#include <array>
#include <atomic>
#include <cstddef>

#include "sys/lang/en.h"
#include "sys/lang/fr.h"

namespace i18n {
namespace {

using detail::Entry;
using detail::Language;

constexpr size_t kCount = static_cast<size_t>(Str::kCount);
using Table = std::array<const char *, kCount>;

// --- Vérifications à la compilation -----------------------------------------
// Tout est constexpr : une table incomplète ou un format incohérent ne compile
// pas. Le message de static_assert ne peut pas nommer la clé fautive ; le test
// host (test_i18n) le fait.

template <size_t N> constexpr bool each_key_once(const Entry (&e)[N]) {
  if (N != kCount) {
    return false;
  }
  std::array<int, kCount> seen{};
  for (const Entry &x : e) {
    const size_t i = static_cast<size_t>(x.id);
    if (i >= kCount || x.text == nullptr || x.text[0] == '\0' || seen[i]++ != 0) {
      return false;
    }
  }
  return true;
}

template <size_t N> constexpr Table index(const Entry (&e)[N]) {
  Table t{};
  for (const Entry &x : e) {
    t[static_cast<size_t>(x.id)] = x.text;
  }
  return t;
}

constexpr bool is_conversion(char c) {
  for (const char *p = "diouxXeEfFgGaAcsp"; *p != '\0'; ++p) {
    if (*p == c) {
      return true;
    }
  }
  return false;
}

// Avance jusqu'à la prochaine conversion printf et renvoie son spécificateur
// complet [début, fin[ (drapeaux, largeur, longueur, conversion). "%%" n'en est
// pas une ; un '%' final non suivi d'une conversion (« 0% » affiché tel quel
// par lv_label_set_text) non plus.
struct Spec {
  const char *begin = nullptr;
  const char *end = nullptr;
};

constexpr Spec next_spec(const char *&p) {
  for (; *p != '\0'; ++p) {
    if (*p != '%') {
      continue;
    }
    if (p[1] == '%') {
      ++p;
      continue;
    }
    const char *q = p + 1;
    while (*q != '\0' && !is_conversion(*q)) {
      ++q;
    }
    if (*q == '\0') {
      p = q;
      return {};
    }
    Spec s{p, q + 1};
    p = q + 1;
    return s;
  }
  return {};
}

constexpr bool same_specs(const char *a, const char *b) {
  for (;;) {
    const Spec sa = next_spec(a);
    const Spec sb = next_spec(b);
    if (sa.begin == nullptr || sb.begin == nullptr) {
      return sa.begin == sb.begin;
    }
    if (sa.end - sa.begin != sb.end - sb.begin) {
      return false;
    }
    for (const char *x = sa.begin, *y = sb.begin; x != sa.end; ++x, ++y) {
      if (*x != *y) {
        return false;
      }
    }
  }
}

constexpr bool formats_match(const Table &a, const Table &b) {
  for (size_t i = 0; i < kCount; ++i) {
    if (!same_specs(a[i], b[i])) {
      return false;
    }
  }
  return true;
}

static_assert(each_key_once(lang_fr::kStrings),
              "sys/lang/fr.h : clé manquante, en double ou texte vide");
static_assert(each_key_once(lang_en::kStrings),
              "sys/lang/en.h : clé manquante, en double ou texte vide");

constexpr Table kFr = index(lang_fr::kStrings);
constexpr Table kEn = index(lang_en::kStrings);

static_assert(formats_match(kFr, kEn),
              "fr.h / en.h : un texte n'a pas les mêmes conversions printf "
              "(%s, %d...) dans les deux langues");

// Indexées par Lang.
constexpr const Table *kTables[] = {&kFr, &kEn};
constexpr const Language *kLanguages[] = {&lang_fr::kLanguage, &lang_en::kLanguage};
static_assert(sizeof(kTables) / sizeof(kTables[0]) ==
                  static_cast<size_t>(kLastLang) + 1,
              "une table par langue");

// Écrit une fois au boot avant la création des tâches qui la lisent ; atomic
// quand même pour qu'aucune lecture inter-tâches ne repose sur cet ordre.
std::atomic<Lang> s_lang{Lang::kFr};

size_t cur() { return static_cast<size_t>(s_lang.load(std::memory_order_relaxed)); }

const char *pick(const char *const *names, int i, int n) {
  return (i < 0 || i >= n) ? "" : names[i];
}

} // namespace

void init(Lang lang) {
  if (static_cast<uint8_t>(lang) > static_cast<uint8_t>(kLastLang)) {
    lang = Lang::kFr;
  }
  s_lang.store(lang, std::memory_order_relaxed);
}

Lang lang() { return s_lang.load(std::memory_order_relaxed); }

const char *t(Str id) {
  const size_t i = static_cast<size_t>(id);
  return i < kCount ? (*kTables[cur()])[i] : "";
}

const char *weekday_long(int wday) {
  return pick(kLanguages[cur()]->weekday_long, wday, 7);
}

const char *weekday_short(int wday) {
  return pick(kLanguages[cur()]->weekday_short, wday, 7);
}

const char *weekday_abbr(int wday) {
  return pick(kLanguages[cur()]->weekday_abbr, wday, 7);
}

const char *month_long(int mon) { return pick(kLanguages[cur()]->month_long, mon, 12); }

const char *month_short(int mon) {
  return pick(kLanguages[cur()]->month_short, mon, 12);
}

bool month_before_day() { return kLanguages[cur()]->month_before_day; }

const char *language_name(Lang lang) {
  const size_t i = static_cast<size_t>(lang);
  return i <= static_cast<size_t>(kLastLang) ? kLanguages[i]->name : "";
}

} // namespace i18n
