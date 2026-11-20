#pragma once

#include <cstddef>

// Décodage PUR du journal d'appels (/sdcard/calls.log). Aucun accès disque,
// aucun header de driver : c'est ce qui permet de le compiler et de le tester
// sur PC (test/), comme net/weather_parse et net/gcal_parse. L'ouverture du
// fichier vit dans call_history.cpp.
//
// Format d'une ligne, écrit par call_history::append_log :
//   AAAA-MM-JJ HH:MM:SS,MANQUE,/sdcard/calls/call_007.jpg
//   AAAA-MM-JJ HH:MM:SS,REPONDU,
namespace call_log {

constexpr size_t kStampMax = 24;
constexpr size_t kPathMax = 80;

struct Entry {
  char stamp[kStampMax];
  char path[kPathMax]; // "" quand aucune capture n'a été archivée
  bool missed;
};

// Décode une ligne. `len` peut inclure un '\r' et/ou un '\n' finaux.
// false si la ligne est malformée : moins de deux virgules, verdict inconnu,
// horodatage vide, ou champ trop long pour Entry.
bool parse_line(const char *line, size_t len, Entry &out);

// Découpe un bloc de texte en lignes et remplit `out` avec les `max` DERNIÈRES
// entrées valides, LA PLUS RÉCENTE EN PREMIER (c'est l'ordre d'affichage).
// `first_partial` : ignorer la première ligne parce que le bloc commence au
// milieu d'une ligne — le cas quand on ne relit que la queue d'un gros fichier.
// Renvoie le nombre d'entrées écrites.
size_t parse_tail(const char *text, size_t len, bool first_partial, Entry *out,
                  size_t max);

} // namespace call_log
