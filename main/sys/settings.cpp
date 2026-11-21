#include "sys/settings.h"

#include "app_config.h"
#include "esp_log.h"
#include "nvs.h"

#include <cstdio>
#include <cstring>

namespace settings {
namespace {

constexpr const char *TAG = "settings";

// Namespace NVS distinct de "audio_cfg" (volume/gain du BSP) : effacer la
// configuration réseau ne doit pas réinitialiser les réglages audio.
constexpr const char *kNvsNamespace = "net_cfg";

constexpr const char *kKeySsid = "ssid";
constexpr const char *kKeyPass = "pass";
constexpr const char *kKeyHost = "host";
// Keys "mq_user", "mq_pass" and "ha_token" from MQTT-era firmware may still
// sit in NVS: never read again, harmless, wiped by an NVS erase.

Config s_cfg = {};
bool s_provisioned = false;

// Les URLs sont dérivées de `host`, donc inconnues à la compilation. Buffers
// statiques : formatés une fois dans init(), jamais réalloués, sûrs à exposer
// comme const char* pour toute la vie du programme.
char s_url_audio[160];
char s_url_mic[160];
char s_url_mjpeg2[160];
char s_url_ota[160];

void copy_default(char *dst, size_t cap, const char *src) {
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

// Lit une chaîne NVS par-dessus la valeur déjà en place (le défaut compilé).
// Laisse `dst` intact si la clé est absente : chaque champ retombe donc
// individuellement sur son défaut, ce qui évite qu'une config partielle
// (ex. seul l'hôte provisionné) rende la carte injoignable.
bool load_string(nvs_handle_t h, const char *key, char *dst, size_t cap) {
  size_t len = cap;
  if (nvs_get_str(h, key, dst, &len) != ESP_OK) {
    return false;
  }
  dst[cap - 1] = '\0';
  return true;
}

void build_urls() {
  const char *host = s_cfg.host;
  snprintf(s_url_audio, sizeof(s_url_audio),
           "http://%s:%d/api/webrtc?src=%s", host, cfg::kGo2rtcPort,
           cfg::kGo2rtcAudioSrc);
  snprintf(s_url_mic, sizeof(s_url_mic), "http://%s:%d/api/webrtc?src=%s", host,
           cfg::kGo2rtcPort, cfg::kGo2rtcMicSrc);
  snprintf(s_url_mjpeg2, sizeof(s_url_mjpeg2),
           "http://%s:%d/api/stream.mjpeg?src=%s", host, cfg::kGo2rtcPort,
           cfg::kGo2rtcMjpeg2Src);
  // https et non http : Home Assistant ne sert QUE du TLS sur kHaPort (constaté
  // le 29/07/2026 — http refuse la connexion, https répond 200).
  snprintf(s_url_ota, sizeof(s_url_ota), "https://%s:%d%s", host, cfg::kHaPort,
           cfg::kOtaFirmwarePath);
}

} // namespace

void init() {
  copy_default(s_cfg.wifi_ssid, kSsidMax, cfg::defaults::kWifiSsid);
  copy_default(s_cfg.wifi_pass, kPassMax, cfg::defaults::kWifiPass);
  copy_default(s_cfg.host, kHostMax, cfg::defaults::kHost);

  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) == ESP_OK) {
    int found = 0;
    found += load_string(h, kKeySsid, s_cfg.wifi_ssid, kSsidMax) ? 1 : 0;
    found += load_string(h, kKeyPass, s_cfg.wifi_pass, kPassMax) ? 1 : 0;
    found += load_string(h, kKeyHost, s_cfg.host, kHostMax) ? 1 : 0;
    nvs_close(h);
    s_provisioned = found > 0;
    ESP_LOGI(TAG, "NVS: %d/3 champs chargés", found);
  } else {
    // Namespace pas encore créé : premier boot, ou config jamais enregistrée.
    ESP_LOGI(TAG, "NVS absente, défauts compilés");
  }

  build_urls();
  // Jamais de log du mot de passe : la sortie UART part aussi vers le ring de
  // la page Debug et vers les clients de l'API qui suivent les logs.
  ESP_LOGI(TAG, "config: ssid=%s host=%s (provisionnée=%s)", s_cfg.wifi_ssid,
           s_cfg.host, s_provisioned ? "oui" : "non");
}

const Config &get() { return s_cfg; }

bool provisioned() { return s_provisioned; }

bool save(const Config &cfg) {
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
    return false;
  }

  // Première erreur d'écriture conservée : on tente quand même les autres clés
  // pour ne pas laisser une config à moitié écrite si une seule échoue.
  esp_err_t first_err = ESP_OK;
  const struct {
    const char *key;
    const char *value;
  } fields[] = {
      {kKeySsid, cfg.wifi_ssid},
      {kKeyPass, cfg.wifi_pass},
      {kKeyHost, cfg.host},
  };
  for (const auto &f : fields) {
    esp_err_t e = nvs_set_str(h, f.key, f.value);
    if (e != ESP_OK && first_err == ESP_OK) {
      first_err = e;
      ESP_LOGE(TAG, "nvs_set_str(%s): %s", f.key, esp_err_to_name(e));
    }
  }

  esp_err_t commit_err = nvs_commit(h);
  nvs_close(h);
  if (first_err != ESP_OK) {
    return false;
  }
  if (commit_err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_commit: %s", esp_err_to_name(commit_err));
    return false;
  }

  // La copie en mémoire n'est PAS mise à jour et les URLs ne sont PAS
  // reconstruites : les modules réseau ont déjà capturé leurs pointeurs d'URL
  // au boot. Changer la config sous leurs pieds donnerait un état incohérent —
  // c'est le redémarrage qui applique.
  ESP_LOGI(TAG, "configuration enregistrée (effective au redémarrage)");
  return true;
}

const char *go2rtc_audio_url() { return s_url_audio; }
const char *go2rtc_mic_url() { return s_url_mic; }
const char *go2rtc_mjpeg2_url() { return s_url_mjpeg2; }
const char *ota_firmware_url() { return s_url_ota; }

} // namespace settings
