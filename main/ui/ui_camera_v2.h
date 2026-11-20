#pragma once

#include "ui.h"
#include <cstdint>

namespace ui_camera_v2 {

/* Crée l'écran caméra V2 (vidéo plein écran + bandeau de contrôles de 80 px
 * superposé en bas) et démarre le timer de composition du bandeau. À appeler
 * depuis ui::create_screens — mutex LVGL déjà tenu. */
void create_screen(const ui::Callbacks &cbs);

/* Rectangle (coords physiques panneau) de la zone vidéo : le panneau entier,
 * (0,0,800,1280). C'est cette géométrie qui autorise le chemin zéro-copie de
 * Bsp::direct_video_target (largeur source == kLcdHRes, dst_x == 0). */
void video_zone_physical_rect(uint32_t &x, uint32_t &y, uint32_t &w,
                              uint32_t &h);

/* À appeler au démarrage d'un stream : compose le bandeau pour qu'il soit
 * publié avant la première frame vidéo. */
void prepare_stream();

/* Met à jour le label de stats du bandeau. Sûr depuis n'importe quelle tâche
 * (prend le mutex LVGL). */
void update_stats(const char *text);

/* Re-pose le bandeau dans le back-buffer DSI courant. Appelée par la tâche de
 * rendu MJPEG après chaque décode, avant present_frame() : le décodeur écrit
 * tout le framebuffer, contrôles compris. Ne prend PAS le mutex LVGL (chemin
 * chaud) — la synchronisation avec la composition passe par un pointeur
 * atomique publié. */
void band_blit();

/* Charge l'écran et lance start_camera_v2() via une tâche différée (évite le
 * deadlock LVGL au clic). */
void open_screen();

/* Idem, depuis un callback d'événement LVGL (mutex déjà tenu). */
void open_screen_from_event();

} // namespace ui_camera_v2
