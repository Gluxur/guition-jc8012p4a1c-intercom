#include "sys/usb_msc.h"

#include "esp_log.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_msc.h"

#include "bsp.h"
#include "media/ringtone_sd.h"
#include "sys/sd_log.h"

#include <atomic>
#include <cstdio>

namespace usb_msc {
namespace {

constexpr const char *TAG = "usb_msc";

// Atomique : écrit par la tâche disque de l'UI, lu par la tâche de l'API (sonnette),
// net_wdt, httpd et cam_stop.
std::atomic<Status> s_status{Status::kOff};
char s_error[96] = "";

// Écrit par la tâche TinyUSB, lu par le timer LVGL de l'écran : atomique,
// jamais volatile — en C++ `volatile` n'impose que la relecture mémoire.
std::atomic<bool> s_host_connected{false};
// Posé par le callback de stockage. Sert à savoir si le montage a RÉELLEMENT
// abouti : tinyusb_msc_set_storage_mount_point() avale les échecs de ses
// helpers internes et renvoie ESP_OK quoi qu'il arrive.
std::atomic<bool> s_mount_failed{false};

tinyusb_msc_storage_handle_t s_storage = nullptr;
void *s_card = nullptr;

void set_error(const char *fmt, esp_err_t err) {
  snprintf(s_error, sizeof s_error, fmt, esp_err_to_name(err));
  ESP_LOGE(TAG, "%s", s_error);
}

// Contexte tâche TinyUSB : pas de log ici, la tâche porte le transfert.
void on_usb_event(tinyusb_event_t *event, void *arg) {
  (void)arg;
  if (event->id == TINYUSB_EVENT_ATTACHED) {
    s_host_connected.store(true, std::memory_order_relaxed);
  } else if (event->id == TINYUSB_EVENT_DETACHED) {
    s_host_connected.store(false, std::memory_order_relaxed);
  }
}

void on_storage_event(tinyusb_msc_storage_handle_t handle,
                      tinyusb_msc_event_t *event, void *arg) {
  (void)handle;
  (void)arg;
  if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ||
      event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED ||
      event->id == TINYUSB_MSC_EVENT_FORMAT_FAILED) {
    s_mount_failed.store(true, std::memory_order_relaxed);
  }
}

// Défait, dans l'ordre inverse, ce qui a été construit jusqu'au point atteint.
// Chaque étape d'enter() appelle ce nettoyage avec son propre niveau : c'est
// l'échelle « goto cleanup » de la règle projet, écrite en une fonction pour
// qu'aucun chemin de retour ne puisse en oublier une marche.
enum class Stage : uint8_t {
  kNothing,
  kLogPaused,
  kUnmounted,
  kCardInit,
  kMscDriver,
  kStorage,
};

void rollback(Stage reached) {
  switch (reached) {
  case Stage::kStorage:
    (void)tinyusb_msc_delete_storage(s_storage);
    s_storage = nullptr;
    [[fallthrough]];
  case Stage::kMscDriver:
    (void)tinyusb_msc_uninstall_driver();
    [[fallthrough]];
  case Stage::kCardInit:
    (void)bsp::Bsp::instance().sd_deinit_card_only(s_card);
    s_card = nullptr;
    [[fallthrough]];
  case Stage::kUnmounted:
    (void)bsp::Bsp::instance().sd_mount();
    [[fallthrough]];
  case Stage::kLogPaused:
    sd_log::resume();
    [[fallthrough]];
  case Stage::kNothing:
    break;
  }
}

} // namespace

bool enter() {
  if (s_status == Status::kActive) {
    return true;
  }
  s_error[0] = '\0';
  s_host_connected.store(false, std::memory_order_relaxed);
  s_mount_failed.store(false, std::memory_order_relaxed);

  // 1. Le journal SD doit avoir fermé son fichier : la tâche d'écriture peut
  // être en plein fwrite/fsync, et démonter sous elle casse la partition.
  if (!sd_log::pause_and_wait()) {
    snprintf(s_error, sizeof s_error, "journal SD occupe");
    ESP_LOGE(TAG, "%s", s_error);
    return false;
  }

  // 2. Rendre le système de fichiers avant de céder le support.
  esp_err_t err = bsp::Bsp::instance().sd_unmount();
  if (err != ESP_OK) {
    set_error("demontage : %s", err);
    rollback(Stage::kLogPaused);
    return false;
  }

  // 3. Matériel seul : c'est esp_tinyusb qui montera la FAT.
  err = bsp::Bsp::instance().sd_init_card_only(&s_card);
  if (err != ESP_OK) {
    set_error("init carte : %s", err);
    rollback(Stage::kUnmounted);
    return false;
  }

  // 4. Pilote MSC. auto_mount_off : sans lui le composant bascule seul la
  // propriété du stockage au branchement et au débranchement du câble, ce qui
  // ferait dépendre l'état de l'appareil de ce qui est branché.
  tinyusb_msc_driver_config_t msc_cfg = {};
  msc_cfg.user_flags.auto_mount_off = 1;
  msc_cfg.callback = on_storage_event;
  msc_cfg.callback_arg = nullptr;
  err = tinyusb_msc_install_driver(&msc_cfg);
  if (err != ESP_OK) {
    set_error("pilote MSC : %s", err);
    rollback(Stage::kCardInit);
    return false;
  }

  // 5. Stockage adossé à la carte, propriété donnée à l'USB dès la création.
  tinyusb_msc_storage_config_t storage_cfg = {};
  storage_cfg.medium.card = static_cast<sdmmc_card_t *>(s_card);
  // do_not_format OBLIGATOIRE : à false, une carte sans système de fichiers
  // reconnaissable serait formatée AUTOMATIQUEMENT. Cela contredirait la
  // politique du BSP — « une carte illisible peut contenir les données de
  // quelqu'un, seul sd_format() l'efface ».
  storage_cfg.fat_fs.do_not_format = true;
  storage_cfg.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
  err = tinyusb_msc_new_storage_sdmmc(&storage_cfg, &s_storage);
  if (err != ESP_OK) {
    set_error("stockage MSC : %s", err);
    rollback(Stage::kMscDriver);
    return false;
  }

  // 6. Pile USB EN DERNIER : le LUN doit exister avant que les descripteurs
  // soient construits (ordre relevé dans les tests du composant, pas déduit).
  // TINYUSB_DEFAULT_CONFIG choisit TINYUSB_PORT_HIGH_SPEED_0 sur P4.
  tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG(on_usb_event);
  err = tinyusb_driver_install(&tusb_cfg);
  if (err != ESP_OK) {
    set_error("pile USB : %s", err);
    rollback(Stage::kStorage);
    return false;
  }

  s_status = Status::kActive;
  ESP_LOGI(TAG, "mode cle USB actif (port OTG High-Speed)");
  return true;
}

bool leave() {
  if (s_status != Status::kActive) {
    return true;
  }

  // Rendre la propriété à l'application avant de tout démonter : laisse au
  // composant l'occasion de vider ce qui est en attente.
  (void)tinyusb_msc_set_storage_mount_point(s_storage,
                                            TINYUSB_MSC_STORAGE_MOUNT_APP);
  (void)tinyusb_driver_uninstall();
  (void)tinyusb_msc_delete_storage(s_storage);
  s_storage = nullptr;
  (void)tinyusb_msc_uninstall_driver();
  (void)bsp::Bsp::instance().sd_deinit_card_only(s_card);
  s_card = nullptr;
  s_host_connected.store(false, std::memory_order_relaxed);

  // Seule source de vérité sur l'état de la carte : notre propre montage.
  const esp_err_t err = bsp::Bsp::instance().sd_mount();
  if (err != ESP_OK) {
    set_error("remontage : %s", err);
    s_status = Status::kError;
    return false;
  }

  sd_log::resume();
  // La raison d'être du mode : le ring.wav qui vient d'être déposé est chargé
  // sans redémarrage.
  ringtone_sd::load();

  s_status = Status::kOff;
  s_error[0] = '\0';
  ESP_LOGI(TAG, "mode cle USB termine, carte rendue a l'application");
  return true;
}

Status status() { return s_status; }

const char *last_error() { return s_error; }

bool host_connected() {
  return s_host_connected.load(std::memory_order_relaxed);
}

} // namespace usb_msc
