#include "media/wav_parse.h"

#include <cstring>

namespace wav {
namespace {

constexpr size_t kRiffHeader = 12;    // "RIFF" + taille + "WAVE"
constexpr size_t kChunkHeader = 8;    // identifiant + taille
constexpr uint32_t kFmtMinBytes = 16; // corps d'un `fmt ` PCM
constexpr uint16_t kFormatPcm = 1;
constexpr uint32_t kRequiredRate = 8000;
constexpr uint16_t kRequiredBits = 16;
constexpr uint16_t kRequiredChannels = 1;

// RIFF est little-endian par définition (RIFX est sa variante big-endian, que
// l'on refuse). Lecture octet par octet plutôt qu'un cast : le tampon n'a
// aucune garantie d'alignement, et un accès non aligné piège sur RISC-V.
uint16_t rd_u16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool id_is(const uint8_t *p, const char *id) { return memcmp(p, id, 4) == 0; }

} // namespace

Error parse(const uint8_t *hdr, size_t len, Info &out) {
  out = Info{};
  if (hdr == nullptr || len < kRiffHeader) {
    return Error::kTruncated;
  }
  if (!id_is(hdr, "RIFF")) {
    return Error::kNotRiff;
  }
  if (!id_is(hdr + 8, "WAVE")) {
    return Error::kNotWave;
  }

  bool fmt_seen = false;
  // `pos` désigne toujours l'en-tête du chunk courant. La boucle progresse d'au
  // moins kChunkHeader à chaque tour, donc elle termine quoi qu'annonce le
  // fichier.
  size_t pos = kRiffHeader;
  while (pos + kChunkHeader <= len) {
    const uint8_t *ch = hdr + pos;
    const uint32_t size = rd_u32(ch + 4);
    const size_t body = pos + kChunkHeader; // <= len (condition de boucle)

    if (id_is(ch, "fmt ")) {
      // `size > len - body` et pas seulement le minimum PCM : une taille forgée
      // (0xFFFFFFF8) faisait reboucler `pos` sur ce même chunk en size_t 32
      // bits -> boucle infinie dans app_main, TWDT, reboot à chaque démarrage.
      if (size < kFmtMinBytes || body + kFmtMinBytes > len ||
          size > len - body) {
        return Error::kTruncated;
      }
      const uint8_t *f = hdr + body;
      // Renseignés AVANT les refus : le journal doit pouvoir nommer la valeur
      // fautive, pas seulement dire que le format est mauvais.
      out.channels = rd_u16(f + 2);
      out.sample_rate = rd_u32(f + 4);
      out.bits = rd_u16(f + 14);
      if (rd_u16(f) != kFormatPcm || out.bits != kRequiredBits) {
        return Error::kNotPcm16;
      }
      if (out.channels != kRequiredChannels) {
        return Error::kNotMono;
      }
      if (out.sample_rate != kRequiredRate) {
        return Error::kBadRate;
      }
      fmt_seen = true;
    } else if (id_is(ch, "data")) {
      if (!fmt_seen) {
        return Error::kNoFmt;
      }
      if (size == 0) {
        return Error::kNoData;
      }
      // Seul chunk autorisé à déclarer plus que la fenêtre : l'appelant ne lit
      // que les premiers Ko du fichier, le corps du bloc est ailleurs.
      out.data_offset = static_cast<uint32_t>(body);
      out.data_bytes = size;
      return Error::kOk;
    } else if (size > len - body) {
      // Chunk ignoré dont la longueur déclarée sort du tampon : fichier
      // corrompu ou forgé. Le suivre ferait lire hors bornes.
      return Error::kTruncated;
    }

    // Bourrage RIFF : un chunk de taille impaire est suivi d'un octet qui ne
    // compte pas dans sa taille déclarée.
    // Calcul en 64 bits : garantit la progression quel que soit size_t (le
    // test host, en 64 bits, ne verrait jamais le rebouclage de la cible).
    const uint64_t next = static_cast<uint64_t>(body) + size + (size & 1u);
    if (next > len) {
      break;
    }
    pos = static_cast<size_t>(next);
  }
  return fmt_seen ? Error::kNoData : Error::kNoFmt;
}

const char *error_text(Error e) {
  switch (e) {
  case Error::kOk:
    return "ok";
  case Error::kTruncated:
    return "fichier tronqué";
  case Error::kNotRiff:
    return "pas un fichier RIFF";
  case Error::kNotWave:
    return "RIFF mais pas WAVE";
  case Error::kNoFmt:
    return "chunk fmt absent";
  case Error::kNotPcm16:
    return "PCM 16 bits requis";
  case Error::kBadRate:
    return "8000 Hz requis";
  case Error::kNotMono:
    return "mono requis";
  case Error::kNoData:
    return "chunk data absent ou vide";
  }
  return "erreur inconnue";
}

} // namespace wav
