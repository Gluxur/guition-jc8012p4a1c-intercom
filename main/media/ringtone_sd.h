#pragma once

#include <cstddef>
#include <cstdint>

// Chargement de la sonnerie personnalisée depuis la carte microSD.
//
// Seule couche IO de la fonctionnalité : media/wav_parse.cpp ne fait que du
// décodage et media/ringtone.cpp que du mixage, tous deux compilés et testés
// sur PC. Ce fichier-ci touche le système de fichiers, il ne l'est pas.
//
// Pourquoi un préchargement intégral et pas un streaming : le contrôleur SDMMC
// est partagé avec la liaison Wi-Fi (esp_hosted). Une lecture SD pendant une
// session ajouterait de la gigue au drain TCP du flux MJPEG, qui est le
// facteur limitant du débit vidéo. Le préchargement supprime toute IO du
// chemin chaud et préserve le contrat de ringtone : mixeur PASSIF, sans tâche.
namespace ringtone_sd {

enum class Status : uint8_t {
  kAbsent,    // pas de carte, ou pas de fichier : sonnerie interne
  kLoaded,    // WAV installé
  kBadFormat, // en-tête refusé : sonnerie interne
  kTooBig,    // WAV installé, tronqué au plafond de 30 s
  kIoError,   // lecture ou allocation en échec : sonnerie interne
};

// Longueur maximale d'un nom de fichier rendu par list_wav(), alignée sur la
// borne du champ NVS correspondant (prefs::kRingFileMax).
inline constexpr size_t kNameMax = 64;

// Plafond de fichiers listés. Une racine de carte n'est pas une médiathèque :
// au-delà, la liste déroulante devient inutilisable au doigt bien avant d'être
// pleine, et le tampon reste borné (kMaxFiles * kNameMax octets).
inline constexpr size_t kMaxFiles = 16;

// Lit le WAV désigné par prefs::ring_file() à la racine de la carte et
// l'installe via ringtone::set_source(). Nom vide = sonnerie interne : la
// source est désinstallée, la mélodie synthétisée reprend.
//
// Premier appel depuis app_main, après sd_log::start() (carte montée) et AVANT
// audio_pipeline::init(). Rappelable ENSUITE à chaud (sortie du mode clé USB,
// changement de fichier depuis la page Configuration) : la fonction désarme la
// sonnerie et laisse le mixeur relâcher l'ancien bloc avant de le libérer.
void load();

// Noms des .wav de la racine, triés alphabétiquement, sans chemin. Retourne le
// nombre écrit (<= max). Les fichiers cachés sont ignorés : une carte passée
// sur un Mac se retrouve pleine de « ._nom.wav » qui ne sont pas des WAV.
size_t list_wav(char out[][kNameMax], size_t max);

[[nodiscard]] Status status();

// Ligne prête à afficher sur la page Configuration ("ring.wav : 4,2 s",
// "ring.wav absent - sonnerie interne", "ring.wav rejeté : 8000 Hz requis").
// Toujours non nulle. Le journal, lui, porte le détail chiffré.
[[nodiscard]] const char *summary();

} // namespace ringtone_sd
