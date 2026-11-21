#pragma once

#include <cstddef>

// Configuration effective au runtime : valeurs de la NVS, avec repli sur les
// défauts compilés de app_config.h. Écrite par la page Configuration de
// l'écran tactile (ui_settings), lue par tous les modules réseau.
//
// Pourquoi la NVS et pas seulement app_config.h : changer d'IP ou de box WiFi
// ne doit pas exiger un rebuild + un flash. Et les secrets saisis à
// l'écran ne passent jamais par git.
namespace settings {

// Bornes des champs. Fixes (pas d'allocation sur un chemin de configuration) et
// alignées sur les limites réelles des protocoles.
inline constexpr size_t kSsidMax = 33;  // 32 octets + NUL (IEEE 802.11)
inline constexpr size_t kPassMax = 65;  // 64 caractères + NUL (WPA2 PSK)
inline constexpr size_t kHostMax = 64;  // IP ou nom d'hôte

struct Config {
  char wifi_ssid[kSsidMax];
  char wifi_pass[kPassMax];
  char host[kHostMax]; // go2rtc + Home Assistant (OTA image)
};

// Charge la NVS (chaque champ absent retombe sur son défaut compilé) et
// formate les URLs dérivées. À appeler APRÈS nvs_flash_init et AVANT le WiFi
// et l'init des sessions.
void init();

// Configuration effective. Référence stable pour toute la vie du programme.
[[nodiscard]] const Config &get();

// Écrit les 3 champs en NVS. N'applique RIEN à chaud : le WiFi et les URLs ne
// se reconfigurent proprement qu'au boot, et le faire pendant un flux actif est
// la voie des bugs difficiles. L'appelant redémarre.
bool save(const Config &cfg);

// true si la NVS contenait au moins un champ (carte provisionnée). Permet à
// l'UI de signaler qu'on tourne encore sur les défauts compilés.
[[nodiscard]] bool provisioned();

// URLs dérivées de `host`, formatées une seule fois dans init(). Pointeurs
// valides pour toute la vie du programme (buffers statiques).
[[nodiscard]] const char *go2rtc_audio_url();
[[nodiscard]] const char *go2rtc_mic_url();
[[nodiscard]] const char *go2rtc_mjpeg2_url(); // flux vidéo de la page caméra
[[nodiscard]] const char *ota_firmware_url(); // proposé au bouton OTA de HA

} // namespace settings
