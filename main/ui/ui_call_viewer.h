#pragma once

// Visionneuse d'une capture d'appel manqué (JPEG relu depuis la carte SD).
//
// Pourquoi un écran à part et pas un simple widget lv_image : les captures sont
// en 800x1280, l'orientation PHYSIQUE du panneau, parce que le pipeline vidéo
// contourne exprès la rotation 270° du flush LVGL (cf. piège n°9 de CLAUDE.md).
// Un lv_image les afficherait dans l'espace LOGIQUE (1280x800) donc couchées.
// L'image passe donc par le même chemin matériel que le flux live
// (video_mjpeg::render_still), et seuls les contrôles restent en LVGL.
namespace ui_call_viewer {

// À appeler une fois au boot, depuis ui::create_screens (mutex LVGL tenu).
void create_screen();

// Affiche la capture `path`. À appeler depuis un callback d'événement LVGL
// (mutex déjà tenu) : la lecture du fichier et le décodage partent dans une
// tâche dédiée, ils dureraient trop longtemps sous le mutex.
// `on_back` : écran à recharger au bouton Retour (et sur échec d'affichage).
// La visionneuse est atteignable depuis deux endroits — la notification de la
// page météo et la page Appels — et doit revenir d'où elle vient. Attendu :
// une fonction sûre en contexte d'événement LVGL (mutex déjà tenu).
// nullptr = retour à la page météo.
void open_screen_from_event(const char *path, void (*on_back)());

} // namespace ui_call_viewer
