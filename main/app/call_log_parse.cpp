#include "app/call_log_parse.h"

#include <cstring>

namespace call_log {
namespace {

constexpr char kMissed[] = "MANQUE";
constexpr char kAnswered[] = "REPONDU";

// Longueur de la ligne sans ses fins de ligne. Le journal est écrit en mode
// texte sur une partition FAT : un '\r' peut s'y glisser si le fichier a
// transité par le PC en mode clé USB.
size_t trim_eol(const char *line, size_t len) {
  while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
    len--;
  }
  return len;
}

// Borne de la prochaine ligne à partir de `i`. Renvoie l'index du '\n' (ou
// `len` si la dernière ligne n'en a pas).
size_t line_end(const char *text, size_t len, size_t i) {
  const char *nl = static_cast<const char *>(memchr(text + i, '\n', len - i));
  return nl == nullptr ? len : static_cast<size_t>(nl - text);
}

} // namespace

bool parse_line(const char *line, size_t len, Entry &out) {
  if (line == nullptr) {
    return false;
  }
  len = trim_eol(line, len);
  if (len == 0) {
    return false;
  }

  const char *c1 = static_cast<const char *>(memchr(line, ',', len));
  if (c1 == nullptr) {
    return false;
  }
  const size_t stamp_len = static_cast<size_t>(c1 - line);
  const char *rest = c1 + 1;
  const size_t rest_len = len - stamp_len - 1;

  const char *c2 = static_cast<const char *>(memchr(rest, ',', rest_len));
  if (c2 == nullptr) {
    return false;
  }
  const size_t verdict_len = static_cast<size_t>(c2 - rest);
  const char *path = c2 + 1;
  const size_t path_len = rest_len - verdict_len - 1;

  // Un horodatage vide ou un champ plus long que la structure = ligne corrompue
  // (fichier tronqué, écriture interrompue par une coupure) : on la saute au
  // lieu d'afficher un appel inventé.
  if (stamp_len == 0 || stamp_len >= kStampMax || path_len >= kPathMax) {
    return false;
  }

  bool missed;
  if (verdict_len == sizeof(kMissed) - 1 &&
      memcmp(rest, kMissed, verdict_len) == 0) {
    missed = true;
  } else if (verdict_len == sizeof(kAnswered) - 1 &&
             memcmp(rest, kAnswered, verdict_len) == 0) {
    missed = false;
  } else {
    return false;
  }

  memcpy(out.stamp, line, stamp_len);
  out.stamp[stamp_len] = '\0';
  memcpy(out.path, path, path_len);
  out.path[path_len] = '\0';
  out.missed = missed;
  return true;
}

size_t parse_tail(const char *text, size_t len, bool first_partial, Entry *out,
                  size_t max) {
  if (text == nullptr || out == nullptr || max == 0) {
    return 0;
  }

  // Deux passes plutôt qu'un tampon circulaire : la première compte les lignes
  // valides, la seconde ne décode que les `max` dernières, directement dans
  // `out`. Aucune rotation d'index à écrire, et le surcoût est un second
  // balayage de 16 Ko — invisible devant la lecture SD qui vient de l'apporter.
  size_t total = 0;
  {
    Entry scratch;
    bool skip = first_partial;
    for (size_t i = 0; i < len;) {
      const size_t end = line_end(text, len, i);
      if (skip) {
        skip = false;
      } else if (end > i && parse_line(text + i, end - i, scratch)) {
        total++;
      }
      i = end + 1;
    }
  }
  if (total == 0) {
    return 0;
  }

  const size_t keep = total < max ? total : max;
  size_t to_skip = total - keep;
  size_t n = 0;
  {
    bool skip_partial = first_partial;
    for (size_t i = 0; i < len && n < keep;) {
      const size_t end = line_end(text, len, i);
      if (skip_partial) {
        skip_partial = false;
      } else if (end > i && parse_line(text + i, end - i, out[n])) {
        if (to_skip > 0) {
          to_skip--; // entrée trop ancienne : `out[n]` sera réécrite
        } else {
          n++;
        }
      }
      i = end + 1;
    }
  }

  // `out[0..n)` est chronologique ; la page veut le plus récent en tête.
  for (size_t a = 0, b = n; a + 1 < b; a++, b--) {
    Entry tmp = out[a];
    out[a] = out[b - 1];
    out[b - 1] = tmp;
  }
  return n;
}

} // namespace call_log
