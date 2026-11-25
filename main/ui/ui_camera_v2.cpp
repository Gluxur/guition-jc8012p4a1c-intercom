#include "gate_style.h"
#include "ui_camera_v2.h"

#include "bsp.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "icons.h"
#include "lvgl.h"
#include "sys/prefs.h"
#include "sys/i18n.h"
#include "theme.h"
#include "sys/crumb.h"
#include <cstdio>
#include <atomic>

using i18n::Str;

// Écran caméra V2 : la vidéo (flux go2rtc 800x1280 = 720p transposé + padding)
// couvre TOUT le framebuffer, le décodeur JPEG matériel y écrivant directement
// (chemin zéro-copie, cf. Bsp::direct_video_target). Conséquence : les
// contrôles ne peuvent pas être posés une fois pour toutes comme dans
// ui_camera V1 — le décodeur les écrase à chaque frame. Ils sont donc
// pré-composés dans un buffer PSRAM par la tâche LVGL, et re-blittés par la
// tâche de rendu après chaque décode (band_blit).
//
// Habillage aligné sur ui_camera / ui_weather_v3 : panneau clair opaque, filet
// 1px, coins arrondis, AUCUNE ombre ni dégradé (contrainte CPU du device).

LV_FONT_DECLARE(montserrat_fr_16);
LV_FONT_DECLARE(montserrat_fr_24);
LV_FONT_DECLARE(montserrat_fr_36);

namespace ui_camera_v2 {

static const char *TAG = "ui_cam2";

// --- Palette (identique à ui_camera) ---------------------------------------
static const uint32_t &kPanel = ui::theme::active.card;  // fond du bandeau
static const uint32_t &kInk = ui::theme::active.ink;    // texte principal
static const uint32_t &kSub = ui::theme::active.sub;    // texte secondaire
static const uint32_t &kHair = ui::theme::active.hair;   // filet / rail de slider
static const uint32_t &kBadge = ui::theme::active.badge;  // fond bouton au repos
static const uint32_t &kWarm = ui::theme::active.warm;   // indicateur volume
static const uint32_t &kCold = ui::theme::active.cold;   // indicateur micro
static const uint32_t &kAnswer = ui::theme::active.answer; // bouton Répondre au repos
static const uint32_t &kMicOn = ui::theme::active.mic_on;  // micro actif (rouge)
static const uint32_t &kLiveBg = ui::theme::active.live_bg; // pastille « En ligne »

// --- Géométrie logique ------------------------------------------------------
static constexpr int32_t kScrW = 1280;
static constexpr int32_t kScrH = 800;
static constexpr int32_t kBandH = 80; // hauteur logique du bandeau
static constexpr int32_t kBandY = kScrH - kBandH;
// Pastille d'état : une ligne, ou deux avec les stats (bandeau de 80 px).
static constexpr int32_t kChipH = 48;
static constexpr int32_t kChipTallH = 64;

// Le bandeau tourné 90° occupe kBandH colonnes physiques sur toute la hauteur
// du panneau : 80 x 1280 px RGB565 = 200 Ko par buffer.
static constexpr int32_t kBandPhysW = kBandH;
static constexpr int32_t kBandPhysH = kScrW;
static constexpr size_t kBandPx = (size_t)kBandPhysW * kBandPhysH;

// Scratch de lv_snapshot : le bandeau entier en ARGB8888 = 1280*80*4 = 400 Ko.
// Le pool interne LVGL (64 Ko) est très loin du compte.
static constexpr size_t kSnapMemSize = 448 * 1024;

static ui::Callbacks s_cbs = {};

static lv_obj_t *s_scr = nullptr;
static lv_obj_t *s_band = nullptr; // conteneur LVGL du bandeau (source snapshot)
static lv_obj_t *s_stats_label = nullptr;
static lv_obj_t *s_answer_btn = nullptr;
static lv_obj_t *s_answer_lbl = nullptr;
static lv_obj_t *s_state_chip = nullptr;
static lv_obj_t *s_state_lbl = nullptr;

// État AFFICHÉ du décroché, piloté par le clic et non par mic_session::live() :
// live() reste faux tant que le peer s'ouvre (plusieurs secondes), le bouton
// repasserait au vert sous le doigt. Lu/écrit uniquement par la tâche LVGL.
static bool s_answered = false;
static int64_t s_answered_at_us = 0;
// Minutes déjà affichées dans la pastille. Résolution à la minute et non à la
// seconde : chaque recomposition du bandeau tient le mutex LVGL ~100 ms et fait
// perdre une frame vidéo (cf. update_stats) — un chrono à la seconde coûterait
// une frame par seconde pendant tout l'appel.
static int32_t s_shown_min = -1;

static void state_chip_refresh();
static void answer_set_visual(bool active);

static uint8_t *s_snap_mem = nullptr;
static uint16_t *s_band_buf[2] = {nullptr, nullptr};
static int s_band_next = 0;
// Publié par la tâche LVGL (compose), lu par la tâche de rendu (blit). Double
// buffer + échange de pointeur atomique : pas de mutex sur le chemin chaud, et
// jamais de bandeau à moitié recomposé à l'écran.
static std::atomic<uint16_t *> s_band_pub{nullptr};
// Buffer que band_blit est EN TRAIN de passer au PPA. Avec seulement deux
// slots, la troisième composition retombe sur le buffer publié deux tours plus
// tôt — que la tâche de rendu peut encore être en train de lire. Le blit dure
// moins d'une ms contre 50 ms de période du timer, donc ça n'arrivait jamais en
// pratique ; rien ne l'interdisait pour autant.
static std::atomic<uint16_t *> s_band_reading{nullptr};
// std::atomic et non volatile : `volatile` en C++ n'impose que la relecture
// mémoire, aucun ordonnancement.
static std::atomic<bool> s_band_dirty{true};

static lv_area_t s_video_zone_phys = {0, 0, 0, 0};
static lv_area_t s_band_phys = {0, 0, 0, 0};

// Dernier rafraîchissement du label de stats (cf. update_stats).
static int64_t s_stats_last_us = 0;

// Bouton portail, à droite du bandeau. Sa mise à jour ne passe PAS par la
// fenêtre de 5 s des stats : un changement d'état doit se voir tout de suite
// (band_timer_cb le relit toutes les 50 ms, un atomique).
static lv_obj_t *s_gate_btn = nullptr;
static lv_obj_t *s_gate_lbl = nullptr;
static uint32_t s_gate_shown = UINT32_MAX;
static void gate_refresh(bool force);

// --- Composition ------------------------------------------------------------

// Composite un pixel ARGB8888 LVGL sur le fond du panneau et le convertit en
// RGB565. Hors de la forme arrondie d'un bouton, lv_snapshot laisse alpha=0 :
// recopié tel quel, ça donnerait des coins noirs sur le panneau clair.
// div255 : équivalent EXACT de x/255 pour x <= 65535, trois divisions
// matérielles par pixel en moins dans la boucle.
static inline uint32_t div255(uint32_t x) { return (x + 1u + (x >> 8)) >> 8; }

static inline uint16_t blend_on_panel(lv_color32_t p, uint8_t br, uint8_t bg_,
                                      uint8_t bb) {
  const uint32_t a = p.alpha;
  const uint32_t ia = 255u - a;
  const uint32_t r = div255(p.red * a + br * ia);
  const uint32_t g = div255(p.green * a + bg_ * ia);
  const uint32_t b = div255(p.blue * a + bb * ia);
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// Capture le bandeau ENTIER (un seul lv_snapshot, pas un par widget comme en
// V1 : ici tout le bandeau est re-blitté à chaque frame de toute façon),
// compose sur kPanel et tourne 90° horaire — même transformation que le flush
// LVGL (rotation 270). Appelée depuis le lv_timer, sous mutex LVGL, hors
// dispatch d'événement : appeler lv_snapshot en réentrance depuis un callback
// d'événement corromprait l'état de rendu interne de LVGL.
static bool band_compose() {
  crumb::mark(crumb::Id::kBandComposeBegin);
  if (s_band == nullptr || s_band_buf[0] == nullptr) {
    crumb::mark(crumb::Id::kBandComposeEnd, 1);
    return false;
  }
  if (s_snap_mem == nullptr) {
    s_snap_mem =
        (uint8_t *)heap_caps_aligned_alloc(64, kSnapMemSize, MALLOC_CAP_SPIRAM);
    if (s_snap_mem == nullptr) {
      ESP_LOGE(TAG, "alloc scratch snapshot (%u o) échouée",
               (unsigned)kSnapMemSize);
      crumb::mark(crumb::Id::kBandComposeEnd, 2);
      return false;
    }
  }

  lv_draw_buf_t snap;
  if (lv_draw_buf_init(&snap, 1, 1, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO,
                       s_snap_mem, kSnapMemSize) != LV_RESULT_OK) {
    crumb::mark(crumb::Id::kBandComposeEnd, 3);
    return false;
  }
  if (lv_snapshot_take_to_draw_buf(s_band, LV_COLOR_FORMAT_ARGB8888, &snap) !=
      LV_RESULT_OK) {
    crumb::mark(crumb::Id::kBandComposeEnd, 4);
    return false;
  }
  crumb::mark(crumb::Id::kBandSnapDone);

  const uint32_t lw = snap.header.w;
  const uint32_t lh = snap.header.h;
  const uint32_t stride = snap.header.stride;
  // Le buffer de destination est dimensionné en dur : refuser une géométrie
  // inattendue plutôt que déborder.
  if (lw != (uint32_t)kScrW || lh != (uint32_t)kBandH) {
    ESP_LOGE(TAG, "snapshot %lux%lu inattendu", (unsigned long)lw,
             (unsigned long)lh);
    crumb::mark(crumb::Id::kBandComposeEnd, 5);
    return false;
  }

  uint16_t *dst = s_band_buf[s_band_next];
  if (s_band_reading.load(std::memory_order_acquire) == dst) {
    // La tâche de rendu blitte ce buffer : recomposer dedans lui ferait sortir
    // un bandeau à moitié neuf. band_timer_cb réarme `dirty` sur false.
    crumb::mark(crumb::Id::kBandComposeEnd, 6);
    return false;
  }
  const lv_color_t panel = lv_color_hex(kPanel);
  // Parcours par tuiles et non ligne par ligne. La transposition écrit à
  // `lx * lh` : 80 px = 160 octets d'écart entre deux itérations consécutives,
  // soit PLUS qu'une ligne de cache (64 o). En balayant une ligne source
  // entière, chacun des 102 400 pixels tombait donc sur une ligne de cache
  // neuve en PSRAM — mesuré à 100-266 ms de mutex LVGL tenu, pendant lesquels
  // la tâche de rendu vidéo restait bloquée dans present_frame() et perdait une
  // frame. Dans une tuile 16x16, les 16 valeurs de lx touchent 16 lignes de
  // cache que les 16 valeurs de ly suivantes réutilisent : ~16x moins de
  // défauts. Le blend lui-même (div255) n'a jamais été le coût.
  constexpr uint32_t kTile = 16;
  static_assert(kScrW % kTile == 0 && kBandH % kTile == 0,
                "tuilage sans reste : les bornes ci-dessous n'en gèrent pas");
  for (uint32_t ly0 = 0; ly0 < lh; ly0 += kTile) {
    for (uint32_t lx0 = 0; lx0 < lw; lx0 += kTile) {
      for (uint32_t ly = ly0; ly < ly0 + kTile; ly++) {
        const lv_color32_t *src =
            (const lv_color32_t *)(snap.data + (size_t)ly * stride);
        uint16_t *dstcol = dst + (lh - 1 - ly);
        for (uint32_t lx = lx0; lx < lx0 + kTile; lx++) {
          dstcol[(size_t)lx * lh] =
              blend_on_panel(src[lx], panel.red, panel.green, panel.blue);
        }
      }
    }
  }

  // Le consommateur n'est plus le CPU (memcpy, cohérent par le cache) mais le
  // DMA du PPA : sans ce writeback il lirait la PSRAM telle qu'elle était avant
  // la composition. Buffer aligné 64 et taille multiple de 64 -> pas de
  // FLAG_UNALIGNED. Coût payé ici (rare : à chaque changement de contrôle), pas
  // dans le chemin chaud.
  crumb::mark(crumb::Id::kBandMsyncBegin);
  esp_cache_msync(dst, kBandPx * sizeof(uint16_t), ESP_CACHE_MSYNC_FLAG_DIR_C2M);

  // release : garantit que les écritures du buffer sont visibles par la tâche
  // de rendu avant qu'elle ne lise le nouveau pointeur.
  s_band_pub.store(dst, std::memory_order_release);
  s_band_next ^= 1;
  crumb::mark(crumb::Id::kBandComposeEnd);
  return true;
}

// Tourne dans lvgl_task (déjà sous LvglLock via lv_timer_handler).
static void band_timer_cb(lv_timer_t *timer) {
  (void)timer;
  gate_refresh(false);
  if (s_answered) {
    const int32_t min =
        (int32_t)((esp_timer_get_time() - s_answered_at_us) / 60000000);
    if (min != s_shown_min) {
      s_shown_min = min;
      state_chip_refresh();
    }
  }
  if (!s_band_dirty.exchange(false, std::memory_order_relaxed)) {
    return;
  }
  // Doigt posé (glissé de curseur) : une recomposition toutes les 150 ms au
  // plus, au lieu d'une par tick de 50 ms. Chacune coûte un lv_snapshot
  // 1280x80 + un blend de 102 400 px dans lvgl_task ; à 20/s le curseur
  // lui-même devenait saccadé. RELEASED repose `dirty` : l'état final est
  // toujours recomposé.
  static int64_t s_last_compose_us = 0;
  const int64_t now = esp_timer_get_time();
  lv_indev_t *indev = bsp::Bsp::instance().touch_indev();
  if (indev != nullptr &&
      lv_indev_get_state(indev) == LV_INDEV_STATE_PRESSED &&
      now - s_last_compose_us < 150000) {
    s_band_dirty.store(true, std::memory_order_relaxed);
    return;
  }
  s_last_compose_us = now;
  if (!band_compose()) {
    // Retenter au prochain tick plutôt que laisser un bandeau périmé.
    s_band_dirty.store(true, std::memory_order_relaxed);
  }
}

static void cb_band_dirty(lv_event_t *e) {
  (void)e;
  s_band_dirty.store(true, std::memory_order_relaxed);
}

static void band_track(lv_obj_t *obj) {
  lv_obj_add_event_cb(obj, cb_band_dirty, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(obj, cb_band_dirty, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(obj, cb_band_dirty, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(obj, cb_band_dirty, LV_EVENT_STYLE_CHANGED, nullptr);
}

// --- Blit (chemin chaud, tâche de rendu MJPEG) ------------------------------

void band_blit() {
  uint16_t *src = s_band_pub.load(std::memory_order_acquire);
  if (src == nullptr) {
    return;
  }
  // Publie la lecture en cours pour que band_compose (tâche LVGL) ne réécrive
  // pas ce buffer sous le DMA du PPA. Deux stores relaxés/release sur le chemin
  // chaud : coût nul devant l'opération PPA elle-même.
  s_band_reading.store(src, std::memory_order_release);
  crumb::mark(crumb::Id::kBandBlitBegin);
  // Recopie par le PPA et non par le CPU. La version memcpy exigeait un
  // esp_cache_msync par ligne (le scan-out DSI lit la PSRAM, pas le cache) :
  // 1280 appels par frame, mesurés à band=20 ms — davantage que le décodage
  // JPEG matériel (11 ms) et poste dominant du chemin chaud. Le DMA du PPA
  // écrit la PSRAM sans passer par le cache : plus un seul msync ici.
  const esp_err_t err = bsp::Bsp::instance().blit_rgb565_to_panel(
      src, (uint32_t)kBandPhysW, (uint32_t)kBandPhysH,
      (uint32_t)s_band_phys.x1, (uint32_t)s_band_phys.y1);
  crumb::mark(crumb::Id::kBandBlitEnd, static_cast<uint32_t>(err));
  s_band_reading.store(nullptr, std::memory_order_release);
  if (err != ESP_OK) {
    // Chemin chaud : jamais de log par frame (saturation UART = lag tactile).
    static uint32_t s_errs = 0;
    if ((++s_errs % 100) == 1) {
      ESP_LOGW(TAG, "blit bandeau : %s", esp_err_to_name(err));
    }
  }
}

// --- API écran --------------------------------------------------------------

void video_zone_physical_rect(uint32_t &x, uint32_t &y, uint32_t &w,
                              uint32_t &h) {
  x = (uint32_t)s_video_zone_phys.x1;
  y = (uint32_t)s_video_zone_phys.y1;
  w = (uint32_t)lv_area_get_width(&s_video_zone_phys);
  h = (uint32_t)lv_area_get_height(&s_video_zone_phys);
}

void prepare_stream() {
  bsp::Bsp::LvglLock lvgl_lock;
  // Nouvelle session : ne pas faire attendre le premier affichage des stats la
  // fin de la fenêtre héritée de la session précédente.
  s_stats_last_us = 0;
  // Une session précédente fermée micro ouvert par un autre chemin que Retour
  // (non-réponse, camera_session::stop) laissait le bouton en « Raccrocher ».
  if (s_answer_btn != nullptr) {
    answer_set_visual(false);
  }
  // Visibilité relue ici et non à la création de l'écran : le réglage s'applique
  // à chaud (page Configuration) et le bandeau est de toute façon recomposé
  // juste en dessous.
  if (s_stats_label != nullptr) {
    // Stats = 2e ligne de la pastille : elle grandit pour la loger, et la
    // ligne d'état remonte. Sans stats, pastille d'une ligne, centrée.
    if (prefs::show_stats()) {
      lv_obj_remove_flag(s_stats_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_height(s_state_chip, kChipTallH);
      lv_obj_align(s_state_lbl, LV_ALIGN_TOP_MID, 0, 6);
    } else {
      lv_obj_add_flag(s_stats_label, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_height(s_state_chip, kChipH);
      lv_obj_center(s_state_lbl);
    }
  }
  gate_refresh(true);
  // Réarme `dirty` si la composition a échoué (buffer en cours de blit, alloc
  // du scratch refusée) : sans ça le bandeau resterait périmé toute la session.
  s_band_dirty.store(!band_compose(), std::memory_order_relaxed);
}

// Appelée 1x/s par la tâche de rendu, en même temps que le log MJPEG.
void update_stats(const char *text) {
  // Recomposer le bandeau pour ce seul label coûte un lv_snapshot 1280x80 + un
  // blend de 102 400 px, mutex LVGL tenu ~100 ms : la tâche de rendu se
  // bloquait dans present_frame() le temps d'une frame vidéo ENTIÈRE, soit un
  // drop par seconde — le compteur de frames coûtait une frame. Fenêtre de 5 s :
  // le compteur reste lisible, le coût devient marginal. Sortie AVANT le mutex,
  // sinon l'appel bloquerait quand même derrière la composition en cours.
  // Overlay masqué (défaut) : aucune recomposition du bandeau, donc aucune
  // frame perdue. C'est la raison d'être du réglage.
  if (!prefs::show_stats()) {
    return;
  }
  constexpr int64_t kStatsPeriodUs = 5000000;
  const int64_t now = esp_timer_get_time();
  if (now - s_stats_last_us < kStatsPeriodUs) {
    return;
  }
  s_stats_last_us = now;

  bsp::Bsp::LvglLock lvgl_lock;
  if (s_stats_label == nullptr) {
    return;
  }
  lv_label_set_text(s_stats_label, text);
  s_band_dirty.store(true, std::memory_order_relaxed);
}

// --- Callbacks --------------------------------------------------------------

// Texte de la pastille d'état. Mutex LVGL requis.
static void state_chip_refresh() {
  char buf[48];
  if (!s_answered) {
    snprintf(buf, sizeof buf, "%s  %s", ui::icon::kMicOff,
             i18n::t(Str::kCamMicOff));
  } else if (s_shown_min <= 0) {
    snprintf(buf, sizeof buf, "%s  %s", ui::icon::kMic,
             i18n::t(Str::kCamLive));
  } else {
    snprintf(buf, sizeof buf, i18n::t(Str::kCamLiveMinutesFmt),
             ui::icon::kMic, (long)s_shown_min);
  }
  lv_label_set_text(s_state_lbl, buf);
  s_band_dirty.store(true, std::memory_order_relaxed);
}

// Le libellé dit ce que fera le PROCHAIN appui, la pastille dit l'état actuel :
// la couleur seule ne suffisait pas à savoir si l'on était décroché.
static void answer_set_visual(bool active) {
  s_answered = active;
  s_answered_at_us = esp_timer_get_time();
  s_shown_min = 0;
  lv_obj_set_style_bg_color(s_answer_btn,
                            lv_color_hex(active ? kMicOn : kAnswer), 0);
  lv_obj_set_style_text_color(s_answer_lbl, lv_color_white(), 0);
  char buf[40];
  snprintf(buf, sizeof buf, "%s  %s", active ? ui::icon::kHangUp : LV_SYMBOL_CALL,
           active ? i18n::t(Str::kCamHangUp)
                  : i18n::t(Str::kCamAnswer));
  lv_label_set_text(s_answer_lbl, buf);
  lv_obj_set_style_bg_color(s_state_chip, lv_color_hex(active ? kLiveBg : kBadge),
                            0);
  lv_obj_set_style_text_color(s_state_lbl, lv_color_hex(active ? kMicOn : kSub),
                              0);
  state_chip_refresh();
}

// Mutex LVGL requis (lvgl_task).
static void gate_refresh(bool force) {
  if (s_gate_btn == nullptr || s_cbs.gate_look == nullptr ||
      s_cbs.gate_version == nullptr) {
    return;
  }
  const uint32_t v = s_cbs.gate_version();
  if (!force && v == s_gate_shown) {
    return;
  }
  s_gate_shown = v;
  const gate_logic::Look l = s_cbs.gate_look();
  if (l.hint[0] != '\0') { // aucune entité configurée : pas de bouton
    lv_obj_add_flag(s_gate_btn, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(s_gate_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_gate_lbl, l.label);
    lv_obj_set_style_bg_color(s_gate_btn,
                              lv_color_hex(ui::gate_tone_color(l.tone)), 0);
    if (l.enabled) {
      lv_obj_remove_state(s_gate_btn, LV_STATE_DISABLED);
    } else {
      lv_obj_add_state(s_gate_btn, LV_STATE_DISABLED);
    }
  }
  // Recomposer coûte un lv_snapshot 1280x80 + un blend dans lvgl_task, et
  // alloue le scratch de 448 Ko : seulement si l'écran caméra est affiché.
  // Sinon prepare_stream() recompose à l'ouverture.
  if (lv_screen_active() == s_scr) {
    s_band_dirty.store(true, std::memory_order_relaxed);
  }
}

static void cb_gate(lv_event_t *e) {
  (void)e;
  if (s_cbs.gate_press != nullptr) {
    (void)s_cbs.gate_press();
  }
  gate_refresh(true);
}

static void cb_back(lv_event_t *e) {
  (void)e;
  if (s_cbs.mic_active()) {
    s_cbs.stop_mic();
    answer_set_visual(false);
  }
  s_cbs.stop_camera();
}

static void cb_toggle_answer(lv_event_t *e) {
  (void)e;
  if (!s_cbs.mic_active()) {
    s_cbs.start_mic();
    answer_set_visual(true);
  } else {
    s_cbs.stop_mic();
    answer_set_visual(false);
  }
}

static void cb_speaker_volume_changed(lv_event_t *e) {
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  // Appliqué à chaque pixel, persisté au relâché seulement : un commit NVS
  // gèle l'autre cœur (drain MJPEG / aud_render) en pleine session.
  bsp::Bsp::instance().set_speaker_volume(
      static_cast<uint8_t>(lv_slider_get_value(slider)),
      lv_event_get_code(e) == LV_EVENT_RELEASED);
}

static void cb_mic_gain_changed(lv_event_t *e) {
  lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
  bsp::Bsp::instance().set_mic_gain(
      static_cast<uint8_t>(lv_slider_get_value(slider)),
      lv_event_get_code(e) == LV_EVENT_RELEASED);
}

// Tâche différée : l'initialisation WebRTC est trop lourde pour le thread LVGL
// et provoquerait un deadlock au clic.
static void camera_v2_deferred_start_task(void *param) {
  (void)param;
  ESP_LOGI(TAG, "Lancement de start_camera_v2() depuis cam2_defer");
  s_cbs.start_camera_v2();
  vTaskDelete(NULL);
}

// --- Construction des widgets -----------------------------------------------

// Fond opaque obligatoire sur chaque widget : lv_snapshot rend un ARGB8888 dont
// l'alpha ne sert qu'aux coins ; un widget transparent ressortirait sur un
// carré noir une fois composé.
static void opaque_bg(lv_obj_t *o, uint32_t color) {
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_outline_width(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
}

// Slider horizontal : LVGL choisit l'orientation par le rapport largeur/hauteur
// (pas d'API d'orientation pour lv_slider).
static lv_obj_t *make_slider(lv_obj_t *parent, int32_t x, int32_t w,
                             uint32_t accent, int32_t value) {
  lv_obj_t *s = lv_slider_create(parent);
  lv_obj_set_size(s, w, 28);
  lv_obj_align(s, LV_ALIGN_LEFT_MID, x, 0);
  lv_slider_set_range(s, 0, 100);
  lv_slider_set_value(s, value, LV_ANIM_OFF);
  opaque_bg(s, kHair);
  lv_obj_set_style_radius(s, 14, 0);
  lv_obj_set_style_bg_color(s, lv_color_hex(accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_obj_set_style_radius(s, 14, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_border_width(s, 2, LV_PART_KNOB);
  lv_obj_set_style_border_color(s, lv_color_hex(accent), LV_PART_KNOB);
  lv_obj_set_style_border_opa(s, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(s, 0, LV_PART_KNOB);
  // pad 0 : un knob plus large que le rail agrandirait la zone capturée par
  // lv_snapshot et cette marge, non peinte, ressortirait en cadre.
  lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
  return s;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int32_t x,
                             int32_t w, uint32_t bg, uint32_t fg,
                             lv_obj_t **out_lbl) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_set_size(b, w, 64);
  lv_obj_align(b, LV_ALIGN_LEFT_MID, x, 0);
  opaque_bg(b, bg);
  lv_obj_set_style_radius(b, 20, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, lv_color_hex(kHair), 0);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t *l = lv_label_create(b);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(fg), 0);
  lv_obj_center(l);
  if (out_lbl != nullptr) {
    *out_lbl = l;
  }
  return b;
}

void create_screen(const ui::Callbacks &cbs) {
  s_cbs = cbs;

  // Buffers du bandeau : 2 x 200 Ko en PSRAM, alignés 64 (lus par le CPU puis
  // recopiés dans un framebuffer DMA). Alloués une fois pour toute la vie du
  // programme — les libérer entre deux sessions fragmenterait la PSRAM sans
  // gain utile.
  for (auto &buf : s_band_buf) {
    buf = (uint16_t *)heap_caps_aligned_alloc(64, kBandPx * sizeof(uint16_t),
                                              MALLOC_CAP_SPIRAM);
    if (buf == nullptr) {
      ESP_LOGE(TAG, "alloc buffer bandeau (%u o) échouée",
               (unsigned)(kBandPx * sizeof(uint16_t)));
      // Rendre le premier buffer : l'écran ne sera pas construit, garder 200 Ko
      // de PSRAM immobilisés pénaliserait les Lottie de l'écran météo.
      for (auto &b : s_band_buf) {
        heap_caps_free(b);
        b = nullptr;
      }
      return;
    }
  }

  s_scr = lv_obj_create(NULL);
  lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(s_scr, lv_color_hex(0x000000), 0);
  lv_obj_set_style_pad_all(s_scr, 0, 0);

  /* Zone vidéo : placeholder de géométrie, jamais de pixels LVGL dedans. Elle
   * couvre tout l'écran — c'est ce qui donne dst_x=0 / dst_w=800 et autorise le
   * chemin zéro-copie du décodeur. */
  lv_obj_t *video_zone = lv_obj_create(s_scr);
  lv_obj_remove_flag(video_zone, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(video_zone, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(video_zone, lv_color_hex(0x000000), 0);
  lv_obj_set_style_border_width(video_zone, 0, 0);
  lv_obj_set_style_pad_all(video_zone, 0, 0);
  lv_obj_set_pos(video_zone, 0, 0);
  lv_obj_set_size(video_zone, kScrW, kScrH);

  /* Attente perçue : la première image met ~3 s (démarrage à froid côté
   * go2rtc) et l'écran restait noir. Jamais masqué : LVGL ne le peint que hors
   * mode direct — avant la première frame, ou sous un bandeau d'erreur —, et le
   * décodeur recouvre tout le framebuffer à chaque image. */
  lv_obj_t *connecting = lv_label_create(video_zone);
  lv_label_set_text(connecting, i18n::t(Str::kCamConnecting));
  lv_obj_set_style_text_font(connecting, &montserrat_fr_36, 0);
  lv_obj_set_style_text_color(connecting, lv_color_hex(kSub), 0);
  lv_obj_align(connecting, LV_ALIGN_CENTER, 0, -kBandH / 2);

  /* Bandeau : conteneur source du snapshot. Tous les contrôles sont ses
   * enfants (coordonnées locales). */
  s_band = lv_obj_create(s_scr);
  lv_obj_remove_flag(s_band, LV_OBJ_FLAG_SCROLLABLE);
  opaque_bg(s_band, kPanel);
  lv_obj_set_style_radius(s_band, 0, 0);
  lv_obj_set_style_pad_all(s_band, 0, 0);
  lv_obj_set_pos(s_band, 0, kBandY);
  lv_obj_set_size(s_band, kScrW, kBandH);

  // Retour | Son | Répondre | état (+ stats) | Micro | Portail — positions
  // logiques sur les 1280 px de large. Répondre à 280 : « Raccrocher » et son
  // icône mesurent ~264 px en 36 px. Les stats vivent dans la pastille
  // d'état depuis que le portail a pris leur place à droite.
  lv_obj_t *back_btn =
      make_button(s_band, LV_SYMBOL_LEFT, 12, 80, kBadge, kInk, nullptr);
  lv_obj_add_event_cb(back_btn, cb_back, LV_EVENT_CLICKED, NULL);

  lv_obj_t *spk_slider =
      make_slider(s_band, 104, 150, kWarm,
                  bsp::Bsp::instance().speaker_volume());
  lv_obj_add_event_cb(spk_slider, cb_speaker_volume_changed,
                      LV_EVENT_VALUE_CHANGED, NULL);
  lv_obj_add_event_cb(spk_slider, cb_speaker_volume_changed, LV_EVENT_RELEASED,
                      NULL);

  s_answer_btn =
      make_button(s_band, "", 266, 280, kAnswer, 0xFFFFFF, &s_answer_lbl);
  lv_obj_add_event_cb(s_answer_btn, cb_toggle_answer, LV_EVENT_CLICKED, NULL);

  s_state_chip = lv_obj_create(s_band);
  lv_obj_remove_flag(s_state_chip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_state_chip, LV_OBJ_FLAG_CLICKABLE);
  opaque_bg(s_state_chip, kBadge);
  lv_obj_set_size(s_state_chip, 290, kChipH);
  lv_obj_set_style_radius(s_state_chip, 24, 0);
  lv_obj_set_style_pad_all(s_state_chip, 0, 0);
  lv_obj_align(s_state_chip, LV_ALIGN_LEFT_MID, 558, 0);
  s_state_lbl = lv_label_create(s_state_chip);
  lv_label_set_long_mode(s_state_lbl, LV_LABEL_LONG_CLIP);
  lv_obj_set_style_text_font(s_state_lbl, &montserrat_fr_24, 0);
  lv_obj_center(s_state_lbl);
  answer_set_visual(false);

  lv_obj_t *mic_slider =
      make_slider(s_band, 860, 146, kCold, bsp::Bsp::instance().mic_gain());
  lv_obj_add_event_cb(mic_slider, cb_mic_gain_changed, LV_EVENT_VALUE_CHANGED,
                      NULL);
  lv_obj_add_event_cb(mic_slider, cb_mic_gain_changed, LV_EVENT_RELEASED, NULL);

  // Stats : 2e ligne de la pastille d'état (prepare_stream la dimensionne).
  s_stats_label = lv_label_create(s_state_chip);
  lv_obj_set_width(s_stats_label, 270);
  lv_label_set_long_mode(s_stats_label, LV_LABEL_LONG_CLIP);
  lv_label_set_text(s_stats_label, "0");
  lv_obj_set_style_text_font(s_stats_label, &montserrat_fr_16, 0);
  lv_obj_set_style_text_color(s_stats_label, lv_color_hex(kSub), 0);
  lv_obj_set_style_text_align(s_stats_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_stats_label, LV_ALIGN_BOTTOM_MID, 0, -4);

  // Portail : libellé en 24 px sur deux lignes au plus (« Portail
  // indisponible » = 237 px), le bouton garde la hauteur de Répondre.
  s_gate_btn = make_button(s_band, "", 1018, 250, kBadge, 0xFFFFFF, &s_gate_lbl);
  lv_obj_set_width(s_gate_lbl, 230);
  lv_label_set_long_mode(s_gate_lbl, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_font(s_gate_lbl, &montserrat_fr_24, 0);
  lv_obj_set_style_text_align(s_gate_lbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(s_gate_lbl);
  lv_obj_set_style_bg_opa(s_gate_btn, LV_OPA_60, LV_STATE_DISABLED);
  lv_obj_add_flag(s_gate_btn, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(s_gate_btn, cb_gate, LV_EVENT_CLICKED, NULL);
  // Masqué tant que prepare_stream n'a pas relu le réglage : l'état visible est
  // celui de la dernière session, jamais celui de la création de l'écran.
  lv_obj_add_flag(s_stats_label, LV_OBJ_FLAG_HIDDEN);

  band_track(back_btn);
  band_track(spk_slider);
  band_track(s_answer_btn);
  band_track(mic_slider);
  band_track(s_stats_label);
  band_track(s_gate_btn);

  lv_obj_update_layout(video_zone);
  lv_obj_get_coords(video_zone, &s_video_zone_phys);
  lv_display_rotate_area(lv_display_get_default(), &s_video_zone_phys);
  lv_obj_update_layout(s_band);
  lv_obj_get_coords(s_band, &s_band_phys);
  lv_display_rotate_area(lv_display_get_default(), &s_band_phys);
  ESP_LOGI(TAG, "video zone physical: x=[%d,%d] y=[%d,%d]",
           (int)s_video_zone_phys.x1, (int)s_video_zone_phys.x2,
           (int)s_video_zone_phys.y1, (int)s_video_zone_phys.y2);
  ESP_LOGI(TAG, "band physical: x=[%d,%d] y=[%d,%d]", (int)s_band_phys.x1,
           (int)s_band_phys.x2, (int)s_band_phys.y1, (int)s_band_phys.y2);

  lv_timer_create(band_timer_cb, 50, NULL);
}

void open_screen() {
  // s_scr reste nul si create_screen a renoncé (alloc des buffers de bandeau
  // échouée) : lv_scr_load(nullptr) déréférence, la page devient un crash au
  // lieu d'une page indisponible.
  if (s_scr == nullptr) {
    ESP_LOGE(TAG, "écran V2 non construit : ouverture ignorée");
    return;
  }
  {
    bsp::Bsp::LvglLock lvgl_lock;
    lv_scr_load(s_scr);
  }
  if (xTaskCreate(camera_v2_deferred_start_task, "cam2_defer", 8192, NULL, 4,
                  NULL) != pdPASS) {
    // Sans cette tâche personne n'ouvre la session : l'écran resterait noir
    // sans le moindre message.
    ESP_LOGE(TAG, "création cam2_defer échouée : session non démarrée");
  }
}

void open_screen_from_event() {
  // Contexte callback LVGL : mutex déjà tenu (ne pas reprendre le LvglLock).
  if (s_scr == nullptr) {
    ESP_LOGE(TAG, "écran V2 non construit : ouverture ignorée");
    return;
  }
  lv_scr_load(s_scr);
  if (xTaskCreate(camera_v2_deferred_start_task, "cam2_defer", 8192, NULL, 4,
                  NULL) != pdPASS) {
    // Sans cette tâche personne n'ouvre la session : l'écran resterait noir
    // sans le moindre message.
    ESP_LOGE(TAG, "création cam2_defer échouée : session non démarrée");
  }
}

} // namespace ui_camera_v2
