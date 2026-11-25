#pragma once

#include <cstddef>
#include <cstdint>

// Écriture d'images de diagnostic sur la carte microSD. Jamais sur un chemin
// chaud : le contrôleur SDMMC est partagé avec la liaison Wi-Fi (slot 1), et
// chaque écriture ajoute de la gigue au drain MJPEG. Appelant : une tâche de
// fond, pile >= 8 Ko (FATFS).
namespace sd_image {

// Carte montée et pas exportée en clé USB (l'hôte USB possède alors le FAT).
[[nodiscard]] bool sd_ready();

// PGM binaire (P5) 8 bits, `w` x `h` octets contigus. `comment` (une ligne,
// sans '\n') est écrit en « # ... » dans l'en-tête ; nullptr pour aucun.
// POSIX open/write et non FILE* : cf. docs/hardware-notes.md (picolibc déborde
// d'un octet du tampon FILE).
[[nodiscard]] bool write_pgm(const char *path, const uint8_t *gray, int w,
                             int h, const char *comment);

// BMP RGB565 (sys/bmp_header) depuis un tampon LVGL : `stride` octets par
// ligne dans `px`. Écrit ligne par ligne, chaque write() <= une ligne d'écran
// (2,5 Ko) : borne la rafale sur le contrôleur SDMMC partagé avec le Wi-Fi.
[[nodiscard]] bool write_bmp565(const char *path, const uint8_t *px, int w,
                                int h, size_t stride);

// Crée `dir` s'il manque, puis supprime les plus anciens fichiers `*<ext>` pour
// n'en garder que `keep - 1` : la place du fichier que l'appelant va écrire.
// « Plus ancien » = plus petit nom : les noms doivent trier dans l'ordre
// chronologique (horodatage en tête). False si `dir` est inutilisable.
[[nodiscard]] bool make_room(const char *dir, const char *ext, int keep);

} // namespace sd_image
