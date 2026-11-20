#include "ui_sdlog.h"

#include "esp_timer.h"

#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <strings.h>

#include "app_config.h"
#include "bsp.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sys/ota_ctrl.h"
#include "sys/sd_log.h"
#include "sys/usb_msc.h"
#include "ui_mic.h"
#include "ui_usb.h"
#include "theme.h"

namespace ui_sdlog {

namespace {

constexpr char TAG[] = "uisd";

// Nombre de fichiers listés. Au-delà, la purge de sd_log a de toute façon
// fait son travail (rétention 14 jours).
constexpr int kMaxFiles = 16;

// Requête déposée par la tâche LVGL, exécutée par la tâche disque.
enum class Job : uint8_t {
  kNone,
  kList,
  kPage,
  kFormat,
  kUsbEnter,
  kUsbLeave,
  kFwList,  // énumère les .bin de la racine et lit leurs en-têtes
  kFwFlash, // arme l'OTA sur le fichier choisi
};

lv_obj_t *s_status_lbl = nullptr;
lv_obj_t *s_list = nullptr;
lv_obj_t *s_view = nullptr;
lv_obj_t *s_prev_btn = nullptr;
lv_obj_t *s_next_btn = nullptr;
lv_timer_t *s_timer = nullptr;

SemaphoreHandle_t s_job_sem = nullptr;
std::atomic<Job> s_job{Job::kNone};
// Nature du résultat disponible, publiée par la tâche disque. Distincte de
// s_job : poll_timer remettait autrefois s_job à kNone après lecture, si bien
// qu'une requête postée juste après s_busy=false et juste avant cette remise à
// zéro était écrasée — le clic ne faisait alors strictement rien.
std::atomic<Job> s_result_job{Job::kNone};
std::atomic<bool> s_result_ready{false};
std::atomic<bool> s_busy{false};

// Fichier consulté et fenêtre de lecture. Écrits par la tâche LVGL avant
// d'armer la requête, lus par la tâche disque ensuite : le sémaphore fait la
// barrière, s_busy interdit une seconde requête entre-temps.
char s_open_name[32] = {};
long s_page_offset = 0;
long s_file_size = 0;

// Résultats produits par la tâche disque, consommés par la tâche LVGL.
char *s_page_buf = nullptr; // cfg::sd::kViewPageBytes + 1, PSRAM
char s_names[kMaxFiles][32] = {};
long s_sizes[kMaxFiles] = {};
int s_name_count = 0;
char s_result_msg[64] = {};

// --- Mise à jour du firmware depuis la carte -------------------------------
// Nombre de .bin listés à la racine. Au-delà, ce n'est plus un dépôt de
// firmware mais une sauvegarde.
constexpr int kMaxFw = 8;
struct FwEntry {
  char name[32];
  ota_ctrl::ImageInfo info;
  bool valid; // en-tête reconnu : sinon la ligne est grisée, non cliquable
};
// Écrits par la tâche disque, lus par la tâche LVGL après s_result_ready —
// même barrière que les autres résultats.
FwEntry s_fw[kMaxFw] = {};
int s_fw_count = 0;

// Chemin choisi par l'utilisateur, écrit par LVGL avant d'armer kFwFlash.
char s_fw_path[96] = {};
// La modale est retenue ici plutôt que retrouvée en remontant les parents du
// bouton cliqué : la hiérarchie interne de lv_msgbox n'est pas un contrat.
lv_obj_t *s_fw_mbox = nullptr;

void request(Job j) {
  if (s_busy.exchange(true)) {
    return; // une opération est déjà en vol
  }
  s_job.store(j);
  xSemaphoreGive(s_job_sem);
}

void do_list() {
  s_name_count = 0;
  DIR *dir = opendir(cfg::sd::kLogDir);
  if (!dir) {
    snprintf(s_result_msg, sizeof(s_result_msg), "dossier %s illisible",
             cfg::sd::kLogDir);
    return;
  }
  struct dirent *ent = nullptr;
  while ((ent = readdir(dir)) != nullptr && s_name_count < kMaxFiles) {
    if (ent->d_type == DT_DIR) {
      continue;
    }
    // dirent::d_name fait 256 octets dans cette configuration newlib : les
    // tampons doivent être dimensionnés au pire cas, sinon
    // -Werror=format-truncation.
    char path[288];
    snprintf(path, sizeof(path), "%s/%s", cfg::sd::kLogDir, ent->d_name);
    struct stat st = {};
    if (stat(path, &st) != 0) {
      continue;
    }
    // Précision explicite : un nom plus long que le slot est tronqué, ce qui
    // est acceptable pour un affichage (les journaux font 14 caractères).
    snprintf(s_names[s_name_count], sizeof(s_names[0]), "%.31s", ent->d_name);
    s_sizes[s_name_count] = static_cast<long>(st.st_size);
    s_name_count++;
  }
  closedir(dir);

  // Tri décroissant : les noms sont des dates ISO, l'ordre lexicographique EST
  // l'ordre chronologique. Tri à bulles, kMaxFiles entrées au plus.
  for (int i = 0; i < s_name_count; i++) {
    for (int j = i + 1; j < s_name_count; j++) {
      if (strcmp(s_names[j], s_names[i]) > 0) {
        // memcpy et non snprintf : source et destination sont deux lignes du
        // MÊME tableau, que GCC 15 ne peut pas prouver disjointes
        // (-Wrestrict). Un échange de lignes de taille fixe n'a de toute façon
        // rien à faire dans un formateur de chaîne.
        char tmp[sizeof(s_names[0])];
        memcpy(tmp, s_names[i], sizeof(tmp));
        memcpy(s_names[i], s_names[j], sizeof(tmp));
        memcpy(s_names[j], tmp, sizeof(tmp));
        const long ts = s_sizes[i];
        s_sizes[i] = s_sizes[j];
        s_sizes[j] = ts;
      }
    }
  }
  s_result_msg[0] = '\0';
}

void do_page() {
  s_page_buf[0] = '\0';
  char path[96];
  snprintf(path, sizeof(path), "%s/%s", cfg::sd::kLogDir, s_open_name);
  FILE *f = fopen(path, "r");
  if (!f) {
    snprintf(s_result_msg, sizeof(s_result_msg), "ouverture impossible");
    return;
  }
  fseek(f, 0, SEEK_END);
  s_file_size = ftell(f);

  const long page = static_cast<long>(cfg::sd::kViewPageBytes);
  if (s_page_offset < 0) {
    s_page_offset = 0;
  }
  if (s_page_offset > s_file_size) {
    // Sentinelle « ouvrir à la fin » (LONG_MAX) ou dépassement par le bouton
    // Suivant : on retombe sur la dernière page complète.
    s_page_offset = (s_file_size > page) ? s_file_size - page : 0;
  }
  fseek(f, s_page_offset, SEEK_SET);
  const size_t n = fread(s_page_buf, 1, static_cast<size_t>(page), f);
  fclose(f);
  s_page_buf[n] = '\0';

  // Coupe la première ligne partielle : une page qui démarre au milieu d'un
  // mot est illisible. Rien à couper en début de fichier.
  if (s_page_offset > 0 && n > 0) {
    char *nl = strchr(s_page_buf, '\n');
    if (nl && static_cast<size_t>(nl - s_page_buf) < n - 1) {
      memmove(s_page_buf, nl + 1, n - static_cast<size_t>(nl - s_page_buf));
    }
  }
  s_result_msg[0] = '\0';
}

void do_format() {
  // Attente d'acquittement, pas un simple drapeau : la tâche sd_log peut être
  // en plein fwrite/fsync avec le fichier du jour ouvert. Formater sous elle
  // démontait la partition avec un descripteur vivant dessus.
  if (!sd_log::pause_and_wait()) {
    sd_log::resume();
    snprintf(s_result_msg, sizeof(s_result_msg),
             "journal SD occupe : formatage annule");
    return;
  }
  const esp_err_t err = bsp::Bsp::instance().sd_format();
  if (err == ESP_OK) {
    mkdir(cfg::sd::kLogDir, 0777);
    snprintf(s_result_msg, sizeof(s_result_msg), "formatage OK");
  } else {
    snprintf(s_result_msg, sizeof(s_result_msg), "formatage : %s",
             esp_err_to_name(err));
  }
  sd_log::resume();
  s_open_name[0] = '\0';
  do_list();
}

// Tâche disque : jamais la tâche LVGL. La séquence démonte un système de
// fichiers et installe un pilote USB, elle dure des centaines de ms.
void do_usb_enter() {
  if (usb_msc::enter()) {
    ui_usb::show();
    return;
  }
  snprintf(s_result_msg, sizeof(s_result_msg), "mode USB : %s",
           usb_msc::last_error());
}

void do_usb_leave() {
  if (!usb_msc::leave()) {
    snprintf(s_result_msg, sizeof(s_result_msg), "sortie USB : %s",
             usb_msc::last_error());
  }
  // Retour à la page Micro/Debug, que le remontage ait abouti ou non : rester
  // sur l'écran USB alors que le mode est terminé serait un mensonge. L'état
  // réel de la carte s'affiche dans l'onglet — poll_timer appelle
  // refresh_status() toutes les 300 ms de lui-même. Ne PAS poster de requête
  // ici : request() sort immédiatement tant que s_busy est vrai, ce qui est
  // encore le cas dans ce contexte, et l'appel serait silencieusement perdu.
  ui_mic::open_screen();
}

// Énumère les .bin de la RACINE de la carte et lit leur en-tête. Bloquant :
// n'existe que dans la tâche disque.
void do_fw_list() {
  s_result_msg[0] = '\0';
  s_fw_count = 0;
  DIR *dir = opendir(cfg::sd::kMountPoint);
  if (!dir) {
    snprintf(s_result_msg, sizeof(s_result_msg), "racine %s illisible",
             cfg::sd::kMountPoint);
    return;
  }
  struct dirent *ent = nullptr;
  while ((ent = readdir(dir)) != nullptr && s_fw_count < kMaxFw) {
    const char *dot = strrchr(ent->d_name, '.');
    if (dot == nullptr || strcasecmp(dot, ".bin") != 0) {
      continue;
    }
    // dirent::d_name fait 256 octets : dimensionner au pire cas, sinon
    // -Werror=format-truncation (même piège que do_list).
    char full[288];
    snprintf(full, sizeof(full), "%s/%s", cfg::sd::kMountPoint, ent->d_name);
    FwEntry &fw = s_fw[s_fw_count];
    snprintf(fw.name, sizeof(fw.name), "%.31s", ent->d_name);
    fw.valid = ota_ctrl::probe_file(full, &fw.info);
    s_fw_count++;
  }
  closedir(dir);
  if (s_fw_count == 0) {
    snprintf(s_result_msg, sizeof(s_result_msg),
             "aucun .bin a la racine de la carte");
  }
}

// Tâche disque : priorité 1, cœur 0, comme net_wdt. Elle ne dispute jamais le
// cœur à la tâche de drain TCP MJPEG, qui y tourne bien plus haut.
void disk_task(void *arg) {
  (void)arg;
  for (;;) {
    xSemaphoreTake(s_job_sem, portMAX_DELAY);
    const Job job = s_job.load();
    switch (job) {
    case Job::kList:
      do_list();
      break;
    case Job::kPage:
      do_page();
      break;
    case Job::kFormat:
      do_format();
      break;
    case Job::kUsbEnter:
      do_usb_enter();
      break;
    case Job::kUsbLeave:
      do_usb_leave();
      break;
    case Job::kFwList:
      do_fw_list();
      break;
    case Job::kFwFlash:
      // request_file est non bloquante : elle copie le chemin et réveille la
      // tâche ota. Rien à attendre ici.
      ota_ctrl::request_file(s_fw_path);
      break;
    default:
      break;
    }
    // Publier la nature du résultat AVANT de rendre la main : s_busy=false
    // rouvre la porte à une nouvelle requête, qui réécrira s_job.
    s_result_job.store(job);
    s_result_ready.store(true);
    s_busy.store(false);
  }
}

void refresh_status() {
  // Seul état qu'on ne sait pas défaire : une sortie du mode clé USB dont le
  // remontage a échoué. L'appareil n'est plus en mode, mais il n'a pas
  // retrouvé sa carte — c'est ce que l'utilisateur doit lire en premier.
  if (usb_msc::status() == usb_msc::Status::kError) {
    lv_label_set_text(s_status_lbl,
                      "Carte non remontee apres le mode USB - formater");
    return;
  }
  const bsp::SdState state = bsp::Bsp::instance().sd_state();
  if (state != bsp::SdState::kMounted) {
    lv_label_set_text_fmt(
        s_status_lbl, "Carte : %s (%s)",
        state == bsp::SdState::kAbsent ? "absente" : "erreur",
        esp_err_to_name(bsp::Bsp::instance().sd_last_error()));
    return;
  }
  // f_getfree (esp_vfs_fat_info) prend le verrou du volume FATFS : appelé
  // toutes les 300 ms depuis la tâche LVGL, il faisait attendre l'UI derrière
  // un fsync de sd_log ou une lecture de calls_load. 10 s de cache suffisent
  // pour un espace libre.
  static uint64_t total = 0, freeb = 0;
  static int64_t s_cap_us = 0;
  const int64_t now_us = esp_timer_get_time();
  if (s_cap_us == 0 || now_us - s_cap_us > 10000000) {
    s_cap_us = now_us;
    (void)bsp::Bsp::instance().sd_capacity(&total, &freeb);
  }
  const uint32_t dropped = sd_log::dropped_lines();
  if (dropped > 0) {
    lv_label_set_text_fmt(
        s_status_lbl, "Carte montee - %llu Mo, %llu Mo libres - %s - %lu "
                      "lignes perdues",
        static_cast<unsigned long long>(total / (1024ULL * 1024)),
        static_cast<unsigned long long>(freeb / (1024ULL * 1024)),
        sd_log::current_file(), static_cast<unsigned long>(dropped));
  } else {
    lv_label_set_text_fmt(
        s_status_lbl, "Carte montee - %llu Mo, %llu Mo libres - %s",
        static_cast<unsigned long long>(total / (1024ULL * 1024)),
        static_cast<unsigned long long>(freeb / (1024ULL * 1024)),
        sd_log::current_file());
  }
}

void cb_file_clicked(lv_event_t *e) {
  lv_obj_t *btn = static_cast<lv_obj_t *>(lv_event_get_target(e));
  const char *txt = lv_list_get_button_text(s_list, btn);
  if (!txt) {
    return;
  }
  // Le libellé vaut "nom  (N Ko)" : n'en garder que le nom.
  size_t i = 0;
  while (txt[i] && txt[i] != ' ' && i < sizeof(s_open_name) - 1) {
    s_open_name[i] = txt[i];
    i++;
  }
  s_open_name[i] = '\0';
  // Ouvrir sur la FIN du fichier : un plantage y est toujours. do_page ramène
  // cet offset volontairement excessif sur la dernière page complète, une fois
  // la taille connue — elle ne l'est pas ici, pas de stat côté LVGL.
  s_page_offset = LONG_MAX;
  lv_textarea_set_text(s_view, "Lecture...");
  request(Job::kPage);
}

void cb_prev(lv_event_t *e) {
  (void)e;
  if (s_open_name[0] == '\0') {
    return;
  }
  s_page_offset -= static_cast<long>(cfg::sd::kViewPageBytes);
  request(Job::kPage);
}

void cb_next(lv_event_t *e) {
  (void)e;
  if (s_open_name[0] == '\0') {
    return;
  }
  s_page_offset += static_cast<long>(cfg::sd::kViewPageBytes);
  request(Job::kPage);
}

void cb_refresh(lv_event_t *e) {
  (void)e;
  request(Job::kList);
}

void cb_format_confirmed(lv_event_t *e) {
  lv_obj_t *mbox = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
  lv_msgbox_close(mbox);
  lv_textarea_set_text(s_view, "Formatage en cours...");
  request(Job::kFormat);
}

void cb_format_cancel(lv_event_t *e) {
  lv_msgbox_close(static_cast<lv_obj_t *>(lv_event_get_user_data(e)));
}

void cb_usb_confirmed(lv_event_t *e) {
  lv_obj_t *mbox = static_cast<lv_obj_t *>(lv_event_get_user_data(e));
  lv_msgbox_close(mbox);
  request(Job::kUsbEnter);
}

void cb_usb_cancel(lv_event_t *e) {
  lv_msgbox_close(static_cast<lv_obj_t *>(lv_event_get_user_data(e)));
}

void cb_usb(lv_event_t *e) {
  (void)e;
  // Confirmation non parce que c'est destructif, mais parce que ça coupe la
  // sonnette : un visiteur qui sonne pendant le transfert n'est pas vu.
  lv_obj_t *mbox = lv_msgbox_create(NULL);
  lv_msgbox_add_title(mbox, "Mode cle USB");
  lv_msgbox_add_text(mbox, "Exposer la carte SD au PC ?\n"
                           "La sonnette sera ignoree pendant ce mode.");
  lv_obj_t *cancel = lv_msgbox_add_footer_button(mbox, "Annuler");
  lv_obj_add_event_cb(cancel, cb_usb_cancel, LV_EVENT_CLICKED, mbox);
  lv_obj_t *ok = lv_msgbox_add_footer_button(mbox, "Exposer");
  lv_obj_add_event_cb(ok, cb_usb_confirmed, LV_EVENT_CLICKED, mbox);
}

void cb_format(lv_event_t *e) {
  (void)e;
  lv_obj_t *mbox = lv_msgbox_create(NULL);
  lv_msgbox_add_title(mbox, "Formater la carte");
  lv_msgbox_add_text(mbox, "Effacer TOUT le contenu de la carte ?\n"
                           "Cette action est irreversible.");
  lv_obj_t *cancel = lv_msgbox_add_footer_button(mbox, "Annuler");
  lv_obj_add_event_cb(cancel, cb_format_cancel, LV_EVENT_CLICKED, mbox);
  lv_obj_t *ok = lv_msgbox_add_footer_button(mbox, "Effacer");
  lv_obj_set_style_bg_color(ok, lv_color_hex(ui::theme::active.btn_danger), 0);
  lv_obj_add_event_cb(ok, cb_format_confirmed, LV_EVENT_CLICKED, mbox);
}

void cb_fw_cancel(lv_event_t *e) {
  (void)e;
  if (s_fw_mbox != nullptr) {
    lv_msgbox_close(s_fw_mbox);
    s_fw_mbox = nullptr;
  }
}

// L'entrée choisie voyage en user_data : s_fw est un tableau de portée fichier,
// ses adresses restent valides tant que la modale existe.
void cb_fw_chosen(lv_event_t *e) {
  const FwEntry *fw = static_cast<const FwEntry *>(lv_event_get_user_data(e));
  if (fw == nullptr || !fw->valid) {
    return;
  }
  snprintf(s_fw_path, sizeof(s_fw_path), "%s/%.31s", cfg::sd::kMountPoint,
           fw->name);
  if (s_fw_mbox != nullptr) {
    lv_msgbox_close(s_fw_mbox);
    s_fw_mbox = nullptr;
  }
  lv_textarea_set_text(s_view, "Mise a jour lancee. Suivre le statut OTA.");
  request(Job::kFwFlash);
}

// Construit la modale de choix. Appelée depuis poll_timer, donc déjà dans la
// tâche LVGL : aucun lock supplémentaire.
void show_fw_dialog() {
  if (s_fw_count == 0) {
    return; // do_fw_list a déjà rempli s_result_msg
  }
  s_fw_mbox = lv_msgbox_create(NULL);
  lv_msgbox_add_title(s_fw_mbox, "Mettre a jour le firmware");
  const esp_app_desc_t *cur = esp_app_get_description();
  lv_msgbox_add_text_fmt(s_fw_mbox, "Version actuelle : %s",
                         cur ? cur->version : "?");

  for (int i = 0; i < s_fw_count; i++) {
    FwEntry &fw = s_fw[i];
    char line[160];
    if (fw.valid) {
      snprintf(line, sizeof(line), "%.31s\n%.31s %.31s - %u Ko", fw.name,
               fw.info.project, fw.info.version,
               (unsigned)(fw.info.size_bytes / 1024));
    } else {
      snprintf(line, sizeof(line), "%.31s\n(pas un firmware)", fw.name);
    }
    lv_obj_t *btn = lv_msgbox_add_footer_button(s_fw_mbox, line);
    if (!fw.valid) {
      lv_obj_add_state(btn, LV_STATE_DISABLED);
      continue;
    }
    lv_obj_add_event_cb(btn, cb_fw_chosen, LV_EVENT_CLICKED, &fw);
  }
  lv_obj_t *cancel = lv_msgbox_add_footer_button(s_fw_mbox, "Annuler");
  lv_obj_add_event_cb(cancel, cb_fw_cancel, LV_EVENT_CLICKED, nullptr);
}

void cb_firmware(lv_event_t *e) {
  (void)e;
  // La carte appartient au PC en mode clé USB : aucune lecture possible.
  if (usb_msc::status() == usb_msc::Status::kActive) {
    lv_textarea_set_text(s_view, "Mode cle USB actif : carte indisponible.");
    return;
  }
  request(Job::kFwList);
}

// Relève les résultats de la tâche disque. Tourne dans la tâche LVGL : seul
// endroit d'où les lv_* sont appelés.
void poll_timer(lv_timer_t *t) {
  (void)t;
  // La page Micro/Debug n'a pas de hook de fermeture pour cet onglet : on
  // vérifie ici que son écran est bien celui affiché. Sinon ce timer
  // repeindrait un label trois fois par seconde pendant un flux caméra, au
  // détriment du rendu vidéo.
  if (lv_obj_get_screen(s_status_lbl) != lv_screen_active()) {
    return;
  }
  refresh_status();

  const bool busy = s_busy.load();
  lv_obj_set_state(s_prev_btn, LV_STATE_DISABLED, busy);
  lv_obj_set_state(s_next_btn, LV_STATE_DISABLED, busy);

  if (!s_result_ready.exchange(false)) {
    return;
  }

  if (s_result_msg[0]) {
    lv_textarea_set_text(s_view, s_result_msg);
  }

  switch (s_result_job.load()) {
  case Job::kList:
  case Job::kFormat:
    lv_obj_clean(s_list);
    for (int i = 0; i < s_name_count; i++) {
      char label[64];
      snprintf(label, sizeof(label), "%.31s  (%ld Ko)", s_names[i],
               s_sizes[i] / 1024);
      lv_obj_t *btn = lv_list_add_button(s_list, LV_SYMBOL_FILE, label);
      lv_obj_add_event_cb(btn, cb_file_clicked, LV_EVENT_CLICKED, NULL);
    }
    break;
  case Job::kFwList:
    show_fw_dialog();
    break;
  case Job::kPage:
    if (!s_result_msg[0]) {
      // lv_textarea_set_text() laisse le curseur en fin de texte et scrolle
      // dessus : la vue s'ouvre donc sur les lignes les plus récentes de la
      // page, ce qu'on veut pour un journal. Ne pas remettre le curseur à 0,
      // cela renvoyait la vue en haut de page à chaque chargement.
      lv_textarea_set_text(s_view, s_page_buf);
    }
    break;
  default:
    break;
  }
  // s_job n'est PAS remis à zéro ici : il appartient désormais à la requête
  // suivante, qui a pu être postée pendant ce traitement.
  s_result_job.store(Job::kNone);
}

} // namespace

void create_tab(lv_obj_t *parent) {
  lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

  s_page_buf = static_cast<char *>(heap_caps_malloc(
      static_cast<size_t>(cfg::sd::kViewPageBytes) + 1, MALLOC_CAP_SPIRAM));
  if (!s_page_buf) {
    lv_obj_t *err = lv_label_create(parent);
    lv_label_set_text(err, "PSRAM insuffisante pour la visionneuse");
    lv_obj_set_style_text_color(err, lv_color_white(), 0);
    lv_obj_center(err);
    return;
  }
  s_page_buf[0] = '\0';

  s_status_lbl = lv_label_create(parent);
  lv_obj_set_style_text_color(s_status_lbl, lv_color_white(), 0);
  lv_label_set_text(s_status_lbl, "Carte : ...");
  lv_obj_align(s_status_lbl, LV_ALIGN_TOP_LEFT, 8, 4);

  s_list = lv_list_create(parent);
  lv_obj_set_size(s_list, 300, 340);
  lv_obj_align(s_list, LV_ALIGN_TOP_LEFT, 8, 36);

  s_view = lv_textarea_create(parent);
  lv_obj_set_size(s_view, 600, 340);
  lv_obj_align(s_view, LV_ALIGN_TOP_RIGHT, -8, 36);
  lv_textarea_set_text(s_view, "");
  lv_obj_add_state(s_view, LV_STATE_DISABLED); // lecture seule
  // Visionneuse, pas éditeur : sans ça chaque PRESSED/PRESSING appelle
  // update_cursor_position_on_click(), qui fait un lv_label_get_letter_on() sur
  // les 4 Ko de page à chaque mouvement de doigt PUIS repositionne le scroll sur
  // le caractère touché (lv_textarea_scroll_to_cusor_pos). Le défilement tactile
  // était donc annulé en permanence — impossible de tenir la vue en bas.
  lv_textarea_set_cursor_click_pos(s_view, false);

  struct ButtonSpec {
    const char *label;
    lv_event_cb_t cb;
    int x;
    lv_obj_t **out;
    uint32_t color;
  };
  static const ButtonSpec kButtons[] = {
      {LV_SYMBOL_LEFT " Prec", cb_prev, 320, &s_prev_btn, ui::theme::active.btn_neutral},
      {"Suiv " LV_SYMBOL_RIGHT, cb_next, 470, &s_next_btn, ui::theme::active.btn_neutral},
      {LV_SYMBOL_REFRESH " Actualiser", cb_refresh, 620, nullptr, ui::theme::active.btn_neutral},
      {LV_SYMBOL_TRASH " Formater", cb_format, 790, nullptr, ui::theme::active.btn_danger},
      {LV_SYMBOL_USB " Cle USB", cb_usb, 940, nullptr, ui::theme::active.btn_usb},
      {LV_SYMBOL_DOWNLOAD " Firmware", cb_firmware, 1090, nullptr, ui::theme::active.btn_fw},
  };
  for (const ButtonSpec &b : kButtons) {
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 145, 60);
    lv_obj_align(btn, LV_ALIGN_TOP_LEFT, b.x, 390);
    lv_obj_set_style_bg_color(btn, lv_color_hex(b.color), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0); // cf. contrainte CPU, ui.cpp
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, b.label);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    lv_obj_add_event_cb(btn, b.cb, LV_EVENT_CLICKED, NULL);
    if (b.out) {
      *b.out = btn;
    }
  }

  // Le bouton Terminer de l'écran USB ne fait que déposer la requête : le
  // démontage appartient à cette tâche disque, pas à LVGL.
  ui_usb::set_leave_cb([] { request(Job::kUsbLeave); });

  s_job_sem = xSemaphoreCreateBinary();
  if (!s_job_sem ||
      xTaskCreatePinnedToCore(disk_task, "sd_ui", 4096, nullptr, 1, nullptr,
                              0) != pdPASS) {
    ESP_LOGE(TAG, "creation de la tache sd_ui impossible");
    return;
  }

  s_timer = lv_timer_create(poll_timer, 300, NULL);
  request(Job::kList);
}

} // namespace ui_sdlog
