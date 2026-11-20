#pragma once

#include <cstddef>
#include <cstdint>

// Décodage de l'en-tête d'un fichier RIFF/WAVE. Logique PURE : aucune IO,
// aucun header de driver — c'est ce qui permet de la compiler et de la tester
// sur PC (test/), contrairement à media/ringtone_sd.cpp qui lit la carte SD.
//
// Le projet n'accepte QUE du PCM 16 bits mono 8 kHz : c'est le format que
// bsp::Bsp::play_pcm consomme directement, et la conversion est triviale côté
// PC (ffmpeg -i son.mp3 -ar 8000 -ac 1 -c:a pcm_s16le ring.wav). Accepter
// 44,1 kHz imposerait un rééchantillonnage fractionnaire — aliasing audible et
// code délicat à valider — pour un besoin que cette commande couvre.
namespace wav {

enum class Error : uint8_t {
  kOk,
  kTruncated, // fenêtre d'en-tête trop courte, ou longueur de chunk aberrante
  kNotRiff,
  kNotWave,
  kNoFmt,
  kNotPcm16,
  kBadRate,
  kNotMono,
  kNoData,
};

struct Info {
  uint32_t data_offset = 0; // octet du premier échantillon dans le fichier
  uint32_t data_bytes = 0;  // taille déclarée du bloc `data`
  // Renseignés dès que le chunk `fmt ` est lu, MÊME si parse() retourne
  // ensuite une erreur : c'est ce qui permet au journal de nommer la valeur
  // fautive ("44100 Hz") au lieu d'un "format invalide" inexploitable.
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  uint16_t bits = 0;
};

// `hdr` = les premiers octets du fichier. 4 Ko couvrent largement les chunks
// LIST/INFO que ffmpeg et Audacity insèrent entre `fmt ` et `data`.
[[nodiscard]] Error parse(const uint8_t *hdr, size_t len, Info &out);

// Libellé court, destiné au journal ET au label de la page Configuration :
// pas de tiret cadratin ni de symbole hors 0x20-0xFF (police montserrat_fr).
[[nodiscard]] const char *error_text(Error e);

} // namespace wav
