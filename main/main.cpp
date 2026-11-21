#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "bsp.h"

#include "app_config.h"
#include "app/audio_pipeline.h"
#include "app/call_history.h"
#include "app/camera_session.h"
#include "app/ha_api.h"
#include "app/mic_session.h"
#include "app/presence.h"
#include "app/video_mjpeg.h"
#include "media/g711.h"
#include "media/ringtone.h"
#include "media/ringtone_sd.h"
#include "net/file_server.h"
#include "net/esphome_api.h"
#include "net/gcal.h"
#include "net/weather.h"
#include "net/wifi_net.h"
#include "sys/dbg_log.h"
#include "sys/net_wdt.h"
#include "sys/ota_ctrl.h"
#include "sys/prefs.h"
#include "sys/sd_log.h"
#include "sys/settings.h"
#include "sys/usb_msc.h"
#include "ui/ui.h"
#include "ui/ui_camera_v2.h"

// ---------------------------------------------------------------------------
// Stub `esp_log` : SUPPRIMÉ au passage à ESP-IDF 6.0 — ne pas le réintroduire.
//
// Historique : `libpeer_default.a` (précompilé, C) appelle un symbole `esp_log`
// qu'ESP-IDF 5.x ne définissait pas. Ce fichier fournissait donc, dans un bloc
// extern "C" (sinon le nom serait mangé et le lien échouerait), une définition
// `esp_log(esp_log_level_t level, const char *tag, const char *format, ...)`
// relayant vers `esp_log_writev`. L'ABI avait été vérifiée par désassemblage des
// sites d'appel : a0 = niveau entier 1/2, a1 = tag, a2 = format. Prendre
// "format" en premier paramètre faisait lire le niveau comme un pointeur de
// chaîne → `Load access fault` dans vfprintf au premier log ICE.
//
// ESP-IDF 6.0 définit lui-même le symbole (`esp_log.h`) :
//     void esp_log(esp_log_config_t config, const char *tag,
//                  const char *format, ...);
// Garder notre version donnait `conflicting declaration of C function`.
// `esp_log_config_t` est une union de 4 octets — donc passée dans a0 comme un
// entier — dont les 3 bits de poids faible sont le niveau. Le `1`/`2` que
// libpeer place dans a0 y est donc lu comme ESP_LOG_ERROR / ESP_LOG_WARN, avec
// tous les drapeaux à 0 : le message sort sans préfixe couleur/horodatage/tag,
// mais il sort, et il passe toujours par le hook `esp_log_set_vprintf` de
// `sys/dbg_log.cpp`. L'ABI reste donc compatible sans stub.
// ---------------------------------------------------------------------------

// `static` at file scope (not inside a class) means internal linkage: this
// variable is private to main.cpp, invisible to other .cpp files even if
// they declare an `extern` with the same name. That's a different meaning
// from a C# `static` field, which is just "belongs to the type, not an
// instance" and is still visible wherever the type is. Les tâches, handles et
// buffers qui remplissaient jadis ce fichier vivent maintenant dans les
// modules de app/ ; main.cpp ne garde que le câblage.
static const char *TAG = "WebRTC_go2rtc";

// Les identifiants réseau et les URLs go2rtc ne sont plus des #define ici : les
// défauts compilés vivent dans app_config.h (`cfg::defaults`) et la valeur
// EFFECTIVE vient de la NVS via `settings::`, éditable sur la page
// Configuration de l'écran tactile. Les modules reçoivent leurs URLs par
// paramètre — aucun n'interroge la configuration globale lui-même.

static void lvgl_task(void *arg) {
  (void)arg;
  ESP_LOGI("LVGL_TASK", "Création des écrans d'interface...");

  // 1. Création des écrans (Protégée par le Lock LVGL)
  {
    bsp::Bsp::LvglLock lvgl_lock;

    ui::init_error_ui();
    ui::Callbacks ui_cbs = {};
    ui_cbs.stop_camera = camera_session::stop;
    ui_cbs.start_camera_v2 = camera_session::start_hd;
    ui_cbs.start_mic = mic_session::start;
    ui_cbs.stop_mic = mic_session::stop;
    // live() et non active() : pendant la sonnerie côté rue le peer est ouvert
    // mais coupé, et le bouton Répondre doit encore proposer de décrocher.
    ui_cbs.mic_active = mic_session::live;
    ui_cbs.mic_level = mic_session::level;
    ui_cbs.mic_conn_state = mic_session::conn_state;
    ui_cbs.log_drain = dbg_log::drain;
    // `[]` = empty capture list: this lambda touches no local state, only
    // file-scope/namespace state, so it needs no closure and implicitly
    // converts to a plain function pointer — that's why it can sit in a struct
    // field (ui::Callbacks) whose declared type is a C function-pointer type,
    // not std::function. A lambda WITH captures (e.g. `[x]`) could not do
    // this: it would need std::function or an equivalent type-erasing
    // wrapper, since captured state can't be represented by a bare function
    // pointer. Roughly analogous to a C# static lambda assigned to a
    // delegate, except C# delegates always carry a hidden target pointer even
    // when unused.
    ui_cbs.toggle_mic_tone = [] {
      if (ringtone::mic_tone_active()) {
        ringtone::mic_tone_stop();
      } else {
        ringtone::mic_tone_start();
      }
    };
    ui_cbs.mic_tone_active = ringtone::mic_tone_active;
    // Écoute locale de la sonnerie. audio_pipeline::wake() est indispensable :
    // hors streaming aud_render dort sur portMAX_DELAY et ne verrait jamais
    // l'armement — même raison que dans ring_cb.
    ui_cbs.toggle_ringtone = [] {
      if (ringtone::is_ringing()) {
        ringtone::stop();
      } else {
        ringtone::start();
        audio_pipeline::wake();
      }
    };
    ui_cbs.ringtone_active = ringtone::is_ringing;
    ui::create_screens(ui_cbs);
  }

  ESP_LOGI("LVGL_TASK", "Écrans créés, démarrage de la boucle principale.");

  // 2. Boucle de rafraîchissement LVGL
  while (true) {
    uint32_t next_ms;
    {
      bsp::Bsp::LvglLock lvgl_lock;
      next_ms = lv_timer_handler();
    }
    // Dormir jusqu'au prochain timer LVGL, pas 10 ms fixes : le délai fixe
    // s'ajoutait à LV_DEF_REFR_PERIOD (0-10 ms de gigue par rafraîchissement,
    // Lottie compris). Plancher 1 ms pour céder le CPU, plafond 33 ms pour que
    // le tactile reste échantillonné même si aucun timer n'est armé.
    if (next_ms < 1) {
      next_ms = 1;
    } else if (next_ms > 33) {
      next_ms = 33;
    }
    vTaskDelay(pdMS_TO_TICKS(next_ms));
  }
}

// Doorbell: called from the ESPHome API task when Home Assistant presses the
// "Sonner" button (its doorbell automation). Garde camera_session::active() :
// ignore si la caméra est déjà active (pas de double démarrage WebRTC /
// rechargement d'écran). open_screen prend le LvglLock en interne et diffère
// le WebRTC dans sa propre tâche -> sûr depuis ce contexte hors LVGL.
static void ring_cb(void) {
  if (ota_ctrl::in_progress()) {
    // Écriture de partition en cours (~30 s) : ouvrir WebRTC + MJPEG en même
    // temps cumulerait le DTLS, le drain vidéo et l'écriture flash. Les gardes
    // OTA refusent déjà le cas inverse (pas d'OTA pendant un appel). La
    // sonnerie intérieure prévient quand même qu'on a sonné.
    ESP_LOGW(TAG, "sonnette pendant une OTA : sonnerie seule");
    if (!prefs::dnd()) {
      ringtone::start();
      audio_pipeline::wake();
    }
    return;
  }
  if (usb_msc::status() == usb_msc::Status::kActive) {
    // La carte appartient au PC : ouvrir l'écran caméra remplacerait l'écran
    // du mode par de la vidéo, et le journal SD est de toute façon suspendu.
    ESP_LOGI(TAG, "sonnette ignorée : mode clé USB actif");
    return;
  }
  if (camera_session::active()) {
    // Stream vidéo déjà ouvert. Si le micro n'est pas activé, personne n'a
    // décroché -> rejoue quand même la sonnerie (nouveau coup de sonnette).
    // Pas de re-démarrage WebRTC : l'écran caméra est déjà en place.
    // live() et non active() : le backchannel est ouvert (coupé) dès la
    // sonnerie côté rue, seul un vrai décroché doit faire ignorer le trigger.
    if (mic_session::live()) {
      ESP_LOGI(TAG, "sonnette ignorée : micro déjà actif");
      return;
    }
    ESP_LOGI(TAG, "sonnette : stream actif, micro coupé -> re-sonnerie");
    // Nouveau coup de sonnette = nouvel appel à tracer, et compte à rebours de
    // non-réponse réarmé. Indépendant du « ne pas déranger », qui ne coupe que
    // le son, pas le suivi.
    call_history::on_call_start();
    if (!prefs::dnd()) {
      ringtone::start();
    }
    audio_pipeline::wake();
    camera_session::request_ringback();
    return;
  }
  ESP_LOGI(TAG, "sonnette -> ouverture écran caméra");
  call_history::on_call_start();
  ui_camera_v2::open_screen();
  // Sonnerie côté rue, hors « ne pas déranger » : celui-ci ne fait taire que
  // l'intérieur, le visiteur doit toujours savoir que ça sonne.
  camera_session::request_ringback();
  // Ne pas déranger coupe la sonnerie SEULE : l'écran s'ouvre quand même, on
  // veut voir le visiteur en silence.
  if (!prefs::dnd()) {
    ringtone::start(); // ding-dong jusqu'au clic micro (ou 30 s)
  }
  // La tâche de lecture dort sur portMAX_DELAY hors streaming : la réveiller
  // pour que la sonnerie parte tout de suite, sans attendre le premier slot
  // audio WebRTC.
  audio_pipeline::wake();
}

// OTA refusé pendant tout streaming (SRAM interne + SDIO partagé esp_hosted).
static bool ota_is_busy(void) {
  return camera_session::active() || mic_session::active() ||
         usb_msc::status() == usb_msc::Status::kActive;
}

// Sonde du serveur de fichiers : toute IO sur la carte est refusée pendant un
// appel (gigue sur le drain TCP du MJPEG, facteur limitant du débit vidéo),
// pendant un OTA (écriture de partition) et en mode clé USB (la carte
// appartient au PC, elle est démontée côté ESP).
static bool file_server_sd_busy(void) {
  return camera_session::active() || ota_ctrl::in_progress() ||
         usb_msc::status() == usb_msc::Status::kActive;
}

// Gel du watchdog réseau : un reboot en plein appel coupe la communication, et
// un reboot pendant l'écriture d'une partition OTA peut briquer la carte.
static bool net_wdt_freeze_cb(void) {
  // Mode clé USB inclus : un redémarrage automatique pendant que le PC écrit
  // sur la carte serait le pire moment possible.
  // Carte jamais configurée (SSID ou hôte vide) : redémarrer ne ferait jamais
  // monter le lien, et couperait l'utilisateur en pleine saisie sur la page
  // Configuration toutes les 5 min.
  return camera_session::active() || mic_session::active() ||
         ota_ctrl::in_progress() ||
         usb_msc::status() == usb_msc::Status::kActive ||
         settings::get().wifi_ssid[0] == '\0' || settings::get().host[0] == '\0';
}

// OTA progress -> the "Statut OTA" entity. The text itself is read back by
// the entity's getter (ota_ctrl::status), so only a push is needed here.
static void ota_report_cb(const char *msg) {
  (void)msg;
  ha_api::publish_now("ota_status");
}

// Marque l'image en cours "valide" (annule le rollback) UNIQUEMENT si elle est
// en attente de vérification (premier boot post-OTA) ET que la connectivité est
// OK. Sinon un crash avant ce point => le bootloader revient à l'ancien slot.
// Appelée en boucle par net_wdt (1x/5 s) tant que l'image reste en attente :
// le verrou évite de relire l'otadata en flash indéfiniment une fois la
// question réglée. Idempotente de toute façon — le verrou n'est qu'une économie.
static void ota_confirm_valid_if_pending(void) {
  static bool settled = false;
  if (settled) {
    return;
  }
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) != ESP_OK)
    return;
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    settled = true;
    return; // rien à faire (image déjà validée, ou rollback désactivé)
  }
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err == ESP_OK) {
    settled = true;
    ESP_LOGI(TAG, "OTA: image marquée valide (rollback annulé)");
  } else {
    // Pas de verrou : on retentera au prochain tick de net_wdt.
    ESP_LOGE(TAG, "OTA: mark_app_valid -> %s", esp_err_to_name(err));
  }
}


extern "C" void app_main(void) {
  // Pas d'ESP_ERROR_CHECK dans ce fichier : les assertions runtime sont
  // désactivées (sdkconfig PERF), la macro masquerait donc l'erreur au lieu de
  // l'abandonner proprement (règle CLAUDE.md).
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    esp_err_t erase_err = nvs_flash_erase();
    if (erase_err != ESP_OK) {
      ESP_LOGE(TAG, "nvs_flash_erase: %s", esp_err_to_name(erase_err));
    }
    ret = nvs_flash_init();
  }
  if (ret != ESP_OK) {
    // NVS absente : volume/gain repartent aux défauts, le reste fonctionne.
    ESP_LOGE(TAG, "nvs_flash_init: %s (préférences audio non persistées)",
             esp_err_to_name(ret));
  }

  // Capture des logs (ICE/DTLS/RTSP/esp_peer) vers un ring SPIRAM pour la
  // page Micro/Debug. Installé tôt : capture continue dès le boot. Chaîne
  // vers la sortie UART d'origine (aucun log perdu).
  dbg_log::init();

  // Phase 1 du journal SD : le ring capte dès maintenant, avant même que la
  // carte soit montable. Sans ça, les logs de settings et du BSP — les plus
  // utiles au diagnostic d'un boot raté — n'atteindraient jamais le fichier.
  sd_log::init_ring();

  // Avant tout module réseau : charge la NVS (ou les défauts compilés) et
  // formate les URLs que camera_session/mic_session recevront ensuite.
  settings::init();

  // Préférences de confort (DND). Après la NVS, avant l'UI qui les affiche.
  prefs::init();

  const esp_err_t bsp_err = bsp::Bsp::instance().init();
  if (bsp_err != ESP_OK) {
    // Écran/tactile morts : on ne lance PAS l'UI (tout appel lv_* planterait),
    // mais on continue le boot pour que le WiFi + l'API + OTA restent
    // disponibles — c'est la seule voie de récupération à distance.
    ESP_LOGE(TAG, "BSP init: %s — UI désactivée, boot poursuivi pour l'OTA",
             esp_err_to_name(bsp_err));
  }

  // Phase 2 : montage + tâche d'écriture. Après le BSP, qui possède le
  // contrôleur SDMMC et le canal LDO ; avant les modules réseau, dont les
  // logs de connexion sont justement ce qu'on veut retrouver après un reboot.
  sd_log::start();
  sd_log::self_test();

  // Après le montage de la carte : crée /sdcard/calls et relit le compteur
  // d'appels manqués. Avant camera_session/mic_session, qui l'appellent.
  call_history::init({camera_session::active, camera_session::stop});

  // Sonnerie personnalisée : /sdcard/ring.wav préchargée en PSRAM. L'ordre est
  // contraignant — cet appel doit rester AVANT audio_pipeline::init(), qui crée
  // aud_render, seule tâche lectrice de la source. Écrire le pointeur pendant
  // que cette tâche n'existe pas encore est ce qui rend inutile tout verrou
  // dans le chemin chaud du mixeur. Ne pas déplacer plus bas.
  ringtone::set_volume(prefs::ring_volume());
  ringtone_sd::load();

  // Plus aucun self-test au boot : la logique pure (A-law, sonnerie, parsers
  // météo/agenda) est couverte par la suite host de test/. Ne reste ici que la
  // construction de la table A-law, et le contrôle carte SD ci-dessus — le
  // seul qui dépende d'un état matériel propre à ce démarrage.
  g711::init();

  // Pipelines : le probe d'état de streaming vient de la session caméra, ce qui
  // évite une dépendance circulaire entre les modules app/.
  if (!audio_pipeline::init(camera_session::active)) {
    // Sonnerie et audio distant muets, mais l'image et le micro restent
    // fonctionnels : on poursuit le boot en le disant.
    ESP_LOGE(TAG, "audio_pipeline::init a échoué (lecture haut-parleur muette)");
  }
  // Moteur JPEG HW créé tôt : il exige de la RAM interne DMA, rare après
  // l'ouverture de WebRTC.
  video_mjpeg::init(camera_session::active);

  if (!camera_session::init(settings::go2rtc_audio_url(),
                            settings::go2rtc_mjpeg2_url())) {
    ESP_LOGE(TAG, "camera_session::init a échoué");
  }
  if (!mic_session::init(settings::go2rtc_mic_url())) {
    ESP_LOGE(TAG, "mic_session::init a échoué");
  }

  if (bsp_err == ESP_OK) {
    // ThorVG's rleRender() alone puts a 16KB Cell buffer on the stack
    // (tvgSwRle.cpp) -- the old 16384-byte SRAM stack left zero margin for
    // the rest of the call chain (shapeGenRle, canvas draw, LVGL drawing)
    // and crashed with a stack protection fault the moment a Lottie widget
    // rendered. Grown to 49152 and moved to PSRAM (xTaskCreateWithCaps),
    // consistent with the other large-stack tasks in this file.
    if (xTaskCreateWithCaps(lvgl_task, "lvgl", 49152, nullptr, 5, nullptr,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
      ESP_LOGE(TAG, "Échec de la création de lvgl_task en PSRAM !");
    }
  }

  const bool net_up = wifi_net::connect_blocking(settings::get().wifi_ssid,
                                                 settings::get().wifi_pass);
  if (!net_up) {
    // Boot poursuivi sans IP : le driver retente en arrière-plan et l'API/OTA
    // se raccrocheront seuls si le lien monte. Renoncer ici supprimerait les
    // seules voies de récupération à distance.
    ESP_LOGE(TAG, "WiFi non connecté : API/OTA démarrés quand même");
  }

  // Après l'IP : le serveur écoute sur l'interface réseau. Démarré seulement si
  // la préférence est armée — éteint par défaut, cf. sys/prefs.h.
  file_server::init({file_server_sd_busy, [](const char *path) {
                       ota_ctrl::request_file(path);
                     }});
  if (prefs::file_server() && !file_server::start()) {
    ESP_LOGE(TAG, "serveur de fichiers indisponible (préférence armée)");
  }

  // AVANT ha_api::init : Home Assistant peut appuyer sur un bouton OTA dès sa
  // connexion. Initialisé après, ota_ctrl::request() tombait sur
  // s_req_sem == nullptr et la commande OTA était jetée sans trace.
  ota_ctrl::init(ota_is_busy, ota_report_cb);

  // Logs live vers les clients de l'API abonnés (option « logs » de HA,
  // aioesphomeapi-logs). Le sink ne copie que si quelqu'un écoute.
  if (!dbg_log::add_line_sink(esphome_api::log_line)) {
    ESP_LOGE(TAG, "sink de logs API non installé (slots pleins)");
  }

  // ESPHome native API: mDNS + server on 6053. After Wi-Fi (mDNS registers on
  // the STA netif); the server waits for the link if it is still down.
  ha_api::init({ring_cb});

  // Caméra de présence AVANT la validation OTA : une image qui planterait dès
  // l'init du CSI ou du capteur doit encore pouvoir revenir en arrière. Après
  // ha_api : le premier changement d'état y est publié directement.
  presence::init({[] { ha_api::publish_now("presence"); }});

  // Point de validation "boot sain" = WiFi (déjà connecté) + Home Assistant
  // connecté à l'API. Attente bornée (~60 s : HA se reconnecte sur l'annonce
  // mDNS, avec un backoff) : au-delà, on ne valide pas ici.
  for (int i = 0; i < 120 && !ha_api::connected(); i++) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  // Conditionnel : valider une image qui n'a jamais vu Home Assistant vidait
  // le rollback de son sens. Sinon net_wdt reprend la validation au premier
  // tick connecté — ou redémarre, et le bootloader revient en arrière.
  if (ha_api::connected()) {
    ota_confirm_valid_if_pending();
  }

  // Dernier module démarré : la fenêtre de 5 min ne doit pas courir pendant le
  // boot (WiFi, API et validation OTA viennent tout juste d'aboutir).
  net_wdt::init(net_wdt_freeze_cb, ota_confirm_valid_if_pending);

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
