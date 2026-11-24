#pragma once

// Préférences applicatives locales, persistées en NVS et appliquées À CHAUD.
//
// Pourquoi un module distinct de sys/settings : settings::save() n'applique
// rien immédiatement par contrat (les modules réseau capturent leurs pointeurs
// d'URL au boot, une reconfiguration à chaud pendant un flux est la voie des
// bugs difficiles). Un réglage de confort comme « Ne pas déranger » doit au
// contraire prendre effet au doigt levé, sans redémarrage.
//
// Namespace NVS propre (`ui_prefs`) : ni celui de settings (`net_cfg`) ni
// celui du BSP (`audio_cfg`).
#include <cstddef>
#include <cstddef>
#include <cstdint>

#include "sys/i18n.h"

namespace prefs {

// Borne d'un nom de fichier de sonnerie. 63 caractères + NUL : au-delà, ce
// n'est plus un nom court de racine FAT et la clé NVS grossit pour rien.
inline constexpr size_t kRingFileMax = 64;

// Charge la NVS. À appeler APRÈS nvs_flash_init et AVANT l'UI.
void init();

// Ne pas déranger : la sonnerie est muette, l'écran caméra s'ouvre quand même.
// Lu depuis la tâche de l'API, écrit depuis la tâche LVGL.
[[nodiscard]] bool dnd();

// Applique et persiste. Un échec d'écriture NVS ne bloque pas l'application
// en mémoire (le réglage vaut pour la session, il sera perdu au reboot).
void set_dnd(bool on);

// Overlay de diagnostic FPS/CPU/RAM. Défaut : masqué. Sur la page caméra V2 il
// coûte une recomposition complète du bandeau (lv_snapshot 1280x80 + blend de
// 102 400 px, mutex LVGL tenu ~100 ms) à chaque rafraîchissement, donc une frame
// vidéo perdue : ce n'est un réglage de confort que quand on diagnostique.
// Lu depuis les tâches de rendu, écrit depuis la tâche LVGL.
[[nodiscard]] bool show_stats();

// Applique et persiste. Même contrat que set_dnd (effet immédiat).
void set_show_stats(bool on);

// Volume de la sonnerie, 0-100 %. S'applique au WAV de la carte SD comme au
// ding-dong synthétisé. Défaut 100. Lu au boot pour initialiser
// ringtone::set_volume(), puis écrit depuis la tâche LVGL.
[[nodiscard]] uint8_t ring_volume();

// Applique et persiste. Même contrat que set_dnd (effet immédiat). Appelé au
// RELÂCHEMENT du slider et non à chaque pixel : écrire la NVS au rythme de
// LV_EVENT_VALUE_CHANGED userait le flash pour rien.
void set_ring_volume(uint8_t percent);

// Nom du WAV de sonnerie à la racine de la carte ("ring.wav" par défaut).
// Chaîne vide = sonnerie interne synthétisée, choix explicite de l'utilisateur.
// Pointeur stable pour toute la vie du programme (buffer statique).
[[nodiscard]] const char *ring_file();

// Applique et persiste. `name` est un nom NU (sans chemin) : c'est ringtone_sd
// qui possède le chemin de la racine, l'UI n'en compose jamais. nullptr ou
// chaîne vide sélectionne la sonnerie interne.
void set_ring_file(const char *name);

// Écriture du journal sur la carte microSD. Défaut : activée. Coupée, la carte
// reste montée (ring.wav, mode clé USB) mais plus aucune ligne n'y est écrite —
// ce qui épargne les cycles d'écriture de la carte quand on ne diagnostique
// rien. Lu par la tâche sd_log, écrit depuis la tâche LVGL.
[[nodiscard]] bool sd_log_enabled();

// Applique et persiste. Même contrat que set_dnd (effet immédiat).
void set_sd_log_enabled(bool on);

// Canal de mises à jour. Défaut : stable (/releases/latest, que GitHub calcule
// sans les pre-releases). Bêta : la plus récente release publiée, pre-release
// comprise. Lu par la tâche ota à chaque vérification, écrit depuis la tâche de
// l'API (entité Home Assistant) : c'est l'appelant qui relance la vérification
// (sys/ota_ctrl), pour que l'entité update change de version tout de suite.
[[nodiscard]] bool beta_channel();

// Applique et persiste. Même contrat que set_dnd (effet immédiat).
void set_beta_channel(bool on);

// Serveur de fichiers Wi-Fi. Défaut : éteint. Toute IO sur la carte ajoute de
// la gigue au drain TCP du flux MJPEG ; un serveur permanent serait une porte
// ouverte en permanence sur ce chemin. Lu au boot par app_main, écrit depuis la
// tâche LVGL et depuis la tâche de l'API (entité Home Assistant).
[[nodiscard]] bool file_server();

// Persiste SEULEMENT. Comme set_sd_log_enabled, ce setter n'agit pas sur le
// module concerné : c'est l'appelant qui fait la paire (file_server::start /
// stop puis cette fonction). Sans quoi sys/ dépendrait de net/, à rebours du
// sens des couches.
void set_file_server(bool on);

// Annulation d'écho du backchannel micro (sans elle le visiteur s'entend dans
// la platine : HP de la tablette -> micro). Deux modes esp-sr seulement, les
// autres ont été mesurés inutilisables sur ce silicium (VOIP_LOW_COST à 450 %
// d'un cœur). Coût mesuré en session vidéo de nuit : FD_HIGH_PERF ~38 % du
// cœur 1, 8,5 Ko de SRAM interne ; FD_LOW_COST ~30 % moins de CPU mais 12,7 Ko
// de SRAM interne — c'est la SRAM, pas le CPU, qui manque le plus en appel.
enum class EchoCancel : uint8_t { kOff = 0, kLowCost = 1, kHighPerf = 2 };

// Défaut : kLowCost. Lu par mic_send au DÉCROCHÉ : un changement en cours
// d'appel vaut pour l'appel suivant. Écrit depuis la tâche LVGL.
[[nodiscard]] EchoCancel echo_cancel();

// Applique et persiste. Même contrat que set_dnd.
void set_echo_cancel(EchoCancel mode);

// Palette de l'interface. kVice = coucher de soleil violet/rose/orange ;
// kClassic = palette d'origine, conservée comme option de repli. Les écrans
// sont construits UNE fois au boot avec la palette lue ici : contrairement aux
// autres préférences, un changement ne vaut qu'au redémarrage suivant.
// Écrit depuis la tâche LVGL (page Configuration), lu une fois par
// ui::create_screens().
enum class UiTheme : uint8_t { kClassic = 0, kVice = 1 };

// Défaut : kVice.
[[nodiscard]] UiTheme ui_theme();

// Persiste. L'effet n'est visible qu'après redémarrage (cf. ci-dessus).
void set_ui_theme(UiTheme theme);

// Langue de l'interface. Même contrat que le thème : l'effet n'est visible
// qu'au redémarrage suivant (cf. sys/i18n.h, qui fige la valeur lue au boot).
// Écrit depuis la tâche LVGL (page Configuration) et la tâche de l'API (select
// Home Assistant), lu une fois par app_main avant l'UI.
// Défaut : français.
[[nodiscard]] i18n::Lang language();

// Persiste seulement (cf. ci-dessus).
void set_language(i18n::Lang lang);

// Identifiants d'entités Home Assistant utilisés par la tablette, éditables
// depuis la page de l'appareil dans HA (entités texte, app/ha_api). Appliqués
// à chaud. Clé absente : défaut compilé (cfg::kHa*Entity, surchargeable dans
// app_secrets.h) ; chaîne vide stockée = entité volontairement non configurée.
enum class HaEntity : uint8_t { kGate, kWeather, kCalendar };

// Copie sous verrou : écrit par la tâche de l'API, lu par les tâches de fetch
// météo/agenda et par app/gate. Sûr depuis n'importe quelle tâche.
void ha_entity(HaEntity which, char *out, size_t cap);

// Persiste et incrémente ha_entities_generation(). La valeur doit déjà être
// validée (ha_entity_id::normalize).
void set_ha_entity(HaEntity which, const char *normalized);

// +1 à chaque set_ha_entity : l'écran météo s'en sert pour relancer un fetch.
[[nodiscard]] uint32_t ha_entities_generation();

// Compteur de démarrages, persisté en NVS. Sert au bandeau écrit en tête de
// chaque bloc du journal SD : il donne un repère stable pour recoller les
// blocs d'un même fichier là où l'horodatage peut manquer (avant SNTP).
[[nodiscard]] uint32_t boot_count();

// Incrémente et persiste. À appeler une seule fois par démarrage.
void bump_boot_count();

} // namespace prefs
