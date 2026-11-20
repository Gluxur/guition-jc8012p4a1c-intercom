#include "bsp.h"

#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h" // sdmmc_card_init : init de la carte sans VFS (mode clé USB)
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_jd9365.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_touch_gsl3680.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "ppa_srm_quirk.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace bsp {

// Unnamed namespace: gives everything inside internal linkage, i.e. only
// visible in this translation unit (.cpp). Closest C# equivalent is
// `internal` / a file-private class, but C# has no true per-file scoping
// — this is stronger, it's invisible even to other files in the same
// component.
namespace {

constexpr char TAG[] = "bsp";

constexpr char kAudioPrefsNamespace[] = "audio_cfg";
constexpr char kSpeakerVolKey[] = "spk_vol";
constexpr char kMicGainKey[] = "mic_gain";
constexpr char kBacklightKey[] = "backlight";
// Recovery floor for the persisted brightness. A near-zero value read back at
// boot would blank the panel with no on-screen way to raise it again.
constexpr uint8_t kBacklightMinPct = 10;

constexpr int kLcdBacklightGpio = 23;
// gpio_num_t et non int : depuis ESP-IDF 6.0 esp_lcd_panel_dev_config_t typé le
// champ reset_gpio_num, et le C++ refuse la conversion implicite depuis int.
constexpr gpio_num_t kLcdResetGpio = GPIO_NUM_27;
constexpr ledc_timer_t kLedcTimer = LEDC_TIMER_0;
constexpr ledc_channel_t kLedcChannel = LEDC_CHANNEL_0;

/* Compteurs de perf LVGL. Écrits uniquement depuis lvgl_task (handle_flush et
 * les callbacks REFR_*), donc pas d'atomics. Un ESP_LOGI/s au plus, et
 * seulement si un rafraîchissement a eu lieu : muet sur une UI immobile. */
struct LvglPerf {
  int64_t refr_start_us = 0;
  int64_t refr_us = 0;
  int64_t flush_us = 0;
  uint32_t refr_cnt = 0;
  uint32_t flush_cnt = 0;
  uint32_t flush_px = 0;
  /* Pic sur la seconde : c'est la trame la PLUS chère qui décide de la
   * fluidité d'une transition (repaint plein écran), pas la moyenne, noyée
   * par les petits rafraîchissements d'horloge ou de VU-mètre. */
  uint32_t cur_px = 0;
  uint32_t peak_px = 0;
  int64_t peak_us = 0;
  int64_t last_log_us = 0;
};
LvglPerf lvgl_perf;

void lvgl_refr_start_cb(lv_event_t *e) {
  (void)e;
  lvgl_perf.refr_start_us = esp_timer_get_time();
  lvgl_perf.cur_px = 0;
}

void lvgl_refr_ready_cb(lv_event_t *e) {
  (void)e;
  /* Solde la dernière bande, laissée en vol par handle_flush. Hors d'un cycle
   * de rafraîchissement, plus aucune transaction du flush ne touche le
   * framebuffer : c'est cet invariant qui permet à la tâche de rendu vidéo de
   * basculer les buffers DSI sans se soucier du PPA de LVGL. Le temps mesuré
   * ici est donc bien celui qui reste après recouvrement — pas une attente
   * cachée hors des compteurs. */
  Bsp::instance().ppa_lvgl_wait_pending();
  const int64_t now_us = esp_timer_get_time();
  const int64_t cur_us = now_us - lvgl_perf.refr_start_us;
  lvgl_perf.refr_us += cur_us;
  ++lvgl_perf.refr_cnt;
  if (lvgl_perf.cur_px > lvgl_perf.peak_px) {
    lvgl_perf.peak_px = lvgl_perf.cur_px;
    lvgl_perf.peak_us = cur_us;
  }
  if (now_us - lvgl_perf.last_log_us < 1000000) {
    return;
  }
  const double refr_ms = lvgl_perf.refr_us / 1000.0;
  const double flush_ms = lvgl_perf.flush_us / 1000.0;
  ESP_LOGD(TAG,
           "LVGL: %lu refr/s, %.1f ms/refr | flush %.1f ms (%lu bd, %lu kpx) | "
           "render %.1f ms | pic %lu kpx en %.1f ms",
           (unsigned long)lvgl_perf.refr_cnt, refr_ms / lvgl_perf.refr_cnt,
           flush_ms / lvgl_perf.refr_cnt, (unsigned long)lvgl_perf.flush_cnt,
           (unsigned long)(lvgl_perf.flush_px / 1000u),
           (refr_ms - flush_ms) / lvgl_perf.refr_cnt,
           (unsigned long)(lvgl_perf.peak_px / 1000u),
           lvgl_perf.peak_us / 1000.0);
  lvgl_perf.refr_us = 0;
  lvgl_perf.flush_us = 0;
  lvgl_perf.refr_cnt = 0;
  lvgl_perf.flush_cnt = 0;
  lvgl_perf.flush_px = 0;
  lvgl_perf.peak_px = 0;
  lvgl_perf.peak_us = 0;
  lvgl_perf.last_log_us = now_us;
}

constexpr int kMipiPhyLdoChan = 3;
constexpr int kMipiPhyLdoMv = 2500;

// Le connecteur TF est alimenté par ESP_LDO_VO4 (schéma 4_CONN) ; le canal 3
// est déjà pris par le PHY MIPI ci-dessus. Les broches (D0..D3=39..42,
// CLK=43, CMD=44) sont celles du slot 0, câblées par l'IOMUX : elles ne se
// déclarent pas. Le slot 1 porte la liaison Wi-Fi esp_hosted.
constexpr int kSdLdoChan = 4;
constexpr char kSdMountPoint[] = "/sdcard";

constexpr int kMipiLaneNum = 2;
constexpr int kMipiLaneBitrateMbps = 1500;
constexpr int kMipiDpiClkMhz = 40;
constexpr int kLcdHsync = 20;
constexpr int kLcdHbp = 20;
constexpr int kLcdHfp = 40;
constexpr int kLcdVsync = 4;
constexpr int kLcdVbp = 8;
/* NE PAS RÉDUIRE. Le driver DSI redémarre le DMA par logiciel à chaque trame
 * (mipi_dsi_dma_trans_done_cb) et la fenêtre disponible est le blanking
 * vertical. À vfp=20 elle ne valait que 704 µs : ratée toutes les 5 à 20 s, la
 * trame suivante démarrait un cycle trop tard, le panneau tenait la même image
 * 57 ms au lieu de 29 -> chute de luminance -> scintillement permanent.
 * vfp=120 porte la fenêtre à 2,9 ms : plus aucune perte mesurée.
 * Contrepartie assumée : refresh = 40e6/(880×1412) = 32,2 Hz.
 * Augmenter dpi_clk sans augmenter vfp d'autant RAMÈNE le bug (la fenêtre est
 * en µs, pas en lignes). Cf. docs/hardware-notes.md (Display). */
constexpr int kLcdVfp = 120;

constexpr i2c_port_t kTouchI2cPort = I2C_NUM_0;
constexpr int kTouchSdaGpio = 7;
constexpr int kTouchSclGpio = 8;
constexpr int kTouchRstGpio = 22;
constexpr int kTouchIntGpio = 21;
// 400 kHz et non 100 : le chargement du firmware tactile (4587 écritures) passe
// de 2,61 à 0,81 s au boot, et chaque lecture (24 o sous mutex LVGL, une par
// cycle indev) raccourcit d'autant. Mesuré le 2026-11-07 : mutex LVGL moyen en
// session 4,3 -> 2,9 ms, startup_chip vérifié, aucune erreur I2C. Vitesse
// propre à ce périphérique : l'ES8311 sur le même bus garde la sienne.
constexpr uint32_t kTouchI2cFreq = 400000;
constexpr uint8_t kTouchI2cAddr = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS;
constexpr int kTouchMaxPoints = 5;

constexpr int kAudioLrclkGpio = 10;
constexpr int kAudioBclkGpio = 12;
constexpr int kAudioMclkGpio = 13;
constexpr int kAudioDoutGpio = 9;
constexpr int kAudioDinGpio = 11;
constexpr uint32_t kAudioSampleRate = 8000;
// Trames stéréo lues d'un coup par read_pcm_ref : un descripteur DMA I2S
// (dma_frame_num par défaut).
constexpr size_t kRxRefChunk = 240;

void log_i2c_scan(i2c_master_bus_handle_t bus) {
  char found[160] = {0};
  size_t used = 0;

  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
      int written = snprintf(found + used, sizeof(found) - used, "%s0x%02X",
                             used ? " " : "", addr);
      if (written > 0) {
        used += static_cast<size_t>(written);
      }
      if (used >= sizeof(found)) {
        break;
      }
    }
  }

  ESP_LOGI(TAG, "I2C scan SDA=%d SCL=%d: %s", kTouchSdaGpio, kTouchSclGpio,
           used ? found : "no device found");
}

/* Identification du capteur caméra branché sur le connecteur MIPI-CSI. Le scan
 * voit deux esclaves que le BSP ne pilote pas (0x32, 0x36) ; 0x36 est l'adresse
 * SCCB des OmniVision. On lit les registres d'ID connus de chaque famille au
 * lieu de deviner : chaque fabricant a les siens, et un capteur qui n'a pas
 * d'horloge XCLK peut acquitter son adresse sans répondre aux lectures (d'où
 * le code d'erreur loggé plutôt qu'une valeur inventée). Lecture seule. */
void log_camera_sensor_probe(i2c_master_bus_handle_t bus) {
  struct Reg16 {
    uint16_t reg;
    const char *what;
  };
  // Registres 16 bits (SCCB OmniVision / SmartSens / Sony).
  static constexpr Reg16 kId16[] = {
      {0x300A, "OV id hi"}, {0x300B, "OV id lo"}, {0x300C, "OV id ext"},
      {0x3107, "SC id hi"}, {0x3108, "SC id lo"}, {0x0016, "IMX id hi"},
      {0x0017, "IMX id lo"},
  };
  static constexpr uint8_t kCandidates[] = {0x32, 0x36};

  for (uint8_t addr : kCandidates) {
    if (i2c_master_probe(bus, addr, 20) != ESP_OK) {
      continue;
    }
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = addr;
    dev_cfg.scl_speed_hz = 100000;
    i2c_master_dev_handle_t dev = nullptr;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
      ESP_LOGE(TAG, "cam probe 0x%02X: add_device failed", addr);
      continue;
    }

    char line[200] = {0};
    size_t used = 0;
    for (const Reg16 &r : kId16) {
      const uint8_t wr[2] = {static_cast<uint8_t>(r.reg >> 8),
                             static_cast<uint8_t>(r.reg & 0xFF)};
      uint8_t val = 0;
      esp_err_t err = i2c_master_transmit_receive(dev, wr, sizeof(wr), &val,
                                                  1, 20);
      int n = (err == ESP_OK)
                  ? snprintf(line + used, sizeof(line) - used, " %04X=%02X",
                             r.reg, val)
                  : snprintf(line + used, sizeof(line) - used, " %04X=err%d",
                             r.reg, static_cast<int>(err));
      if (n > 0 && used + static_cast<size_t>(n) < sizeof(line)) {
        used += static_cast<size_t>(n);
      }
    }
    ESP_LOGI(TAG, "cam probe 0x%02X reg16:%s", addr, line);

    // Registres 8 bits : GalaxyCore (0xF0/0xF1) et dump des 16 premiers pour
    // reconnaître un périphérique qui ne serait pas un capteur (RTC, PMIC...).
    used = 0;
    line[0] = '\0';
    static constexpr uint8_t kId8[] = {0xF0, 0xF1, 0x00, 0x01, 0x02, 0x03,
                                       0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                       0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F};
    for (uint8_t reg : kId8) {
      uint8_t val = 0;
      esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, &val, 1, 20);
      int n = (err == ESP_OK)
                  ? snprintf(line + used, sizeof(line) - used, " %02X=%02X",
                             reg, val)
                  : snprintf(line + used, sizeof(line) - used, " %02X=err",
                             reg);
      if (n > 0 && used + static_cast<size_t>(n) < sizeof(line)) {
        used += static_cast<size_t>(n);
      }
    }
    ESP_LOGI(TAG, "cam probe 0x%02X reg8:%s", addr, line);

    if (i2c_master_bus_rm_device(dev) != ESP_OK) {
      ESP_LOGW(TAG, "cam probe 0x%02X: rm_device failed", addr);
    }
  }
}

/* Fin de trame DSI. Sur ce silicium (pré-v3) l'événement VSYNC du bridge
 * n'existe pas : le driver appelle ce callback depuis mipi_dsi_dma_trans_done_
 * cb, c.-à-d. juste après avoir relancé le DMA sur `cur_fb_index` — l'instant
 * exact où la bascule demandée par draw_bitmap devient effective. Contexte ISR:
 * IRAM_ATTR obligatoire, et uniquement des API ...FromISR(). */
IRAM_ATTR bool dsi_refresh_done_cb(esp_lcd_panel_handle_t,
                                   esp_lcd_dpi_panel_event_data_t *,
                                   void *user_ctx) {
  auto sem = static_cast<SemaphoreHandle_t>(user_ctx);
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(sem, &woken);
  return woken == pdTRUE;
}

/* Fin de transaction PPA du client LVGL. Contexte ISR (appelée depuis l'ISR
 * DMA2D) : IRAM_ATTR obligatoire, uniquement des API ...FromISR(). Le
 * sémaphore arrive par le champ user_data de l'opération. */
IRAM_ATTR bool ppa_lvgl_done_cb(ppa_client_handle_t, ppa_event_data_t *,
                                void *user_data) {
  auto sem = static_cast<SemaphoreHandle_t>(user_data);
  if (sem == nullptr) {
    return false;
  }
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(sem, &woken);
  return woken == pdTRUE;
}

} // namespace

Bsp &Bsp::instance() {
  // Function-local static: constructed exactly once, on first call, and
  // its destructor runs at program exit (in an embedded/ESP-IDF app that
  // in practice never happens). This is the classic C++ singleton — no
  // heap allocation, no explicit `new`, the object lives in static
  // storage. There is no C# equivalent construct; the nearest is a
  // `Lazy<Bsp>` field, but that needs an explicit backing field + property.
  static Bsp inst;
  return inst;
}

uint32_t Bsp::tick_cb() {
  return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// static_cast<Bsp*> here is the C++ typed alternative to a C-style cast:
// LVGL hands back a `void*` (the `this` pointer we stashed via
// lv_display_set_user_data at init time — see init_lvgl); static_cast
// re-attaches the type the compiler needs to call the member function.
// It does no runtime check (unlike a C# `as`/cast on a reference type) —
// if the wrong pointer were ever stored, this would silently misbehave,
// not throw. This is the "trampoline" mentioned in bsp.h: recovers `this`
// so the static C-callback can dispatch to real instance methods.
void Bsp::flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
  static_cast<Bsp *>(lv_display_get_user_data(disp))
      ->handle_flush(disp, area, px_map);
}

void Bsp::touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
  static_cast<Bsp *>(lv_indev_get_user_data(indev))->handle_touch_read(data);
}

void Bsp::handle_flush(lv_display_t *disp, const lv_area_t *area,
                       uint8_t *px_map) {
  if (direct_mode_.load(std::memory_order_relaxed)) {
    lv_display_flush_ready(disp);
    return;
  }

  const int64_t flush_start_us = esp_timer_get_time();
  lv_display_rotation_t rot = lv_display_get_rotation(disp);
  if (rot == LV_DISPLAY_ROTATION_0 || rot_buf_ == nullptr ||
      ppa_lvgl_ == nullptr) {
    /* Chemin sans PPA : la transaction éventuellement en vol vise le même
     * framebuffer, il faut la laisser finir avant d'y recopier en CPU. */
    ppa_lvgl_wait_pending();
    esp_lcd_panel_draw_bitmap(panel_, area->x1, area->y1, area->x2 + 1,
                              area->y2 + 1, px_map);
  } else {
    /* Mode PARTIAL : `area` est l'aire invalidée, pas l'écran entier. Le PPA ne
     * tourne donc que ce qui a réellement changé — la variante RENDER_MODE_FULL
     * essayée en 35cec64 tournait 1 Mpx (42,6 ms !) pour un simple label, et le
     * tearing qu'elle prétendait corriger venait en fait des pertes de trame
     * DSI (cf. docs/hardware-notes.md, corrigées par kLcdVfp).
     * 270° CCW = 90° CW : même transform que l'ancienne boucle logicielle. */
    uint32_t w = static_cast<uint32_t>(lv_area_get_width(area));
    uint32_t h = static_cast<uint32_t>(lv_area_get_height(area));

    lv_area_t phy = *area;
    lv_display_rotate_area(disp, &phy);

    /* Sortie PPA = DIRECTEMENT le framebuffer scanné, pas un buffer relais.
     * L'ancien chemin (PPA -> rot_buf_ -> esp_lcd_panel_draw_bitmap) payait
     * deux fois : la recopie CPU ligne par ligne PSRAM->PSRAM, et surtout le
     * esp_cache_msync que dpi_panel_draw_bitmap fait sur
     * (y_end - y_start) * h_pixels — or après rotation 270° une bande LVGL
     * couvre TOUTE la hauteur physique (1280), donc chaque flush réécrivait en
     * cache les 2 Mo du framebuffer entier. 7 bandes par repaint plein écran =
     * ~14 Mo de writeback : c'est ce qui rendait les transitions d'écran
     * saccadées (2-3 trames affichées sur les 300 ms d'anim). */
    void *fb = dsi_shown_frame_buffer();

    ppa_srm_oper_config_t srm = {};
    srm.in.buffer = px_map;
    srm.in.pic_w = w;
    srm.in.pic_h = h;
    srm.in.block_w = w;
    srm.in.block_h = h;
    srm.in.block_offset_x = 0;
    srm.in.block_offset_y = 0;
    srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    if (fb != nullptr) {
      srm.out.buffer = fb;
      srm.out.buffer_size =
          static_cast<size_t>(kLcdHRes) * kLcdVRes * sizeof(uint16_t);
      srm.out.pic_w =
          kLcdHRes; /* géométrie physique du panneau, pas la bande */
      srm.out.pic_h = kLcdVRes;
      srm.out.block_offset_x = static_cast<uint32_t>(phy.x1);
      srm.out.block_offset_y = static_cast<uint32_t>(phy.y1);
    } else {
      /* Repli si esp_lcd_dpi_panel_get_frame_buffer a échoué au boot. */
      srm.out.buffer = rot_buf_;
      srm.out.buffer_size = rot_buf_size_;
      srm.out.pic_w = h; /* après 270° CCW : largeur ← h, hauteur ← w */
      srm.out.pic_h = w;
      srm.out.block_offset_x = 0;
      srm.out.block_offset_y = 0;
    }
    srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_270;
    srm.scale_x = 1.0f;
    srm.scale_y = 1.0f;
    srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;

    /* Contournement du bug matériel DIG-734 : ppa_srm.c décide d'activer
     * bypass_mb_order d'après la géométrie AVANT rotation, alors qu'en 270° la
     * sortie fait `h` de large et `w` de haut. Certaines bandes — dont 224x68,
     * le bouton Semaine/Mois de l'Agenda — laissent alors un dernier bloc plus
     * petit que la FIFO 2D-DMA : elle n'est jamais vidée, le canal RX ne voit
     * pas d'EOF, la transaction ne finit JAMAIS. Le sémaphore du moteur SRM
     * n'étant rendu que par l'ISR de fin, c'est tout le PPA (client vidéo
     * compris) qui meurt jusqu'au redémarrage. On découpe donc la bande en
     * sous-bandes que le driver traite correctement. Détail et tests :
     * components/bsp/ppa_srm_quirk.h, test/test_ppa_srm_quirk.cpp. */
    uint32_t heights[ppa_quirk::kMaxSplitParts];
    heights[0] = h;
    uint32_t parts = 1;
    if (fb != nullptr) {
      parts = ppa_quirk::rot270_split(w, h, heights, ppa_quirk::kMaxSplitParts);
      if (parts == 0) {
        /* Aucune découpe en hauteur ne convient (bandes très étroites). On
         * soumet quand même : une bande risquée vaut mieux qu'une bande jamais
         * peinte, et le log ci-dessous nomme la géométrie si elle bloque. */
        static bool s_warned = false;
        if (!s_warned) {
          s_warned = true;
          ESP_LOGW(TAG, "bande %lux%lu sans découpe PPA sûre", (unsigned long)w,
                   (unsigned long)h);
        }
        heights[0] = h;
        parts = 1;
      }
    }

    esp_err_t ppa_err = ESP_OK;
    uint32_t row = 0;
    for (uint32_t i = 0; i < parts && ppa_err == ESP_OK; ++i) {
      if (parts > 1) {
        /* L'image d'entrée reste la bande complète (in.pic_w/pic_h) : seuls le
         * bloc lu et sa destination bougent. La destination passe par
         * lv_display_rotate_area comme la bande entière — en 270° une découpe
         * en lignes devient une découpe en colonnes physiques. */
        lv_area_t sub = *area;
        sub.y1 = area->y1 + static_cast<int32_t>(row);
        sub.y2 = sub.y1 + static_cast<int32_t>(heights[i]) - 1;
        lv_area_t phy_sub = sub;
        lv_display_rotate_area(disp, &phy_sub);
        srm.in.block_h = heights[i];
        srm.in.block_offset_y = row;
        srm.out.block_offset_x = static_cast<uint32_t>(phy_sub.x1);
        srm.out.block_offset_y = static_cast<uint32_t>(phy_sub.y1);
      }
      ppa_err = ppa_lvgl_run(srm, w, heights[i]);
      row += heights[i];
    }
    if (ppa_err != ESP_OK) {
      /* Une seule ligne sur 100 : après une transaction perdue, le driver rend
       * ESP_FAIL à CHAQUE bande et logge déjà de son côté ("exceed maximum
       * pending transactions"). Deux lignes par flush depuis le chemin chaud
       * LVGL saturent l'UART et étranglent le drain TCP du flux MJPEG — la
       * panne d'affichage se doublait alors d'une panne de débit. */
      static uint32_t fail_n = 0;
      if (ppa_err != ESP_ERR_TIMEOUT && (fail_n++ % 100) == 0) {
        ESP_LOGW(TAG, "PPA SRM failed: %s (x%lu)", esp_err_to_name(ppa_err),
                 (unsigned long)fail_n);
      }
      lv_display_flush_ready(disp);
      return;
    }

    /* fb != nullptr : le PPA (DMA) dépose les pixels directement dans le
     * framebuffer et le driver PPA invalide la fenêtre côté cache CPU. Le DMA
     * DSI relit la PSRAM : rien à recopier ni à synchroniser, et rien à
     * attendre non plus — la dernière transaction reste en vol, elle sera
     * soldée par le flush suivant ou par la fin du cycle (REFR_READY).
     * fb == nullptr : le repli recopie rot_buf_ en CPU, il faut donc que la
     * rotation soit terminée. */
    if (fb == nullptr) {
      ppa_lvgl_wait_pending();
      esp_lcd_panel_draw_bitmap(panel_, phy.x1, phy.y1, phy.x2 + 1, phy.y2 + 1,
                                rot_buf_);
    }
  }

  /* Part "flush" du cycle de rafraîchissement ; le total est mesuré par les
   * callbacks REFR_START/REFR_READY (cf. lvgl_perf). La différence des deux
   * isole le rendu logiciel LVGL, seul poste que le flush ne couvre pas. */
  lvgl_perf.flush_us += esp_timer_get_time() - flush_start_us;
  const uint32_t band_px = static_cast<uint32_t>(lv_area_get_width(area)) *
                           static_cast<uint32_t>(lv_area_get_height(area));
  lvgl_perf.flush_px += band_px;
  lvgl_perf.cur_px += band_px;
  ++lvgl_perf.flush_cnt;
  lv_display_flush_ready(disp);
}

void Bsp::handle_touch_read(lv_indev_data_t *data) {
  if (!touch_) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  static uint32_t s_call_count = 0;
  if (s_call_count++ == 0) {
    ESP_LOGI(TAG, "touch_read_cb: handle=%p read_data=%p", touch_,
             touch_ ? reinterpret_cast<void *>(touch_->read_data) : nullptr);
  }

  esp_err_t err = esp_lcd_touch_read_data(touch_);
  if (err != ESP_OK) {
    static uint32_t s_read_err_count = 0;
    if (++s_read_err_count <= 5) {
      ESP_LOGW(TAG, "GSL3680 read failed: %s", esp_err_to_name(err));
    }
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  esp_lcd_touch_point_data_t points[kTouchMaxPoints];
  uint8_t point_count = 0;
  err = esp_lcd_touch_get_data(touch_, points, &point_count, kTouchMaxPoints);
  if (err != ESP_OK || point_count == 0) {
    // Doigt levé : le prochain appui redevient un appui ordinaire.
    touch_wake_swallow_ = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  // Panneau éteint : ce toucher RALLUME, et rien d'autre. Le laisser remonter à
  // LVGL cliquerait le bouton qui se trouve sous le doigt, sur un écran que
  // l'utilisateur ne voyait pas. Le verrou tient jusqu'au relâchement, sinon la
  // suite du même appui produirait le clic qu'on vient d'éviter.
  // Coût : deux écritures de registre LEDC, aucune NVS — ce callback est un
  // chemin chaud (règle CLAUDE.md), d'où l'absence de log ici.
  if (touch_wake_enabled_ && !display_on_.load(std::memory_order_relaxed)) {
    restore_backlight();
    touch_wake_swallow_ = true;
  }
  if (touch_wake_swallow_) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }

  data->point.x = points[0].x;
  data->point.y = points[0].y;
  data->state = LV_INDEV_STATE_PRESSED;

  ESP_LOGD(TAG, "touch GSL3680: points=%u x=%u y=%u id=%u", point_count,
           points[0].x, points[0].y, points[0].track_id);
}

esp_err_t Bsp::init() {
  // Read before the first backlight write: loaded from init_audio (further
  // down), the persisted value would land after set_backlight() and the panel
  // would flash at 100 % on every boot.
  load_prefs();
  ESP_RETURN_ON_ERROR(init_panel(), TAG, "panel init failed");
  ESP_RETURN_ON_ERROR(init_lvgl(), TAG, "LVGL init failed");
  // Appliquer seulement : la valeur sort de la NVS, la réécrire coûtait un
  // commit flash à chaque démarrage.
  apply_backlight_duty(backlight_pct_);
  ESP_RETURN_ON_ERROR(init_touch(), TAG, "touch init failed");

  const esp_err_t audio_err = init_audio();
  if (audio_err != ESP_OK) {
    ESP_LOGW(TAG, "audio init failed (%s), speaker disabled",
             esp_err_to_name(audio_err));
  }

  ESP_LOGI(TAG,
           "BSP init OK: panel %dx%d (logique 1280x800 paysage), touch=%s, "
           "audio=%s",
           (int)kLcdHRes, (int)kLcdVRes, touch_ ? "GSL3680" : "disabled",
           audio_codec_ ? "ES8311" : "disabled");
  return ESP_OK;
}

// Note on error handling here and in the other init_* functions:
// ESP_RETURN_ON_ERROR is a macro that logs and does an early `return` on
// failure — the C equivalent of C#'s exception unwinding, but with a key
// difference: nothing runs automatically to release resources already
// acquired earlier in the function (no stack unwind + finally/dispose).
// `ldo_mipi_phy` below, for instance, is never released if a later
// ESP_RETURN_ON_ERROR bails out. In C# a `using`/try-finally chain would
// handle this for free; in C++ it needs RAII wrappers (see LvglLock in
// bsp.h) or manual `goto cleanup`, neither of which is used for this
// function's local handles.
esp_err_t Bsp::init_panel() {
  esp_ldo_channel_handle_t ldo_mipi_phy = nullptr;
  esp_ldo_channel_config_t ldo_cfg = {};
  ldo_cfg.chan_id = kMipiPhyLdoChan;
  ldo_cfg.voltage_mv = kMipiPhyLdoMv;
  ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo_mipi_phy), TAG,
                      "LDO acquire");

  ledc_timer_config_t ledc_timer = {};
  ledc_timer.speed_mode = LEDC_LOW_SPEED_MODE;
  ledc_timer.timer_num = kLedcTimer;
  ledc_timer.duty_resolution = LEDC_TIMER_13_BIT;
  ledc_timer.freq_hz = 5000;
  ledc_timer.clk_cfg = LEDC_AUTO_CLK;
  ESP_RETURN_ON_ERROR(ledc_timer_config(&ledc_timer), TAG, "LEDC timer");

  ledc_channel_config_t ledc_ch = {};
  ledc_ch.gpio_num = kLcdBacklightGpio;
  ledc_ch.speed_mode = LEDC_LOW_SPEED_MODE;
  ledc_ch.channel = kLedcChannel;
  ledc_ch.timer_sel = kLedcTimer;
  ledc_ch.duty = 0;
  ledc_ch.hpoint = 0;
  ESP_RETURN_ON_ERROR(ledc_channel_config(&ledc_ch), TAG, "LEDC channel");

  esp_lcd_dsi_bus_handle_t mipi_dsi_bus = nullptr;
  esp_lcd_dsi_bus_config_t bus_cfg = {};
  bus_cfg.bus_id = 0;
  bus_cfg.num_data_lanes = kMipiLaneNum;
  bus_cfg.lane_bit_rate_mbps = kMipiLaneBitrateMbps;
  ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &mipi_dsi_bus), TAG,
                      "DSI bus");

  esp_lcd_panel_io_handle_t mipi_dbi_io = nullptr;
  esp_lcd_dbi_io_config_t dbi_cfg = {};
  dbi_cfg.virtual_channel = 0;
  dbi_cfg.lcd_cmd_bits = 8;
  dbi_cfg.lcd_param_bits = 8;
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_cfg, &mipi_dbi_io), TAG,
      "DBI IO");

  esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.virtual_channel = 0;
  dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = kMipiDpiClkMhz;
  dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
  dpi_cfg.num_fbs = 2;
  dpi_cfg.video_timing.h_size = kLcdHRes;
  dpi_cfg.video_timing.v_size = kLcdVRes;
  dpi_cfg.video_timing.hsync_pulse_width = kLcdHsync;
  dpi_cfg.video_timing.hsync_back_porch = kLcdHbp;
  dpi_cfg.video_timing.hsync_front_porch = kLcdHfp;
  dpi_cfg.video_timing.vsync_pulse_width = kLcdVsync;
  dpi_cfg.video_timing.vsync_back_porch = kLcdVbp;
  dpi_cfg.video_timing.vsync_front_porch = kLcdVfp;

  jd9365_vendor_config_t vendor_cfg = {};
  vendor_cfg.mipi_config.dsi_bus = mipi_dsi_bus;
  vendor_cfg.mipi_config.dpi_config = &dpi_cfg;

  esp_lcd_panel_dev_config_t panel_dev_cfg = {};
  panel_dev_cfg.reset_gpio_num = kLcdResetGpio;
  panel_dev_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_dev_cfg.bits_per_pixel = 16;
  panel_dev_cfg.vendor_config = &vendor_cfg;

  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_jd9365(mipi_dbi_io, &panel_dev_cfg, &panel_), TAG,
      "JD9365");
  // esp_lcd_dpi_panel_enable_dma2d(panel_);  // disabled: corrupts PSRAM on
  // v1.3
  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel_), TAG, "panel reset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel_), TAG, "panel init");
  // mirror removed: combined with LVGL rotation produces wrong orientation
  // ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_, true, true));

  // Double framebuffer DSI pour le rendu vidéo direct : le rendu écrit
  // toujours dans le buffer qui N'EST PAS en cours de scan-out (present_frame
  // fait tourner write_idx_), ce qui élimine le tearing visible sur les
  // contrôles LVGL composités par-dessus la vidéo (num_fbs=1 le causait).
  esp_err_t fb_err =
      esp_lcd_dpi_panel_get_frame_buffer(panel_, 2, &fbs_[0], &fbs_[1]);
  if (fb_err != ESP_OK || fbs_[0] == nullptr || fbs_[1] == nullptr) {
    ESP_LOGE(TAG, "get_frame_buffer failed: %s", esp_err_to_name(fb_err));
    fbs_[0] = fbs_[1] = nullptr; // dsi_frame_buffer() renverra nullptr
  }

  /* Signal de fin de trame : sans lui, rien ne dit quand la bascule armée par
   * present_frame() est réellement prise (cf. wait_frame_flip). L'ISR n'a
   * besoin que du sémaphore, pas de `this` : on le passe en user_ctx. */
  flip_sem_ = xSemaphoreCreateBinary();
  if (flip_sem_ == nullptr) {
    ESP_LOGE(TAG, "flip semaphore alloc failed");
  } else {
    esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {};
    dpi_cbs.on_refresh_done = dsi_refresh_done_cb;
    esp_err_t cb_err =
        esp_lcd_dpi_panel_register_event_callbacks(panel_, &dpi_cbs, flip_sem_);
    if (cb_err != ESP_OK) {
      /* wait_frame_flip() redevient un no-op : mieux vaut du tearing que la
       * tâche vidéo bloquée sur un sémaphore que personne ne donne. */
      ESP_LOGE(TAG, "register refresh_done cb: %s", esp_err_to_name(cb_err));
      vSemaphoreDelete(flip_sem_);
      flip_sem_ = nullptr;
    }
  }

  ppa_client_config_t ppa_cfg = {};
  ppa_cfg.oper_type = PPA_OPERATION_SRM;
  /* 2 et non le défaut 1 : l'élément de transaction n'est recyclé que par l'ISR
   * de fin (ppa_core.c), et le header du driver précise lui-même que 1 ne
   * suffit « que si toutes les transactions sont en PPA_TRANS_MODE_BLOCKING » —
   * ce que nous ne faisons plus. Une transaction perdue par l'attente bornée
   * n'est donc JAMAIS rendue, et à max_pending=1 toutes les opérations
   * suivantes de ce client échouent définitivement en ESP_FAIL. */
  ppa_cfg.max_pending_trans_num = 2;
  ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &ppa_lvgl_), TAG,
                      "PPA LVGL client");

  /* PPA_TRANS_MODE_BLOCKING attend le sémaphore de transaction avec
   * portMAX_DELAY (ppa_core.c) : quand la transaction DMA2D ne signale jamais
   * sa fin, lvgl_task reste bloquée à vie et TOUT l'écran gèle, sans panique ni
   * chien de garde (les tâches idle continuent de tourner). Diagnostiqué avec
   * l'instrumentation LVGL_STALL : phase=2, eBlocked, dans_ppa=1, géométrie de
   * bande valide. Le flush passe donc en NON_BLOCKING avec une attente bornée
   * sur ce sémaphore : une transaction perdue coûte une bande non peinte, pas
   * l'appareil entier. */
  ppa_lvgl_done_ = xSemaphoreCreateBinary();
  if (ppa_lvgl_done_ != nullptr) {
    ppa_event_callbacks_t ppa_cbs = {};
    ppa_cbs.on_trans_done = ppa_lvgl_done_cb;
    if (ppa_client_register_event_callbacks(ppa_lvgl_, &ppa_cbs) != ESP_OK) {
      /* Sans callback, l'attente bornée ne serait jamais réveillée : on repasse
       * au mode bloquant historique plutôt que de perdre toutes les bandes. */
      vSemaphoreDelete(ppa_lvgl_done_);
      ppa_lvgl_done_ = nullptr;
      ESP_LOGE(TAG, "PPA LVGL done cb : repli sur le mode bloquant");
    }
  }

  /* Separate PPA client for video upscaling: render_yuv420_packed_to_panel
   * runs on the video render task concurrently with handle_flush, and a PPA
   * client handle must not be shared across tasks. */
  ppa_client_config_t ppa_video_cfg = {};
  ppa_video_cfg.oper_type = PPA_OPERATION_SRM;
  ppa_video_cfg.max_pending_trans_num = 2; // cf. commentaire du client LVGL
  ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_video_cfg, &ppa_video_), TAG,
                      "PPA video client");

  /* Même dispositif que pour le client LVGL : le mode BLOCKING attend le
   * sémaphore de transaction ET celui du moteur avec portMAX_DELAY. Comme le
   * moteur SRM est partagé entre les deux clients, laisser ce chemin bloquant
   * suffisait à regeler l'appareil dès qu'une transaction du flush était
   * perdue. Le callback est le même : il ne fait que donner le sémaphore reçu
   * en user_data. */
  ppa_video_done_ = xSemaphoreCreateBinary();
  if (ppa_video_done_ != nullptr) {
    ppa_event_callbacks_t ppa_vcbs = {};
    ppa_vcbs.on_trans_done = ppa_lvgl_done_cb;
    if (ppa_client_register_event_callbacks(ppa_video_, &ppa_vcbs) != ESP_OK) {
      vSemaphoreDelete(ppa_video_done_);
      ppa_video_done_ = nullptr;
      ESP_LOGE(TAG, "PPA vidéo done cb : repli sur le mode bloquant");
    }
  }

  /* Pas de buffer d'upscale intermédiaire : render_yuv420_packed_to_panel()
   * fait converser + mettre à l'échelle le PPA directement dans le
   * framebuffer DSI. L'ancien scale_buf_ (1,95 Mo de PSRAM, panel-sized)
   * n'était plus lu par personne depuis ce changement. */

  return ESP_OK;
}

esp_err_t Bsp::init_lvgl() {
  lv_init();
  lvgl_mux_ = xSemaphoreCreateRecursiveMutex();
  ESP_RETURN_ON_FALSE(lvgl_mux_, ESP_ERR_NO_MEM, TAG, "LVGL mutex");

  /* Horloge LUE à la demande plutôt qu'un esp_timer périodique de 2 ms
   * (lv_tick_inc) : celui-ci réveillait la tâche esp_timer (prio 22, cœur 0)
   * 500 fois par seconde, préemptant à chaque fois le drain TCP du MJPEG qui
   * vit sur le même cœur. */
  lv_tick_set_cb(&Bsp::tick_cb);

  /* Logical display is landscape 1280×800 (physical panel rotated 270°).
   * Each PARTIAL band: logical 1280-wide × draw_lines tall.
   * After 270° rotation: physical strip draw_lines-wide × 1280-tall (same pixel
   * count). draw_size must hold one band worth of pixels.
   * Le mode FULL (35cec64) a été abandonné : il imposait une rotation PPA de
   * 1 Mpx (42,6 ms) et une bascule de framebuffer à chaque flush, même pour
   * repeindre un label. */
  /* Le rendu logiciel LVGL écrit ce buffer pixel par pixel. En PSRAM il
   * plafonnait à ~7,6 Mpx/s (mesuré le 28/07/2026 via les compteurs REFR_*),
   * alors que le bus PSRAM est déjà saturé par le PPA et le scan-out DSI. En
   * SRAM interne le rendu ne partage plus ce bus.
   * Enveloppe volontairement courte (2 x 16 lignes = 80 Ko) : mbedtls alloue
   * son contexte DTLS en SRAM interne pour le backchannel micro et en manquait
   * déjà. Repli PSRAM avec des bandes larges si l'allocation interne échoue.
   * DEUX buffers, à budget mémoire inchangé (c'était 1 x 32 lignes) : le flush
   * ne bloque plus jusqu'à la fin de la rotation PPA, il laisse la transaction
   * en vol et rend la main à LVGL (cf. ppa_lvgl_run / ppa_lvgl_wait_pending).
   * Le second buffer est ce qui rend ça légal — LVGL rend la bande n+1 dans
   * l'autre moitié pendant que le DMA lit encore la bande n. Avec un buffer
   * unique il écraserait le px_map sous le nez du PPA.
   * Contrepartie : deux fois plus de transactions PPA par repaint plein cadre
   * (50 au lieu de 25), donc deux fois l'overhead de soumission.
   * 2 x 32 depuis que LVGL alloue en PSRAM (lv_mem_psram.c), qui a rendu
   * ~240 Ko de SRAM interne. Mesuré le 2026-11-07 : repaint de l'écran caméra
   * 50 -> 37 ms, mutex LVGL max 20 -> 4 ms ; SRAM interne au minimum en session
   * 248 -> 167 Ko, encore très au-dessus du besoin DTLS. 1280x32 est la
   * géométrie de bande d'avant le double buffer : sûre vis-à-vis de DIG-734. */
  constexpr size_t kDrawLinesInternal = 32;
  constexpr size_t kDrawLinesPsram = 60;
  size_t draw_size =
      static_cast<size_t>(kLcdVRes) * kDrawLinesInternal * sizeof(uint16_t);
  void *db0 = heap_caps_aligned_alloc(64, draw_size,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  void *db1 = (db0 != nullptr)
                  ? heap_caps_aligned_alloc(
                        64, draw_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)
                  : nullptr;
  const bool draw_buf_internal = (db1 != nullptr);
  if (!draw_buf_internal) {
    /* Tout ou rien : un buffer en SRAM et l'autre en PSRAM ferait alterner le
     * débit du rendu logiciel d'une bande à l'autre (7,6 Mpx/s en PSRAM), et le
     * recouvrement mesuré n'aurait plus aucun sens. */
    heap_caps_free(db0);
    draw_size =
        static_cast<size_t>(kLcdVRes) * kDrawLinesPsram * sizeof(uint16_t);
    db0 = heap_caps_aligned_alloc(64, draw_size, MALLOC_CAP_SPIRAM);
    db1 = heap_caps_aligned_alloc(64, draw_size, MALLOC_CAP_SPIRAM);
  }
  /* PPA output buffer (chemin de repli seulement, cf. handle_flush) :
   * ppa_srm.c:195 exige que l'adresse ET la taille soient multiples de la
   * ligne de cache (CONFIG_CACHE_L2_CACHE_LINE_SIZE). 1280*16*2 = 40960 et
   * 1280*60*2 = 153600 sont multiples de 64 comme de 128 : OK dans les deux
   * configurations de ligne. */
  rot_buf_ = heap_caps_aligned_alloc(128, draw_size, MALLOC_CAP_SPIRAM);
  rot_buf_size_ = draw_size;
  assert(db0 && db1 && rot_buf_);

  /* Résolution physique du panneau = 800x1280 portrait.
   * Rotation 90° → résolution logique 1280x800 paysage pour toute l'UI. */
  display_ = lv_display_create(kLcdHRes, kLcdVRes);
  lv_display_set_user_data(display_, this);
  lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display_, &Bsp::flush_cb);
  lv_display_set_buffers(display_, db0, db1, draw_size,
                         LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_rotation(display_, LV_DISPLAY_ROTATION_270);

  /* Thème : le défaut LVGL en mode SOMBRE, à plat.
   * lv_display_create() a déjà auto-initialisé le thème avec bleu/rouge et le
   * mode issu de Kconfig ; on le ré-initialise ici pour choisir l'accent. Un
   * lv_theme_default_init() aux mêmes paramètres est un no-op (il compare avant
   * de reconstruire ses styles), l'appel n'est donc pas gratuit deux fois.
   *
   * Trois raisons, dans cet ordre :
   * 1. TOUS les widgets qui dépendent réellement du thème (interrupteurs,
   *    curseurs, listes déroulantes, onglets, clavier, boîtes de dialogue)
   *    vivent sur des écrans SOMBRES (Caméra, Configuration, Micro, Carte SD) :
   *    en mode clair ils sortaient gris pâle sur fond anthracite. Les deux
   *    écrans clairs de l'appli (Météo, Agenda) peignent chacun de leurs
   *    conteneurs eux-mêmes et ne dépendent pas du thème — à une exception
   *    près, corrigée avec ce changement : make_icon_box() dans ui_weather_v3
   *    laissait son fond au thème et posait un carré noir derrière chaque
   *    icône Lottie.
   * 2. lv_theme_default.c ne pose son ombre floutée sur lv_button que dans la
   *    branche CLAIRE. Le mode sombre la supprime donc À LA SOURCE, pour tous
   *    les widgets, y compris ceux qu'on ne construit pas nous-mêmes.
   * 3. Avec LV_THEME_DEFAULT_GROW et TRANSITION_TIME désarmés dans sdkconfig,
   *    un appui ne déclenche plus ni agrandissement animé ni transition de
   *    style : chacun de ces effets valait une salve de repaints par toucher,
   *    et sur ce panneau un repaint plein cadre coûte ~52 ms rien qu'en
   *    rotation PPA.
   *
   * Accent = le vert « actif » déjà utilisé par les voyants et les boutons
   * d'action (ui.cpp, ui_calls.cpp) ; secondaire = l'orange d'alerte des
   * bandeaux d'erreur. LV_FONT_DEFAULT (montserrat 14) ne sert qu'aux widgets
   * dont l'application ne fixe pas la police. */
  lv_display_set_theme(display_,
                       lv_theme_default_init(display_, lv_color_hex(0x1FA85A),
                                             lv_color_hex(0xD97A3D),
                                             /*dark=*/true, LV_FONT_DEFAULT));

  lv_display_add_event_cb(display_, lvgl_refr_start_cb, LV_EVENT_REFR_START,
                          nullptr);
  lv_display_add_event_cb(display_, lvgl_refr_ready_cb, LV_EVENT_REFR_READY,
                          nullptr);
  ESP_LOGI(TAG,
           "LVGL display: hor=%d ver=%d (after rotation), draw-buf 2x%zuKB %s",
           (int)lv_display_get_horizontal_resolution(display_),
           (int)lv_display_get_vertical_resolution(display_), draw_size / 1024,
           draw_buf_internal ? "SRAM interne" : "PSRAM (repli)");

  return ESP_OK;
}

esp_err_t Bsp::init_touch() {
  i2c_master_bus_config_t i2c_cfg = {};
  i2c_cfg.i2c_port = kTouchI2cPort;
  i2c_cfg.sda_io_num = static_cast<gpio_num_t>(kTouchSdaGpio);
  i2c_cfg.scl_io_num = static_cast<gpio_num_t>(kTouchSclGpio);
  i2c_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  i2c_cfg.glitch_ignore_cnt = 7;
  i2c_cfg.flags.enable_internal_pullup = true;

  ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus_), TAG, "I2C bus");
  log_i2c_scan(i2c_bus_);
  log_camera_sensor_probe(i2c_bus_);

  esp_lcd_panel_io_handle_t touch_io = nullptr;
  /* Same values as ESP_LCD_TOUCH_IO_I2C_GSL3680_CONFIG(); spelled out because
   * the macro's partial designated init trips
   * -Werror=missing-field-initializers in C++. */
  esp_lcd_panel_io_i2c_config_t touch_io_cfg = {};
  touch_io_cfg.dev_addr = kTouchI2cAddr;
  touch_io_cfg.control_phase_bytes = 1;
  touch_io_cfg.dc_bit_offset = 0;
  touch_io_cfg.lcd_cmd_bits = 8;
  touch_io_cfg.lcd_param_bits = 8;
  touch_io_cfg.scl_speed_hz = kTouchI2cFreq;
  touch_io_cfg.flags.disable_control_phase = 1;
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_i2c(i2c_bus_, &touch_io_cfg, &touch_io), TAG,
      "touch IO");

  esp_lcd_touch_io_gsl3680_config_t gsl_cfg = {};
  gsl_cfg.dev_addr = kTouchI2cAddr;

  esp_lcd_touch_config_t touch_cfg = {};
  touch_cfg.x_max = kLcdHRes - 1;
  touch_cfg.y_max = kLcdVRes - 1;
  touch_cfg.rst_gpio_num = static_cast<gpio_num_t>(kTouchRstGpio);
  touch_cfg.int_gpio_num = static_cast<gpio_num_t>(kTouchIntGpio);
  touch_cfg.levels.reset = 0;
  touch_cfg.levels.interrupt = 0;
  touch_cfg.flags.mirror_x = true;
  // mirror_y=false (pas true) : avec la rotation LVGL 270°, logical_x =
  // y_phys (cf. lv_display_rotate_point). Un mirror_y à true masquait le
  // problème pour tous les boutons centrés horizontalement (le centre est
  // invariant par mirroir), mais un bouton excentré (ex: bouton micro à
  // droite de l'écran) recevait un logical_x proche de 0 au lieu de proche
  // du max — confirmé par mesure : appui physique visé sur le bouton micro
  // (x_phys≈378,y_phys≈75) donnait logical≈(75,420) au lieu de (≈1200,420).
  touch_cfg.flags.mirror_y = false;
  touch_cfg.driver_data = &gsl_cfg;

  esp_err_t touch_err =
      esp_lcd_touch_new_i2c_gsl3680(touch_io, &touch_cfg, &touch_);
  if (touch_err == ESP_OK) {
    ESP_LOGI(TAG, "touch handle=%p read_data=%p", touch_,
             touch_ ? reinterpret_cast<void *>(touch_->read_data) : nullptr);
    touch_indev_ = lv_indev_create();
    lv_indev_set_type(touch_indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev_, &Bsp::touch_read_cb);
    lv_indev_set_user_data(touch_indev_, this);
    lv_indev_set_display(touch_indev_, display_);
    ESP_LOGI(TAG, "GSL3680 touch OK at I2C addr 0x%02X, firmware loaded",
             kTouchI2cAddr);
  } else {
    ESP_LOGW(TAG, "GSL3680 init failed (%s), touch disabled",
             esp_err_to_name(touch_err));
    touch_ = nullptr;
  }
  /* Touch is optional: init() succeeds without it. */
  return ESP_OK;
}

esp_err_t Bsp::init_audio() {
  if (!i2c_bus_) {
    return ESP_ERR_INVALID_STATE;
  }

  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  // Sans lui (défaut false), un sous-débit fait reboucler le DMA TX sur ses
  // derniers descripteurs : ~180 ms d'audio périmé rejoué en boucle
  // (bourdonnement). Avec, chaque descripteur consommé est remis à zéro dans
  // l'ISR : un trou réseau devient du silence. Le partage de chan_cfg avec la
  // RX est sans effet, l'option ne concerne que l'émission.
  chan_cfg.auto_clear_after_cb = true;
  ESP_RETURN_ON_ERROR(
      i2s_new_channel(&chan_cfg, &audio_tx_chan_, &audio_rx_chan_), TAG,
      "I2S new channel");

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kAudioSampleRate);
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  std_cfg.gpio_cfg.mclk = static_cast<gpio_num_t>(kAudioMclkGpio);
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(kAudioBclkGpio);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(kAudioLrclkGpio);
  std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(kAudioDoutGpio);
  std_cfg.gpio_cfg.din = static_cast<gpio_num_t>(kAudioDinGpio);
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(audio_tx_chan_, &std_cfg), TAG,
                      "I2S std mode (tx)");
  ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(audio_rx_chan_, &std_cfg), TAG,
                      "I2S std mode (rx)");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(audio_tx_chan_), TAG,
                      "I2S channel enable (tx)");
  ESP_RETURN_ON_ERROR(i2s_channel_enable(audio_rx_chan_), TAG,
                      "I2S channel enable (rx)");

  audio_codec_i2s_cfg_t i2s_if_cfg = {};
  i2s_if_cfg.port = I2S_NUM_0;
  i2s_if_cfg.tx_handle = audio_tx_chan_;
  i2s_if_cfg.rx_handle = audio_rx_chan_;
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if_cfg);
  ESP_RETURN_ON_FALSE(data_if, ESP_FAIL, TAG, "audio codec data if");

  audio_codec_i2c_cfg_t i2c_if_cfg = {};
  i2c_if_cfg.port = I2C_NUM_0;
  i2c_if_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
  i2c_if_cfg.bus_handle = i2c_bus_;
  const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_if_cfg);
  ESP_RETURN_ON_FALSE(ctrl_if, ESP_FAIL, TAG, "audio codec ctrl if");

  // Ajoutez un log pour vérifier si le matériel est bien à 1

  const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
  ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "audio codec gpio if");

  es8311_codec_cfg_t es8311_cfg = {};
  es8311_cfg.ctrl_if = ctrl_if;
  es8311_cfg.gpio_if = gpio_if;
  es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
  es8311_cfg.master_mode = false;
  es8311_cfg.use_mclk = true;
  es8311_cfg.pa_pin = GPIO_NUM_20;
  es8311_cfg.pa_reverted = false;
  es8311_cfg.hw_gain.pa_voltage = 5.0;
  es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
  const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
  ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "es8311 codec if");

  esp_codec_dev_cfg_t dev_cfg = {};
  dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
  dev_cfg.codec_if = codec_if;
  dev_cfg.data_if = data_if;
  audio_codec_ = esp_codec_dev_new(&dev_cfg);
  ESP_RETURN_ON_FALSE(audio_codec_, ESP_FAIL, TAG, "esp_codec_dev_new");

  esp_codec_dev_sample_info_t sample_cfg = {};
  sample_cfg.bits_per_sample = 16;
  sample_cfg.channel = 1;
  sample_cfg.channel_mask = 0x01;
  sample_cfg.sample_rate = kAudioSampleRate;
  ESP_RETURN_ON_ERROR(esp_codec_dev_open(audio_codec_, &sample_cfg), TAG,
                      "esp_codec_dev_open");

  dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
  audio_input_codec_ = esp_codec_dev_new(&dev_cfg);
  ESP_RETURN_ON_FALSE(audio_input_codec_, ESP_FAIL, TAG,
                      "esp_codec_dev_new (in)");
  ESP_RETURN_ON_ERROR(esp_codec_dev_open(audio_input_codec_, &sample_cfg), TAG,
                      "esp_codec_dev_open (in)");

  ESP_RETURN_ON_ERROR(
      esp_codec_dev_set_out_vol(audio_codec_, speaker_volume_pct_), TAG,
      "esp_codec_dev_set_out_vol");
  ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(audio_input_codec_,
                                                mic_gain_pct_ * 48.0f / 100.0f),
                      TAG, "esp_codec_dev_set_in_gain");

  ESP_LOGI(TAG, "ES8311 audio OK: %luHz mono 16-bit (in+out)",
           (unsigned long)kAudioSampleRate);
  return ESP_OK;
}

void Bsp::load_prefs() {
  nvs_handle_t handle;
  if (nvs_open(kAudioPrefsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return; // namespace not yet created: keep defaults
  }
  uint8_t val = 0;
  if (nvs_get_u8(handle, kSpeakerVolKey, &val) == ESP_OK) {
    speaker_volume_pct_ = val;
  }
  if (nvs_get_u8(handle, kMicGainKey, &val) == ESP_OK) {
    mic_gain_pct_ = val;
  }
  if (nvs_get_u8(handle, kBacklightKey, &val) == ESP_OK) {
    // A stored value below the floor can only come from a firmware that
    // persisted the power-off blanking. Treat it as "unset" and go back to
    // full brightness, otherwise the board boots dark forever and only an NVS
    // erase recovers it.
    backlight_pct_ = (val < kBacklightMinPct) ? 100 : val;
  }
  nvs_close(handle);
}

void Bsp::set_speaker_volume(uint8_t volume_pct, bool persist) {
  if (volume_pct > 100) {
    volume_pct = 100;
  }
  speaker_volume_pct_ = volume_pct;
  if (audio_codec_) {
    esp_codec_dev_set_out_vol(audio_codec_, volume_pct);
  }
  if (!persist) {
    return;
  }
  nvs_handle_t handle;
  if (nvs_open(kAudioPrefsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_u8(handle, kSpeakerVolKey, volume_pct);
    nvs_commit(handle);
    nvs_close(handle);
  }
}

void Bsp::set_mic_gain(uint8_t gain_pct, bool persist) {
  if (gain_pct > 100) {
    gain_pct = 100;
  }
  mic_gain_pct_ = gain_pct;
  if (audio_input_codec_) {
    esp_codec_dev_set_in_gain(audio_input_codec_, gain_pct * 48.0f / 100.0f);
  }
  if (!persist) {
    return;
  }
  nvs_handle_t handle;
  if (nvs_open(kAudioPrefsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_u8(handle, kMicGainKey, gain_pct);
    nvs_commit(handle);
    nvs_close(handle);
  }
}

esp_err_t Bsp::play_pcm(const int16_t *pcm, size_t samples) {
  if (!audio_codec_) {
    return ESP_ERR_INVALID_STATE;
  }
  // const_cast strips the `const` from `pcm` because esp_codec_dev_write's
  // C signature takes a non-const pointer even though it only reads the
  // buffer (a common C-API sloppiness). This is the one cast category
  // that's genuinely dangerous — it's a promise to the compiler that the
  // callee won't actually mutate through the pointer, unverifiable here
  // beyond reading the codec driver source. C# has no analogue: there's
  // no way to "cast away readonly" on an array reference in the first
  // place.
  return esp_codec_dev_write(audio_codec_, const_cast<int16_t *>(pcm),
                             (int)(samples * sizeof(int16_t)));
}

esp_err_t Bsp::read_pcm(int16_t *pcm, size_t samples) {
  if (!audio_input_codec_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (mic_ref_) {
    return read_pcm_ref(pcm, nullptr, samples);
  }
  return esp_codec_dev_read(audio_input_codec_, pcm,
                            (int)(samples * sizeof(int16_t)));
}

esp_err_t Bsp::set_mic_reference(bool on) {
  if (!audio_rx_chan_) {
    return ESP_ERR_INVALID_STATE;
  }
  if (on == mic_ref_) {
    return ESP_OK;
  }
  if (on && rx_stereo_ == nullptr) {
    rx_stereo_ = static_cast<int16_t *>(heap_caps_malloc(
        kRxRefChunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (rx_stereo_ == nullptr) {
      return ESP_ERR_NO_MEM;
    }
  }
  // Même configuration que celle posée par esp_codec_dev pour la capture mono
  // (set_drv_fs : 2 slots de 16 bits, masque slot 0 = LEFT) : on ne change que
  // le mode et le masque, jamais la largeur — elle est partagée avec le TX en
  // full duplex (BCLK/WS communs).
  i2s_std_slot_config_t slot = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
      I2S_DATA_BIT_WIDTH_16BIT, on ? I2S_SLOT_MODE_STEREO : I2S_SLOT_MODE_MONO);
  slot.slot_mask = on ? I2S_STD_SLOT_BOTH : I2S_STD_SLOT_LEFT;
  ESP_RETURN_ON_ERROR(i2s_channel_disable(audio_rx_chan_), TAG,
                      "I2S rx disable");
  esp_err_t err = i2s_channel_reconfig_std_slot(audio_rx_chan_, &slot);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "reconfig RX %s : %s", on ? "stéréo" : "mono",
             esp_err_to_name(err));
  } else {
    mic_ref_ = on;
  }
  // Réactivé quoi qu'il arrive : un RX laissé désactivé couperait le micro
  // jusqu'au redémarrage.
  const esp_err_t en = i2s_channel_enable(audio_rx_chan_);
  return err != ESP_OK ? err : en;
}

esp_err_t Bsp::read_pcm_ref(int16_t *mic, int16_t *ref, size_t samples) {
  if (!audio_input_codec_ || !mic_ref_ || rx_stereo_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }
  size_t done = 0;
  while (done < samples) {
    const size_t n =
        samples - done < kRxRefChunk ? samples - done : kRxRefChunk;
    const esp_err_t err = esp_codec_dev_read(
        audio_input_codec_, rx_stereo_, (int)(n * 2 * sizeof(int16_t)));
    if (err != ESP_OK) {
      return err;
    }
    for (size_t i = 0; i < n; i++) {
      mic[done + i] = rx_stereo_[2 * i];
      if (ref != nullptr) {
        ref[done + i] = rx_stereo_[2 * i + 1];
      }
    }
    done += n;
  }
  return ESP_OK;
}

void Bsp::set_backlight(uint8_t brightness_pct, bool persist) {
  if (brightness_pct > 100) {
    brightness_pct = 100;
  }
  backlight_pct_ = brightness_pct;
  apply_backlight_duty(brightness_pct);
  if (!persist) {
    return;
  }

  // NVS failure is non-fatal: the value stays applied to the panel, only the
  // persistence is lost — same contract as set_speaker_volume.
  nvs_handle_t handle;
  if (nvs_open(kAudioPrefsNamespace, NVS_READWRITE, &handle) == ESP_OK) {
    nvs_set_u8(handle, kBacklightKey, brightness_pct);
    nvs_commit(handle);
    nvs_close(handle);
  }
}

void Bsp::apply_backlight_duty(uint8_t brightness_pct) {
  const uint32_t duty = (static_cast<uint32_t>(brightness_pct) * 8191U) / 100U;
  ledc_set_duty(LEDC_LOW_SPEED_MODE, kLedcChannel, duty);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, kLedcChannel);
  display_on_.store(brightness_pct > 0, std::memory_order_relaxed);
}

void Bsp::backlight_off() {
  // Hardware blanking (deep sleep): neither remembered nor persisted. Routing
  // this through set_backlight() would store a 0 and blank the panel on every
  // subsequent boot — the board would come back dark with no way to fix it
  // from the screen.
  apply_backlight_duty(0);
}

void Bsp::set_backlight_temporary(uint8_t brightness_pct) {
  if (brightness_pct > 100) {
    brightness_pct = 100;
  }
  // backlight_pct_ n'est PAS touché : c'est lui qui permet de revenir au
  // réglage de l'utilisateur, et rien n'est écrit en NVS.
  apply_backlight_duty(brightness_pct);
}

void Bsp::restore_backlight() { apply_backlight_duty(backlight_pct_); }

// lock()/unlock() are the raw pair that LvglLock (bsp.h) wraps with RAII.
// Seuls les appelants de CE fichier (render_*_to_panel, present_frame) les
// utilisent nus, et uniquement autour d'un unique appel PPA sans `return`
// intermédiaire — c'est la seule forme où le nu reste sûr. Hors de bsp.cpp,
// TOUT passe par LvglLock : un `return` ajouté plus tard dans une région
// verrouillée sans unlock gèlerait le mutex LVGL à vie, sans le moindre
// avertissement du compilateur. Les sessions caméra/micro relâchaient
// autrefois le mutex de leur appelant par un unlock() nu pendant leur
// démontage : le démontage vit désormais dans sa propre tâche (cf.
// camera_session::stop), ce qui supprime ce cas entièrement.
bool Bsp::lock(uint32_t timeout_ms) {
  if (xSemaphoreTakeRecursive(
          lvgl_mux_, timeout_ms == 0 ? portMAX_DELAY
                                     : pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return false; // timeout : ne pas compter une détention qu'on n'a pas
                  // obtenue
  }
  if (++lvgl_hold_depth_ == 1) {
    lvgl_hold_start_us_ = esp_timer_get_time();
  }
  return true;
}

void Bsp::unlock() {
  if (--lvgl_hold_depth_ == 0) {
    const uint32_t held =
        (uint32_t)(esp_timer_get_time() - lvgl_hold_start_us_);
    if (held > lvgl_max_hold_us_.load(std::memory_order_relaxed)) {
      lvgl_max_hold_us_.store(held, std::memory_order_relaxed);
      const char *name = pcTaskGetName(nullptr);
      if (name != nullptr) {
        strncpy(lvgl_max_hold_owner_, name, sizeof(lvgl_max_hold_owner_) - 1);
        lvgl_max_hold_owner_[sizeof(lvgl_max_hold_owner_) - 1] = '\0';
      }
    }
  }
  xSemaphoreGiveRecursive(lvgl_mux_);
}

uint32_t Bsp::lvgl_max_hold_us(char *owner, size_t owner_len) {
  if (owner != nullptr && owner_len > 0) {
    strncpy(owner, lvgl_max_hold_owner_, owner_len - 1);
    owner[owner_len - 1] = '\0';
  }
  return lvgl_max_hold_us_.exchange(0, std::memory_order_relaxed);
}

esp_err_t Bsp::render_yuv420_packed_to_panel(const void *packed, uint32_t w,
                                             uint32_t h, uint32_t dst_x,
                                             uint32_t dst_y, uint32_t dst_w,
                                             uint32_t dst_h) {
  void *fb = dsi_frame_buffer();
  if (!ppa_video_ || !fb) {
    return ESP_ERR_INVALID_STATE;
  }
  if (w == 0 || h == 0 || (w & 1) || (h & 1) || w > dst_w * 16 ||
      h > dst_h * 16) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (dst_w == 0 || dst_h == 0 || dst_x + dst_w > (uint32_t)kLcdHRes ||
      dst_y + dst_h > (uint32_t)kLcdVRes) {
    return ESP_ERR_INVALID_SIZE;
  }

  /* Facteur d'upscale/downscale : aspect préservé, quantifié au pas 1/16 du
   * PPA, borné à <16. floor garantit que la sortie tient dans la cible. */
  float s = std::min((float)dst_w / (float)w, (float)dst_h / (float)h);
  s = std::floor(s * 16.0f) / 16.0f;
  if (s > 15.9375f) {
    s = 15.9375f; // 255/16, borne haute du PPA
  }
  if (s < 0.0625f) {
    return ESP_ERR_INVALID_SIZE;
  }
  uint32_t out_w = (uint32_t)((float)w * s);
  uint32_t out_h = (uint32_t)((float)h * s);
  if (out_w > dst_w) {
    out_w = dst_w;
  }
  if (out_h > dst_h) {
    out_h = dst_h;
  }

  ppa_srm_oper_config_t srm = {};
  srm.in.buffer = packed;
  srm.in.pic_w = w;
  srm.in.pic_h = h;
  srm.in.block_w = w;
  srm.in.block_h = h;
  srm.in.block_offset_x = 0;
  srm.in.block_offset_y = 0;
  srm.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
  srm.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
  srm.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;
  srm.out.buffer = fb;
  srm.out.buffer_size = (size_t)kLcdHRes * kLcdVRes * sizeof(uint16_t);
  srm.out.pic_w = kLcdHRes;
  srm.out.pic_h = kLcdVRes;
  /* Centrage dans la zone cible (pas le panneau entier) : offsets pairs
   * (contrainte descripteur DMA sur certains modes couleur). */
  srm.out.block_offset_x = dst_x + (((dst_w - out_w) / 2) & ~1u);
  srm.out.block_offset_y = dst_y + (((dst_h - out_h) / 2) & ~1u);
  srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  srm.scale_x = s;
  srm.scale_y = s;
  srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;

  video_rect_x_ = srm.out.block_offset_x;
  video_rect_y_ = srm.out.block_offset_y;
  video_rect_w_ = out_w;
  video_rect_h_ = out_h;

  lock();
  esp_err_t ret = ppa_video_run(srm);
  unlock();
  return ret;
}

esp_err_t Bsp::ppa_lvgl_wait_pending() {
  if (!ppa_lvgl_pending_) {
    return ESP_OK;
  }
  ppa_lvgl_pending_ = false;
  /* 500 ms : deux ordres de grandeur au-dessus de la bande la plus chère
   * mesurée (rotation plein écran ~52 ms), donc jamais atteint en régime
   * normal. */
  if (xSemaphoreTake(ppa_lvgl_done_, pdMS_TO_TICKS(500)) == pdTRUE) {
    return ESP_OK;
  }
  /* L'ISR de fin peut encore rendre le sémaphore après ce timeout : on solde la
   * dette à la prochaine soumission plutôt que d'y consommer un acquittement
   * qui n'est pas le sien. */
  ppa_lvgl_stale_ack_ = true;
  const uint32_t n = ppa_lvgl_timeouts_.fetch_add(1) + 1;
  ESP_LOGE(TAG, "PPA SRM sans fin de transaction (#%lu)", (unsigned long)n);
  return ESP_ERR_TIMEOUT;
}

esp_err_t Bsp::ppa_lvgl_run(ppa_srm_oper_config_t &srm, uint32_t w,
                            uint32_t h) {
  /* Attente bornée côté BSP plutôt que le portMAX_DELAY interne du driver :
   * cf. le commentaire de ppa_lvgl_done_ dans init_ppa(). Repli sur le mode
   * bloquant historique si le sémaphore ou le callback n'ont pas pu être
   * installés au boot — ce repli reste strictement en série, sans décalage. */
  if (ppa_lvgl_done_ == nullptr) {
    srm.mode = PPA_TRANS_MODE_BLOCKING;
    srm.user_data = nullptr;
    return ppa_do_scale_rotate_mirror(ppa_lvgl_, &srm);
  }

  /* Attente de la bande PRÉCÉDENTE, pas de celle qu'on soumet : entre les deux,
   * LVGL a rendu logiciellement la bande courante pendant que le DMA du PPA
   * travaillait. C'est tout le gain — le coût d'un repaint plein cadre passe de
   * (rendu + rotation) à max(rendu, rotation). */
  esp_err_t ret = ppa_lvgl_wait_pending();
  if (ret != ESP_OK) {
    /* La bande précédente n'a jamais fini : soumettre par-dessus ne ferait
     * qu'empiler sur un moteur SRM déjà en peine. handle_flush loggue et rend
     * la main à LVGL. */
    return ret;
  }
  if (ppa_lvgl_stale_ack_) {
    ppa_lvgl_stale_ack_ = false;
    xSemaphoreTake(ppa_lvgl_done_, 0);
  }

  srm.mode = PPA_TRANS_MODE_NON_BLOCKING;
  srm.user_data = ppa_lvgl_done_;
  ret = ppa_do_scale_rotate_mirror(ppa_lvgl_, &srm);
  if (ret == ESP_OK) {
    ppa_lvgl_pending_ = true;
  } else {
    ESP_LOGD(TAG, "soumission PPA refusée : bande %lux%lu", (unsigned long)w,
             (unsigned long)h);
  }
  return ret;
}

esp_err_t Bsp::ppa_video_run(ppa_srm_oper_config_t &srm) {
  srm.mode = (ppa_video_done_ != nullptr) ? PPA_TRANS_MODE_NON_BLOCKING
                                          : PPA_TRANS_MODE_BLOCKING;
  srm.user_data = ppa_video_done_;
  if (ppa_video_done_ != nullptr) {
    /* Purge d'un acquittement tardif de la transaction précédente : sans elle
     * le take suivant retournerait immédiatement pour une transaction qui n'a
     * pas encore tourné. */
    xSemaphoreTake(ppa_video_done_, 0);
  }
  esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_video_, &srm);
  if (ret == ESP_OK && ppa_video_done_ != nullptr) {
    /* 300 ms : l'opération vidéo la plus chère mesurée est l'upscale plein
     * écran à ~22 ms. Borne plus serrée que les 500 ms du flush car ce chemin
     * tient le mutex LVGL : chaque milliseconde d'attente ici est une frame
     * que mjpeg_rnd ne présente pas. */
    if (xSemaphoreTake(ppa_video_done_, pdMS_TO_TICKS(300)) != pdTRUE) {
      const uint32_t n = ppa_video_timeouts_.fetch_add(1) + 1;
      ESP_LOGE(TAG, "PPA vidéo sans fin de transaction (#%lu)",
               (unsigned long)n);
      ret = ESP_ERR_TIMEOUT;
    }
  }
  return ret;
}

esp_err_t Bsp::render_rgb565_to_panel(const void *rgb565, uint32_t w,
                                      uint32_t h, uint32_t dst_x,
                                      uint32_t dst_y, uint32_t dst_w,
                                      uint32_t dst_h, uint32_t src_stride) {
  void *fb = dsi_frame_buffer();
  if (!ppa_video_ || !fb) {
    return ESP_ERR_INVALID_STATE;
  }
  if (w == 0 || h == 0 || w > dst_w * 16 || h > dst_h * 16) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (dst_w == 0 || dst_h == 0 || dst_x + dst_w > (uint32_t)kLcdHRes ||
      dst_y + dst_h > (uint32_t)kLcdVRes) {
    return ESP_ERR_INVALID_SIZE;
  }

  /* Même quantification 1/16 du PPA que la version YUV, aspect préservé. */
  float s = std::min((float)dst_w / (float)w, (float)dst_h / (float)h);
  s = std::floor(s * 16.0f) / 16.0f;
  if (s > 15.9375f) {
    s = 15.9375f;
  }
  if (s < 0.0625f) {
    return ESP_ERR_INVALID_SIZE;
  }
  uint32_t out_w = (uint32_t)((float)w * s);
  uint32_t out_h = (uint32_t)((float)h * s);
  if (out_w > dst_w) {
    out_w = dst_w;
  }
  if (out_h > dst_h) {
    out_h = dst_h;
  }

  ppa_srm_oper_config_t srm = {};
  srm.in.buffer = rgb565;
  // pic_w = stride réel de la source (aligné 16 par le décodeur JPEG HW), pas
  // la largeur visible w : sinon le PPA lit chaque ligne décalée -> image
  // tuilée.
  srm.in.pic_w = src_stride ? src_stride : w;
  srm.in.pic_h = h;
  srm.in.block_w = w;
  srm.in.block_h = h;
  srm.in.block_offset_x = 0;
  srm.in.block_offset_y = 0;
  /* Entrée RGB565 : pas de conversion couleur (le JPEG HW a déjà fait YUV->RGB
   * en BT.601, cohérent avec le décodeur). */
  srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.out.buffer = fb;
  srm.out.buffer_size = (size_t)kLcdHRes * kLcdVRes * sizeof(uint16_t);
  srm.out.pic_w = kLcdHRes;
  srm.out.pic_h = kLcdVRes;
  /* Centrage horizontal aligné 32 px = 64 octets = une ligne de cache. Le
   * centrage exact d'un 540 dans 800 donne 130 px, soit un début de ligne à
   * l'octet y*1600 + 260 : 260 % 64 = 4, donc les 960 lignes écrites par le PPA
   * démarrent TOUTES désalignées vis-à-vis du burst DMA (128 o) et de la ligne
   * de cache -> read-modify-write à chaque bord de ligne. L'ancien `& ~1u` ne
   * garantissait que la parité (contrainte YUV), pas l'alignement mémoire.
   * Arrondi vers le BAS puis borné à dst_x : le bloc ne peut que reculer, donc
   * il tient toujours dans [dst_x, dst_x+dst_w). Décalage résiduel <= 31 px,
   * absorbé par la bande letterbox.
   * Le vertical n'a pas ce problème : une ligne fait kLcdHRes*2 = 1600 o =
   * 25*64, donc n'importe quel offset Y tombe déjà sur une ligne de cache. */
  constexpr uint32_t kOutXAlignPx = 32;
  uint32_t off_x = dst_x + (dst_w - out_w) / 2;
  off_x &= ~(kOutXAlignPx - 1u);
  if (off_x < dst_x) {
    off_x = dst_x;
  }
  srm.out.block_offset_x = off_x;
  srm.out.block_offset_y = dst_y + (((dst_h - out_h) / 2) & ~1u);
  srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  srm.scale_x = s;
  srm.scale_y = s;
  srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;

  video_rect_x_ = srm.out.block_offset_x;
  video_rect_y_ = srm.out.block_offset_y;
  video_rect_w_ = out_w;
  video_rect_h_ = out_h;

  /* L'attente de ce mutex a été mesurée (25/07/2026) à ~250 µs médians pour
   * 21 ms d'opération PPA : la contention avec lvgl_task n'est PAS un poste de
   * coût, inutile de chercher à s'en passer ici. */
  lock();
  esp_err_t ret = ppa_video_run(srm);
  unlock();
  return ret;
}

esp_err_t Bsp::blit_rgb565_to_panel(const void *rgb565, uint32_t w, uint32_t h,
                                    uint32_t dst_x, uint32_t dst_y) {
  void *fb = dsi_frame_buffer();
  if (!ppa_video_ || !fb || !rgb565) {
    return ESP_ERR_INVALID_STATE;
  }
  if (w == 0 || h == 0 || dst_x + w > (uint32_t)kLcdHRes ||
      dst_y + h > (uint32_t)kLcdVRes) {
    return ESP_ERR_INVALID_SIZE;
  }

  ppa_srm_oper_config_t srm = {};
  srm.in.buffer = rgb565;
  srm.in.pic_w = w; /* source contiguë : stride == largeur */
  srm.in.pic_h = h;
  srm.in.block_w = w;
  srm.in.block_h = h;
  srm.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.out.buffer = fb;
  srm.out.buffer_size = (size_t)kLcdHRes * kLcdVRes * sizeof(uint16_t);
  srm.out.pic_w = kLcdHRes;
  srm.out.pic_h = kLcdVRes;
  srm.out.block_offset_x = dst_x;
  srm.out.block_offset_y = dst_y;
  srm.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
  srm.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
  /* 1.0 exactement : le pas de quantification du PPA est 1/16, 1.0 est donc
   * représentable sans arrondi — la sortie fait pile w x h. */
  srm.scale_x = 1.0f;
  srm.scale_y = 1.0f;
  srm.alpha_update_mode = PPA_ALPHA_NO_CHANGE;

  /* video_rect_* n'est PAS mis à jour : il décrit le bloc vidéo, dont les
   * appelants (nettoyage des bandes letterbox) doivent rester indépendants de
   * cette superposition. */
  /* Pas de mutex LVGL, contrairement à render_rgb565_to_panel : la tâche LVGL
   * ne peut pas être en face. En direct_mode elle n'écrit pas le framebuffer
   * (même raison que direct_video_target), et le producteur de `rgb565` compose
   * dans SON buffer PSRAM. Le prendre coûtait cher : mesuré à band=5 ms quand
   * la composition du bandeau ne tombait pas dans la même seconde, jusqu'à 22
   * ms quand elle tombait dedans (lv_snapshot 1280x80 ARGB8888 tient le mutex
   * du début à la fin) — et une frame vidéo entière était alors perdue. */
  return ppa_video_run(srm);
}

uint8_t *Bsp::direct_video_target(uint32_t w, uint32_t h, uint32_t dst_x,
                                  uint32_t dst_y, uint32_t dst_w,
                                  uint32_t dst_h, size_t *out_size) {
  auto *fb = static_cast<uint8_t *>(dsi_frame_buffer());
  if (fb == nullptr || out_size == nullptr) {
    return nullptr;
  }
  /* Le décodeur JPEG HW aligne w ET h sur 16 px et écrit TOUT le rectangle
   * aligné : exiger que la source soit déjà alignée évite que les lignes/
   * colonnes de bourrage (contenu indéfini) deviennent visibles — le PPA, lui,
   * ne recopiait que le rectangle utile. */
  if (((w | h) & 15u) != 0) {
    return nullptr;
  }
  /* Stride identique = seule condition pour que les lignes décodées tombent
   * pile sur les lignes du framebuffer. Toute autre largeur exige le PPA. */
  if (w != static_cast<uint32_t>(kLcdHRes) || dst_x != 0 ||
      dst_w != static_cast<uint32_t>(kLcdHRes)) {
    return nullptr;
  }
  if (h > dst_h || dst_y + dst_h > static_cast<uint32_t>(kLcdVRes)) {
    return nullptr;
  }

  /* Centrage vertical sur ligne paire, comme le faisait le PPA. L'offset en
   * octets vaut y0 * 800 * 2 = y0 * 1600, toujours multiple de 64 : la
   * contrainte d'alignement DMA/cache du décodeur (esp_dma_is_buffer_
   * alignment_satisfied) est satisfaite pour n'importe quel y0. La taille
   * (w*h*2) l'est aussi. */
  const uint32_t y0 = dst_y + (((dst_h - h) / 2) & ~1u);

  video_rect_x_ = 0;
  video_rect_y_ = y0;
  video_rect_w_ = w;
  video_rect_h_ = h;

  *out_size = static_cast<size_t>(w) * h * sizeof(uint16_t);
  return fb + static_cast<size_t>(y0) * kLcdHRes * sizeof(uint16_t);
}

esp_err_t Bsp::present_frame() {
  // write_idx_ is std::atomic<int>: this is C++'s lock-free primitive for
  // a value shared between the video render task and any reader of
  // dsi_frame_buffer() (running on a different core). relaxed ordering is
  // enough because this is a single flip flag, not something guarding
  // access to other memory — there's no larger critical section it needs
  // to "publish". C#'s nearest equivalents are `Interlocked.*` (explicit
  // per-call, no wrapper type) or `Volatile`; std::atomic<T> instead makes
  // the variable itself carry the concurrency contract at every use site.
  int idx = write_idx_.load(std::memory_order_relaxed);
  void *fb = fbs_[idx];
  if (!fb) {
    return ESP_ERR_INVALID_STATE;
  }
  /* Le mutex LVGL ne sert qu'à publier shown_idx_ de façon cohérente avec un
   * flush en cours (handle_flush peint dsi_shown_frame_buffer). En direct_mode
   * le flush est un no-op : le prendre ici faisait attendre chaque frame vidéo
   * derrière la recomposition du bandeau (lv_snapshot sous le même mutex). */
  const bool locked = !direct_mode_.load(std::memory_order_relaxed);
  if (locked) {
    lock();
  }
  /* Pointer lies inside the driver's own fbs[]: esp_lcd_panel_draw_bitmap
   * recognises it and just flags this buffer for scan-out at the next
   * end of the DMA frame in progress, no copy. Rotates write_idx_ so the NEXT
   * frame's writes target the buffer this call just retired from the CPU/PPA
   * side — retired on paper only: it stays on screen until that flip lands,
   * hence wait_frame_flip(). */
  esp_err_t ret =
      esp_lcd_panel_draw_bitmap(panel_, 0, 0, kLcdHRes, kLcdVRes, fb);
  /* Purge collée à l'armement : une fin de trame ANTÉRIEURE ne prouve rien,
   * seule la suivante garantit que le scan-out a quitté l'autre buffer. Elle
   * se faisait après unlock(), qui peut céder le mutex à lvgl_task (prio 5 >
   * mjpeg_rnd) : la vraie bascule tombait dans la fenêtre, était jetée, et
   * wait_frame_flip attendait une trame panneau de plus (~31 ms). Purger
   * AVANT d'armer serait pire : la fin de trame de l'ancien buffer passerait
   * pour la bascule -> écriture sous le scan-out. */
  if (flip_sem_ != nullptr) {
    xSemaphoreTake(flip_sem_, 0);
  }
  if (ret == ESP_OK) {
    /* Le driver vient de poser cur_fb_index = idx : c'est désormais ce buffer
     * que handle_flush doit peindre (dsi_shown_frame_buffer). Publié AVANT de
     * rendre le mutex, sinon un flush pris dans l'intervalle peignait l'ancien
     * buffer affiché. */
    shown_idx_.store(idx, std::memory_order_relaxed);
  }
  if (locked) {
    unlock();
  }
  if (flip_sem_ != nullptr) {
    flip_pending_.store(true, std::memory_order_relaxed);
  }
  // XOR with 1 flips between index 0/1 — the idiomatic C-ish way to
  // ping-pong a double buffer without a branch; store() is the atomic
  // write counterpart to load() above.
  write_idx_.store(idx ^ 1, std::memory_order_relaxed);
  return ret;
}

esp_err_t Bsp::wait_frame_flip(uint32_t timeout_ms) {
  if (flip_sem_ == nullptr) {
    return ESP_OK;
  }
  /* exchange : la bascule n'est attendue qu'une fois par present_frame(), même
   * si l'appelant repasse ici avant d'avoir présenté une nouvelle trame. */
  if (!flip_pending_.exchange(false, std::memory_order_relaxed)) {
    return ESP_OK;
  }
  if (xSemaphoreTake(flip_sem_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    /* Panneau arrêté ou trame anormalement longue : rendre la main plutôt que
     * geler la vidéo. L'appelant écrira dans le buffer au risque du tearing. */
    return ESP_ERR_TIMEOUT;
  }
  return ESP_OK;
}

void Bsp::set_direct_mode(bool active) {
  direct_mode_.store(active, std::memory_order_relaxed);
  if (!active && display_) {
    lv_obj_invalidate(lv_screen_active());
  }
}

// --- Carte microSD ---------------------------------------------------------

namespace {

// Le P4 n'a qu'UN contrôleur SDMMC (`SDMMC_LL_HOST_CTLR_NUMS == 1`) pour ses
// deux slots : la carte SD est sur le slot 0, esp_hosted (liaison Wi-Fi vers le
// C6) sur le slot 1. Jusqu'à ESP-IDF 5.4 `sdmmc_host_init()` était idempotent
// (« SDMMC host already initialized, skipping init flow ») et les deux
// cohabitaient sans rien faire de particulier. Depuis 6.0 il appelle sans
// condition `sd_host_create_sdmmc_controller()`, dont `sd_host_claim_controller()`
// cherche une entrée LIBRE : esp_hosted ayant déjà réclamé l'unique contrôleur
// bien avant `app_main`, le montage échoue sur
// « no available sd host controller » / ESP_ERR_NOT_FOUND.
// Contournement de l'ESP-IDF issue #16233 (recommandé par esp_hosted dans
// `examples/host_sdcard_with_hosted`) : ne pas traiter comme fatal le fait que
// le contrôleur soit déjà réclamé.
//
// L'exemple d'Espressif remplace `host.init` par un stub qui renvoie toujours
// ESP_OK. Ici on appelle le vrai `sdmmc_host_init()` et on n'avale QUE
// ESP_ERR_NOT_FOUND : le firmware de test (`test_apps/`) embarque le même BSP
// sans esp_hosted, et un ESP_OK inconditionnel y masquerait un contrôleur
// jamais créé. Toute autre erreur (mémoire, interruption) remonte.
//
// `host.deinit` n'est délibérément PAS touché : `SDMMC_HOST_DEFAULT()` pose
// déjà SDMMC_HOST_FLAG_DEINIT_ARG + `deinit_p = sdmmc_host_deinit_slot`, qui
// retire le seul slot 0 et convertit en ESP_OK l'ESP_ERR_INVALID_STATE que
// renvoie `sd_host_del_controller()` quand le contrôleur sert encore —
// exactement le comportement voulu. Le neutraliser fuiterait le slot 0 et
// ferait échouer tout remontage (`sd_unmount()` sur erreurs d'écriture,
// formatage).
esp_err_t sd_host_init_shared() {
  const esp_err_t err = sdmmc_host_init();
  return (err == ESP_ERR_NOT_FOUND) ? ESP_OK : err;
}

// Le montage et le formatage partagent toute leur configuration, à
// `format_if_mount_failed` près. Facteur commun pour que les deux chemins ne
// puissent pas diverger (fréquence, largeur de bus, taille d'unité).
esp_err_t sd_mount_with(void *pwr_handle, bool format_if_failed,
                        sdmmc_card_t **out_card) {
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  host.pwr_ctrl_handle = static_cast<sd_pwr_ctrl_handle_t>(pwr_handle);
  host.init = &sd_host_init_shared;

  sdmmc_slot_config_t slot_cfg = {};
  slot_cfg.cd = SDMMC_SLOT_NO_CD;
  slot_cfg.wp = SDMMC_SLOT_NO_WP;
  slot_cfg.width = 4;
  slot_cfg.flags = 0;

  esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
  mount_cfg.format_if_mount_failed = format_if_failed;
  mount_cfg.max_files = 5;
  mount_cfg.allocation_unit_size = 64 * 1024;

  return esp_vfs_fat_sdmmc_mount(kSdMountPoint, &host, &slot_cfg, &mount_cfg,
                                 out_card);
}

} // namespace

esp_err_t Bsp::sd_mount() {
  if (sd_state_ == SdState::kMounted) {
    return ESP_OK;
  }

  if (sd_pwr_handle_ == nullptr) {
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {};
    ldo_cfg.ldo_chan_id = kSdLdoChan;
    sd_pwr_ctrl_handle_t handle = nullptr;
    const esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &handle);
    if (err != ESP_OK) {
      sd_last_err_ = err;
      sd_state_ = SdState::kError;
      ESP_LOGE(TAG, "LDO SD (canal %d) : %s", kSdLdoChan, esp_err_to_name(err));
      return err;
    }
    sd_pwr_handle_ = handle;
  }

  sdmmc_card_t *card = nullptr;
  const esp_err_t err = sd_mount_with(sd_pwr_handle_, false, &card);
  sd_last_err_ = err;
  if (err != ESP_OK) {
    sd_card_ = nullptr;
    // ESP_FAIL = montage refusé alors que la carte répond (non formatée,
    // FAT illisible) : distinct d'une carte simplement absente, et c'est ce
    // que l'écran doit dire pour orienter vers le formatage.
    sd_state_ = (err == ESP_FAIL) ? SdState::kError : SdState::kAbsent;
    ESP_LOGW(TAG, "carte SD non montée : %s", esp_err_to_name(err));
    return err;
  }

  sd_card_ = card;
  sd_state_ = SdState::kMounted;
  uint64_t total = 0, freeb = 0;
  (void)sd_capacity(&total, &freeb);
  ESP_LOGI(TAG, "carte SD montée sur %s : %llu Mo, %llu Mo libres",
           kSdMountPoint, (unsigned long long)(total / (1024ULL * 1024)),
           (unsigned long long)(freeb / (1024ULL * 1024)));
  return ESP_OK;
}

esp_err_t Bsp::sd_unmount() {
  if (sd_state_ != SdState::kMounted) {
    return ESP_OK;
  }
  const esp_err_t err = esp_vfs_fat_sdcard_unmount(
      kSdMountPoint, static_cast<sdmmc_card_t *>(sd_card_));
  sd_card_ = nullptr;
  sd_state_ = SdState::kAbsent;
  sd_last_err_ = err;
  return err;
}

esp_err_t Bsp::sd_init_card_only(void **out_card) {
  if (out_card == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_card = nullptr;
  // Le VFS et TinyUSB ne peuvent pas posséder le slot en même temps.
  if (sd_state_ == SdState::kMounted) {
    return ESP_ERR_INVALID_STATE;
  }

  // Même alimentation que sd_mount() : le LDO on-chip 4 (ESP_LDO_VO4 au
  // schéma). Conservé d'un montage à l'autre, on ne le recrée pas s'il existe.
  if (sd_pwr_handle_ == nullptr) {
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {};
    ldo_cfg.ldo_chan_id = kSdLdoChan;
    sd_pwr_ctrl_handle_t handle = nullptr;
    const esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &handle);
    if (err != ESP_OK) {
      sd_last_err_ = err;
      ESP_LOGE(TAG, "LDO SD (canal %d) : %s", kSdLdoChan, esp_err_to_name(err));
      return err;
    }
    sd_pwr_handle_ = handle;
  }

  // Configuration identique à sd_mount_with(), y compris host.init partagé :
  // sans lui, esp_hosted ayant déjà réclamé l'unique contrôleur SDMMC du P4,
  // l'init échouerait sur ESP_ERR_NOT_FOUND (ESP-IDF issue #16233).
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_0;
  host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
  host.pwr_ctrl_handle = static_cast<sd_pwr_ctrl_handle_t>(sd_pwr_handle_);
  host.init = &sd_host_init_shared;

  sdmmc_slot_config_t slot_cfg = {};
  slot_cfg.cd = SDMMC_SLOT_NO_CD;
  slot_cfg.wp = SDMMC_SLOT_NO_WP;
  slot_cfg.width = 4;
  slot_cfg.flags = 0;

  esp_err_t err = (*host.init)();
  if (err != ESP_OK) {
    sd_last_err_ = err;
    ESP_LOGE(TAG, "init hôte SDMMC : %s", esp_err_to_name(err));
    return err;
  }

  err = sdmmc_host_init_slot(host.slot, &slot_cfg);
  if (err != ESP_OK) {
    sd_last_err_ = err;
    ESP_LOGE(TAG, "init slot SDMMC : %s", esp_err_to_name(err));
    return err;
  }

  // calloc et non heap_caps_malloc(SPIRAM) : la structure est petite et le
  // driver SDMMC y écrit depuis des contextes où la PSRAM n'est pas garantie.
  sdmmc_card_t *card =
      static_cast<sdmmc_card_t *>(calloc(1, sizeof(sdmmc_card_t)));
  if (card == nullptr) {
    (void)sdmmc_host_deinit_slot(host.slot);
    sd_last_err_ = ESP_ERR_NO_MEM;
    return ESP_ERR_NO_MEM;
  }

  err = sdmmc_card_init(&host, card);
  if (err != ESP_OK) {
    free(card);
    (void)sdmmc_host_deinit_slot(host.slot);
    sd_last_err_ = err;
    ESP_LOGE(TAG, "sdmmc_card_init : %s", esp_err_to_name(err));
    return err;
  }

  *out_card = card;
  sd_last_err_ = ESP_OK;
  ESP_LOGI(TAG, "carte SD initialisée sans système de fichiers (mode clé USB)");
  return ESP_OK;
}

esp_err_t Bsp::sd_deinit_card_only(void *card) {
  if (card == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }
  // Le slot est retiré explicitement plutôt que par host.deinit_p : on connaît
  // le slot qu'on a ouvert, et sd_mount() saura le rouvrir derrière.
  const esp_err_t err = sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_0);
  free(static_cast<sdmmc_card_t *>(card));
  sd_last_err_ = err;
  return err;
}

esp_err_t Bsp::sd_format() {
  if (sd_state_ == SdState::kMounted) {
    const esp_err_t err = esp_vfs_fat_sdcard_format(
        kSdMountPoint, static_cast<sdmmc_card_t *>(sd_card_));
    sd_last_err_ = err;
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "formatage SD : %s", esp_err_to_name(err));
    }
    return err;
  }

  // Carte non montée (neuve ou système de fichiers cassé) :
  // esp_vfs_fat_sdcard_format() ne sait pas la traiter (elle exige une
  // partition déjà montée). C'est le SEUL chemin autorisé à poser
  // format_if_mount_failed, et il découle d'une confirmation à l'écran.
  if (sd_pwr_handle_ == nullptr) {
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {};
    ldo_cfg.ldo_chan_id = kSdLdoChan;
    sd_pwr_ctrl_handle_t handle = nullptr;
    const esp_err_t err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &handle);
    if (err != ESP_OK) {
      sd_last_err_ = err;
      return err;
    }
    sd_pwr_handle_ = handle;
  }

  sdmmc_card_t *card = nullptr;
  const esp_err_t err = sd_mount_with(sd_pwr_handle_, true, &card);
  sd_last_err_ = err;
  if (err != ESP_OK) {
    sd_state_ = SdState::kError;
    ESP_LOGE(TAG, "formatage + montage SD : %s", esp_err_to_name(err));
    return err;
  }
  sd_card_ = card;
  sd_state_ = SdState::kMounted;
  return ESP_OK;
}

bool Bsp::sd_capacity(uint64_t *total_bytes, uint64_t *free_bytes) {
  if (sd_state_ != SdState::kMounted) {
    return false;
  }
  uint64_t total = 0, freeb = 0;
  if (esp_vfs_fat_info(kSdMountPoint, &total, &freeb) != ESP_OK) {
    return false;
  }
  if (total_bytes) {
    *total_bytes = total;
  }
  if (free_bytes) {
    *free_bytes = freeb;
  }
  return true;
}

} // namespace bsp
