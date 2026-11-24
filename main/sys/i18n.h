#pragma once

// Langue de l'interface (écran, page web du serveur de fichiers, noms des
// entités Home Assistant). Deux langues : français (historique, défaut) et
// anglais.
//
// Les textes vivent dans un fichier par langue (sys/lang/fr.h, sys/lang/en.h),
// tables `constexpr` compilées dans le firmware : en flash (.rodata), aucun
// tas, aucune lecture de fichier ni de NVS par texte. Des fichiers lus à
// l'exécution (JSON sur la carte SD) ajouteraient de l'IO SD — que le projet
// bannit des chemins chauds — et perdraient les vérifications ci-dessous.
//
// i18n.cpp vérifie à la COMPILATION que chaque langue définit chaque clé une
// seule fois, et que les textes qui servent de format printf ont les mêmes
// conversions dans le même ordre dans toutes les langues : un %s face à un %d
// serait un crash au premier affichage, pas une coquille.
//
// Pas de glyphe LVGL (LV_SYMBOL_*) dans les tables : une icône ne se traduit
// pas, et les tables restent compilables sur host sans LVGL. L'appelant compose
// ("%s %s", icône, texte).
//
// Figée au boot, comme le thème : écrans LVGL et catalogue d'entités HA sont
// construits une seule fois. prefs::set_language() ne fait que persister ;
// relire la préférence à chaque t() ferait basculer l'interface à moitié. D'où
// un instantané pris par init(), seul lu ensuite.
//
// Logique pure, sans ESP-IDF : testée sur host (test/).
#include <cstdint>

namespace i18n {

enum class Lang : uint8_t { kFr = 0, kEn = 1 };
inline constexpr Lang kLastLang = Lang::kEn;

// Clés des textes. Groupées par écran ; l'ordre n'a pas d'importance (les
// tables sont indexées par clé, pas par position).
enum class Str : uint16_t {
  // --- Commun
  kClose,
  kBack,
  kLoading,
  kCancel,
  // --- Accueil
  kHomeGate,
  kHomeCamera,
  kHomeWeather,
  kHomeCalendar,
  kHomeCalls,
  kHomeSettings,
  kHomeScreenOff,
  // --- Portail
  kGateSetUpInHa,
  kGateUnavailable,
  kGateUnavailableShort,
  kGateFailed,
  kGateFailedShort,
  kGateOpen,
  kGateOpenShort,
  kGateClose,
  kGateStop,
  // --- Caméra et session vidéo
  kCamSignalingRetryN,
  kCamSignalingBusy,
  kCamSignalingRetrying,
  kCamPreviousSessionStuck,
  kCamWebrtcOpenError,
  kCamTaskAllocError,
  kCamVideoStartError,
  kCamStreamLost,
  kCamMicOff,
  kCamLive,
  kCamLiveMinutesFmt,
  kCamHangUp,
  kCamAnswer,
  kCamConnecting,
  // --- Sonnerie et fichiers WAV
  kRingBuiltIn,
  kRingFileMissingFmt,
  kRingFileUnreadableFmt,
  kRingFileRejectedFmt,
  kRingFileTooBigFmt,
  kRingFileCutFmt,
  kWavTruncated,
  kWavNotRiff,
  kWavNotWave,
  kWavNoFmt,
  kWavNotPcm16,
  kWavBadRate,
  kWavNotMono,
  kWavNoData,
  kWavUnknown,
  // --- Météo
  kWeatherOngoingFmt,
  kWeatherAllDayFmt,
  kWeatherMoreFmt,
  kWeatherMissedManyFmt,
  kWeatherMissedOneFmt,
  kWeatherMetaFmt,
  kWeatherError,
  kWeatherToday,
  kWeatherView,
  kWeatherNext,
  kWeatherSunny,
  kWeatherClearNight,
  kWeatherPartlyCloudy,
  kWeatherCloudy,
  kWeatherFog,
  kWeatherRainy,
  kWeatherPouring,
  kWeatherSnowy,
  kWeatherSnowyRainy,
  kWeatherHail,
  kWeatherLightning,
  kWeatherLightningRainy,
  kWeatherWindy,
  kWeatherExceptional,
  // --- Agenda
  kAgendaUntitled,
  kAgendaMonth,
  kAgendaWeek,
  // --- Appels
  kCallsSnapshotUnavailable,
  kCallsMissed,
  kCallsAnswered,
  kCallsViewPhoto,
  kCallsNoPhoto,
  kCallsNone,
  kCallsLogUnreadable,
  // --- Configuration
  kSettingsThemeOptions,
  kSettingsEchoOptions,
  kSettingsRingInternal,
  kSettingsRestart,
  kSettingsLater,
  kSettingsRequired,
  kSettingsNvsFailed,
  kSettingsSaved,
  kSettingsSavedTitle,
  kSettingsSavedBody,
  kSettingsThemeTitle,
  kSettingsThemeBody,
  kSettingsLangTitle,
  kSettingsLangBody,
  kSettingsUnknownAddress,
  kSettingsRingStop,
  kSettingsRingTest,
  kSettingsSave,
  kSettingsImmediateHint,
  kSettingsBrightness,
  kSettingsDnd,
  kSettingsSdLog,
  kSettingsRingVolume,
  kSettingsRingFile,
  kSettingsEcho,
  kSettingsTheme,
  kSettingsLanguage,
  kSettingsFileServer,
  kSettingsWifiPassword,
  kSettingsServer,
  kSettingsFromNvs,
  kSettingsDefaults,
  // --- Micro / Debug
  kMicStopTune,
  kMicTune,
  kMicStopRing,
  kMicRing,
  kMicConnecting,
  kMicConnected,
  kMicFailed,
  kMicSdpYes,
  kMicSdpNo,
  kMicGainFmt,
  kMicTab,
  kMicSdTab,
  kMicGainZero,
  kMicLevel,
  // --- Journal SD et firmware
  kSdFolderUnreadableFmt,
  kSdCannotOpen,
  kSdFormatBusy,
  kSdFormatOk,
  kSdFormatErrorFmt,
  kSdUsbEnterErrorFmt,
  kSdUsbLeaveErrorFmt,
  kSdRootUnreadableFmt,
  kSdNoFirmware,
  kSdNotRemounted,
  kSdCardMissingFmt,
  kSdCardErrorFmt,
  kSdMountedLostFmt,
  kSdMountedFmt,
  kSdReading,
  kSdFormatting,
  kSdUsbModeTitle,
  kSdUsbConfirm,
  kSdUsbExpose,
  kSdFormatTitle,
  kSdFormatConfirm,
  kSdErase,
  kSdFwStarted,
  kSdFwTitle,
  kSdFwCurrentFmt,
  kUnitKb,
  kSdFwInvalid,
  kSdUsbActive,
  kSdNoPsram,
  kSdCardPending,
  kSdPrev,
  kSdNext,
  kSdRefresh,
  kSdFormat,
  kSdUsbDrive,
  // --- Mode clé USB
  kUsbHostConnected,
  kUsbHostNone,
  kUsbInfo,
  kUsbDoorbellIgnored,
  kUsbFinish,
  // --- Home Assistant
  kHaUpdateTitle,
  kHaEchoOff,
  kHaEchoLowCost,
  kHaEchoHighPerf,
  kHaEntScreen,
  kHaEntDnd,
  kHaEntPage,
  kHaEntEchoCancel,
  kHaEntLanguage,
  kHaEntRingVolume,
  kHaEntSpeakerVolume,
  kHaEntMicGain,
  kHaEntCamera,
  kHaEntShowStats,
  kHaEntSdLog,
  kHaEntFileServer,
  kHaEntBetaChannel,
  kHaEntRing,
  kHaEntAckMissed,
  kHaEntReboot,
  kHaEntOtaSd,
  kHaEntPresenceSnapshot,
  kHaEntPresenceCaptures,
  kHaEntScreenshot,
  kHaEntScreenshotsAll,
  kHaEntRssi,
  kHaEntIp,
  kHaEntUptime,
  kHaEntPsram,
  kHaEntSramFree,
  kHaEntCpu0,
  kHaEntCpu1,
  kHaEntSdFree,
  kHaEntMissedCalls,
  kHaEntFwVersion,
  kHaEntGateEntity,
  kHaEntWeatherEntity,
  kHaEntCalendarEntity,
  kHaEntFirmware,
  kHaEntOtaStatus,
  kHaEntInCall,
  kHaEntPresence,
  kCount
};

// Une fois, AVANT l'UI et ha_api::init(). Valeur hors bornes : français.
void init(Lang lang);

[[nodiscard]] Lang lang();

// Texte de la clé dans la langue figée au boot. Durée de vie statique (flash) :
// utilisable avec lv_dropdown_set_options_static ou gardé en pointeur. Jamais
// nullptr ; "" pour une clé hors bornes.
[[nodiscard]] const char *t(Str id);

// Noms calendaires. `wday` au format struct tm (0 = dimanche), `mon` 0-11.
// Hors bornes : "" (jamais nullptr).
[[nodiscard]] const char *weekday_long(int wday);   // "Lundi" / "Monday"
[[nodiscard]] const char *weekday_short(int wday);  // "lun." / "Mon"
[[nodiscard]] const char *weekday_abbr(int wday);   // "Lun" / "Mon"
[[nodiscard]] const char *month_long(int mon);      // "janvier" / "January"
[[nodiscard]] const char *month_short(int mon);     // "janv." / "Jan"

// Ordre des dates : « lundi 23 novembre » ou « Monday, November 23 ». Ce
// n'est pas qu'une affaire de mots, printf ne sait pas permuter ses
// arguments : l'appelant choisit son format selon ce drapeau.
[[nodiscard]] bool month_before_day();

// Nom de la langue dans sa propre langue, sans accent : il sert aussi
// d'option du select Home Assistant (même convention ASCII que les autres
// libellés de app/ha_api). Indépendant de la langue en vigueur.
[[nodiscard]] const char *language_name(Lang lang);

} // namespace i18n
