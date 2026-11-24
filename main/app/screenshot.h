#pragma once

// Captures d'écran de l'interface vers la carte SD, pour la documentation.
// Fichiers : cfg::sd::kScreenshotDir/<fr|en>_<page>.bmp (RGB565, 1280x800,
// ~2 Mo). Pilotées depuis Home Assistant (boutons de diagnostic).
//
// Refusées pendant une session caméra (direct_mode : LVGL ne dessine plus
// l'écran, une capture montrerait la page d'avant) et en mode clé USB (l'hôte
// possède la carte). Une seule à la fois ; une requête pendant un tour est
// ignorée.
namespace screenshot {

// Crée la tâche. Une fois au boot, après l'UI.
void init();

// Capture la page affichée. Non bloquant, sûr depuis la tâche de l'API.
void request_current();

// Fait le tour des pages (ui::kPageNames), une capture chacune, puis revient à
// l'accueil. ~3 s par page. Non bloquant. Pour l'autre langue : changer la
// langue, redémarrer, relancer.
void request_all();

} // namespace screenshot
