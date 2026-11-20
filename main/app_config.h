#pragma once

// Configuration compilée du projet, en un seul point.
//
// Deux natures de valeurs, volontairement séparées :
//   - `cfg::defaults::*` : ce qui change d'une installation à l'autre (WiFi,
//     hôte du serveur, identifiants). Ce ne sont QUE des défauts : la valeur
//     effective vient de la NVS via `settings::` (page Configuration de
//     l'écran tactile). Ne jamais lire ces constantes ailleurs que dans
//     settings.cpp.
//   - le reste : ports, chemins d'API, topics MQTT, entité HA. Propriétés du
//     déploiement go2rtc/Home Assistant, identiques sur toutes les cartes, non
//     éditables à l'écran. Lisibles partout.
//
// `inline constexpr` et non `#define` : les macros n'ont ni type, ni portée,
// ni existence pour le débogueur, et fuient dans toutes les unités de
// compilation. `inline` (et pas seulement `constexpr`) garantit un objet
// unique au lieu d'une copie par TU.

#if __has_include("app_secrets.h")
#include "app_secrets.h"
#endif

#ifndef APP_SECRET_WIFI_SSID
#define APP_SECRET_WIFI_SSID ""
#endif
#ifndef APP_SECRET_WIFI_PASS
#define APP_SECRET_WIFI_PASS ""
#endif
#ifndef APP_SECRET_HOST
#define APP_SECRET_HOST ""
#endif
#ifndef APP_SECRET_MQTT_USER
#define APP_SECRET_MQTT_USER ""
#endif
#ifndef APP_SECRET_MQTT_PASS
#define APP_SECRET_MQTT_PASS ""
#endif
#ifndef APP_SECRET_HA_TOKEN
#define APP_SECRET_HA_TOKEN ""
#endif

namespace cfg {

namespace defaults {

// Valeurs de repli uniquement, utilisées tant que la NVS n'est pas
// provisionnée (page Configuration de l'écran, ou tools/provision_nvs.ps1).
// Les vraies valeurs ne sont JAMAIS versionnées : elles viennent du fichier
// optionnel `main/app_secrets.h` (ignoré par git, modèle dans
// `main/app_secrets.example.h`). Sans lui, tout est vide et l'appareil démarre
// directement sur une configuration à saisir à l'écran.
inline constexpr const char *kWifiSsid = APP_SECRET_WIFI_SSID;
inline constexpr const char *kWifiPass = APP_SECRET_WIFI_PASS;

// go2rtc, Home Assistant et le broker MQTT tournent sur la même machine : un
// seul hôte à saisir sur la page Configuration.
inline constexpr const char *kHost = APP_SECRET_HOST;

inline constexpr const char *kMqttUser = APP_SECRET_MQTT_USER;
inline constexpr const char *kMqttPass = APP_SECRET_MQTT_PASS;

// Jeton longue durée Home Assistant (API REST météo / agenda).
inline constexpr const char *kHaToken = APP_SECRET_HA_TOKEN;

} // namespace defaults

// --- Ports des services (mêmes valeurs sur toutes les installations) --------
inline constexpr int kGo2rtcPort = 1984;
inline constexpr int kHaPort = 8123;
inline constexpr int kMqttPort = 1883;

// --- Flux go2rtc -----------------------------------------------------------
// WebRTC ne sert plus qu'à l'audio : flux audio-only pour que go2rtc n'envoie
// plus le H.264 (économie WiFi + décharge transcode). Dans go2rtc.yaml :
//   vto_audio_only:
//     - "ffmpeg:vto_main_stream#audio=pcma"
inline constexpr const char *kGo2rtcAudioSrc = "vto_audio_only";
inline constexpr const char *kGo2rtcMicSrc = "vto_audio_input_only";
// Flux MJPEG, 1280x720 @ 15 fps, livré en 800x1280 : transposé (le PPA
// ne tourne pas et le panneau est physiquement portrait) puis padé de 80 px en
// largeur pour que la source fasse exactement kLcdHRes. C'est cette égalité de
// stride qui autorise le décodeur JPEG matériel à écrire directement dans le
// framebuffer DSI (cf. Bsp::direct_video_target) — sans elle, le PPA revient
// dans le chemin chaud à ~40 ms/frame. Les 80 px de padding sont recouverts par
// le bandeau de contrôles.
inline constexpr const char *kGo2rtcMjpeg2Src = "vto_mjpeg2";

// --- Home Assistant --------------------------------------------------------
// À adapter : identifiant de l'entité `weather.*` de votre installation
// (Paramètres > Entités dans Home Assistant).
inline constexpr const char *kHaWeatherEntity = "weather.forecast_home";
// Agenda affiché sur la page Météo + la page Agenda. Intégration Google
// Calendar native de Home Assistant (entité calendar.*), pas d'OAuth Google
// sur la cible.
inline constexpr const char *kHaCalendarEntity = "calendar.home";
// Chemin du firmware servi par HA, proposé comme payload du bouton OTA.
inline constexpr const char *kOtaFirmwarePath = "/local/fw.bin";

// Fuseau POSIX (Europe/Paris, bascule été/hiver par la libc) et serveur SNTP.
inline constexpr const char *kTimezone = "CET-1CEST,M3.5.0,M10.5.0/3";
inline constexpr const char *kSntpServer = "pool.ntp.org";

// --- Topics MQTT -----------------------------------------------------------
namespace topic {
// Déclenche l'ouverture de l'écran caméra quand ce topic reçoit exactement
// cette charge utile.
inline constexpr const char *kRing = "visiophone/ring";
inline constexpr const char *kRingPayload = "ON";

inline constexpr const char *kOtaCmd = "visio/ota/cmd";       // payload = URL
inline constexpr const char *kOtaStatus = "visio/ota/status"; // progression
inline constexpr const char *kLog = "visio/log";              // logs (batch)
inline constexpr const char *kLogEnable = "visio/log/enable"; // "1" / "0"
inline constexpr const char *kFwVersion = "visio/fw/version"; // retained

// Testament MQTT : « offline » publié par le BROKER si l'appareil tombe sans
// déconnexion propre. Toutes les entités le déclarent en `avty_t` — sans lui,
// une carte Home Assistant affiche indéfiniment la dernière valeur connue d'un
// appareil débranché depuis trois jours.
inline constexpr const char *kStatus = "visio/status";
inline constexpr const char *kStatusOnline = "online";
inline constexpr const char *kStatusOffline = "offline";

// Mise à jour depuis la carte microSD (payload = chemin du .bin).
inline constexpr const char *kOtaSdCmd = "visio/ota/sd";
inline constexpr const char *kOtaSdDefaultPath = "/sdcard/fw.bin";
} // namespace topic

// --- Journal sur carte microSD ---------------------------------------------
namespace sd {
inline constexpr const char *kMountPoint = "/sdcard";
inline constexpr const char *kLogDir = "/sdcard/log";
// Vignette de diagnostic de la caméra de présence (presence::request_snapshot).
inline constexpr const char *kPresenceSnapshot = "/sdcard/presence.pgm";
// Ring PSRAM : doit absorber la rafale de logs du boot, produite avant que
// l'heure SNTP soit connue et donc avant l'ouverture du fichier daté.
inline constexpr int kRingBytes = 32768;
inline constexpr int kLineMax = 256; // aligné sur dbg_log::kLineMax
inline constexpr int kFlushPeriodMs = 2000;
inline constexpr int kFlushThresholdBytes = 8192;
// Écriture par blocs bornés : le contrôleur SDMMC est partagé avec la liaison
// Wi-Fi (slot 1), un gros transfert d'un coup y ajouterait de la gigue.
inline constexpr int kWriteChunkBytes = 4096;
inline constexpr int kRetentionDays = 14;
inline constexpr int kMinFreeMb = 8;
// Au-delà, on renonce à attendre SNTP et on écrit dans nodate.txt.
inline constexpr int kTimeWaitMs = 60000;
inline constexpr int kViewPageBytes = 4096;

// --- Historique d'appels ---------------------------------------------------
// Une capture JPEG par appel manqué + un journal texte d'une ligne par appel.
inline constexpr const char *kCallDir = "/sdcard/calls";
inline constexpr const char *kCallLog = "/sdcard/calls.log";
// Rotation FIFO : le numéro de slot reboucle, la carte ne se remplit jamais.
inline constexpr int kCallHistoryLimit = 100;
// Le journal, lui, ne reboucle pas tout seul : il est rogné par la tête dès
// qu'il dépasse cette taille, sinon il grossit indéfiniment (une ligne par
// coup de sonnette, pour toute la vie de l'appareil).
inline constexpr int kCallLogMaxBytes = 32768;
// Queue relue pour peupler la page Appels. Doit rester < kCallLogMaxBytes.
inline constexpr int kCallLogTailBytes = 16384;
// Nombre d'appels affichés sur la page Appels (les plus récents).
inline constexpr int kCallListMax = 60;
// Sans réponse au bout de ce délai, l'appel est classé manqué et l'écran
// caméra se referme tout seul (bascule vers la page météo).
inline constexpr int kCallNoAnswerMs = 30000;
} // namespace sd

} // namespace cfg
