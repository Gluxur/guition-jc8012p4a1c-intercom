#pragma once

// Icônes FontAwesome 5 absentes du jeu LV_SYMBOL_* de LVGL, en UTF-8. Elles ne
// sont rendues QUE par les polices qui les embarquent : montserrat_fr_24/36
// (liste `-r` dans leur en-tête) et icons_fa_56 (tuiles de l'accueil, icônes
// seules). Ajouter une icône ici impose de régénérer ces polices.
namespace ui::icon {

inline constexpr const char *kVideo = "\xEF\x80\xBD";     // U+F03D video
inline constexpr const char *kCalendar = "\xEF\x81\xB3";  // U+F073 calendar-alt
inline constexpr const char *kMic = "\xEF\x84\xB0";       // U+F130 microphone
inline constexpr const char *kMicOff = "\xEF\x84\xB1";    // U+F131 microphone-slash
inline constexpr const char *kSun = "\xEF\x86\x85";       // U+F185 sun
inline constexpr const char *kHangUp = "\xEF\x8F\x9D";    // U+F3DD phone-slash
inline constexpr const char *kCloudSun = "\xEF\x9B\x84";  // U+F6C4 cloud-sun
inline constexpr const char *kGate = "\xEF\x94\xAB";      // U+F52B door-open

} // namespace ui::icon
