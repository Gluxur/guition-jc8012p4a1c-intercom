#pragma once

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ppa.h"
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

// Étiquette C de esp_cam_ctlr_handle_t (esp_cam_ctlr_types.h). Déclarée ICI,
// au niveau global : écrite dans la classe, `struct esp_cam_ctlr_t *` aurait
// déclaré un bsp::esp_cam_ctlr_t distinct, incompatible avec le handle IDF.
struct esp_cam_ctlr_t;
struct isp_processor_t; // idem pour isp_proc_handle_t (driver/isp_types.h)

namespace bsp {

// État de la carte microSD. kAbsent couvre le cas « pas de carte » comme
// « carte retirée » ; seul sd_last_error() distingue les deux.
enum class SdState : uint8_t { kAbsent, kMounted, kError };

/**
 * Board support package for the JD9365 MIPI-DSI panel + GSL3680 touch.
 *
 * Owns the LCD panel, LVGL display/input devices, backlight, the LVGL
 * mutex and the PPA clients used for rotation/upscaling. The hardware is
 * unique, so the class is a singleton: access it via Bsp::instance().
 */
class Bsp {
public:
  // constexpr, not const: guarantees compile-time evaluation, usable as
  // array bound / template arg. Closest C# equivalent is a `const int`
  // field, but C# has no compile-time-only guarantee like this.
  static constexpr int32_t kLcdHRes = 800;
  static constexpr int32_t kLcdVRes = 1280;

  // No public ctor (see private Bsp() below) + instance() returning a
  // function-local static reference = the "Meyers singleton". Unlike a C#
  // singleton with a static field initialized eagerly or via a lock, the
  // local static here is lazily constructed on first call AND its
  // initialization is thread-safe by C++11 guarantee (compiler emits a
  // hidden guard variable) — no manual double-checked locking needed.
  static Bsp &instance();

  // Rule of five: copy AND move are explicitly deleted. This type wraps
  // unique hardware (one panel, one I2C bus...), so copying an instance
  // would duplicate handles that must only be closed once. C# has no
  // by-value copy semantics for classes (reference type by default) and no
  // move semantics at all, so this whole block has no C# counterpart —
  // in C# "can't be copied" is just "don't implement ICloneable".
  Bsp(const Bsp &) = delete;
  Bsp &operator=(const Bsp &) = delete;
  Bsp(Bsp &&) = delete;
  Bsp &operator=(Bsp &&) = delete;

  // [[nodiscard]] is a compiler-enforced version of "don't ignore this
  // return value" — closest C# analogue is a Roslyn analyzer warning on
  // an unused result, but here it's a hard build warning/error from the
  // language itself. esp_err_t is a plain enum/int (ESP-IDF has no
  // exceptions), so every fallible call needs its result checked manually
  // — there's no try/catch to fall back on if you forget.
  /** Initialise panel, backlight, LVGL, touch. Must be called once before
   * anything else. */
  [[nodiscard]] esp_err_t init();

  [[nodiscard]] lv_display_t *display() const { return display_; }
  [[nodiscard]] lv_indev_t *touch_indev() const { return touch_indev_; }
  [[nodiscard]] esp_lcd_panel_handle_t panel() const { return panel_; }

  /** Backlight brightness, 0–100 %. Values above 100 are clamped. Applied live
   * and, if `persist`, written to NVS. Sliders pass persist=false while the
   * finger moves and true on release: an NVS commit suspends the flash cache
   * and stalls the OTHER core (no SPI_FLASH_AUTO_SUSPEND, no XIP from PSRAM),
   * i.e. the MJPEG drain and aud_render, once per slider pixel. */
  void set_backlight(uint8_t brightness_pct, bool persist = true);

  /** Blank the panel without touching the stored brightness. For power-off /
   * deep sleep only — set_backlight(0) would persist the blanking. */
  void backlight_off();

  /** Apply a brightness WITHOUT persisting it and without overwriting the
   * remembered value. For transient states only: HA screen-off, full
   * brightness while a call is on screen. Going through set_backlight() would
   * write NVS twice per call (flash wear) and, if power were lost mid-call,
   * would leave the user's own setting overwritten for good. */
  void set_backlight_temporary(uint8_t brightness_pct);

  /** Leave a transient state: re-apply the remembered brightness. */
  void restore_backlight();

  /** True while the panel is actually lit. NOT deducible from backlight():
   * that accessor returns the remembered setting, which blanking leaves
   * untouched on purpose. Owned here so that no two modules keep rival copies
   * of the panel state. */
  [[nodiscard]] bool display_on() const {
    return display_on_.load(std::memory_order_relaxed);
  }

  /** Arm/disarm wake-on-touch. Armed by default: touching a dark panel
   * restores the remembered brightness and that first touch is swallowed, so
   * it cannot press whatever sits under the finger on a screen the user could
   * not see. Disarm before a deliberate blanking that must STAY dark — the
   * power-off button blanks then waits ~2.5 s before deep sleep, and a stray
   * touch would light the panel back up in the meantime. */
  void set_touch_wake_enabled(bool enabled) { touch_wake_enabled_ = enabled; }

  /** Speaker output volume, 0–100. Applied live; written to NVS only if
   * `persist` (same slider contract as set_backlight). */
  void set_speaker_volume(uint8_t volume_pct, bool persist = true);

  /** Mic input gain, 0–100 UI scale (mapped internally to 0–48dB). Applied
   * live; written to NVS only if `persist` (same contract as set_backlight). */
  void set_mic_gain(uint8_t gain_pct, bool persist = true);

  /** Last-applied speaker volume, 0–100. */
  [[nodiscard]] uint8_t speaker_volume() const { return speaker_volume_pct_; }

  /** Last-applied mic gain, 0–100 UI scale. */
  [[nodiscard]] uint8_t mic_gain() const { return mic_gain_pct_; }

  /** Last-applied backlight brightness, 0–100 %. */
  [[nodiscard]] uint8_t backlight() const { return backlight_pct_; }

  /** Take/release the recursive LVGL mutex. timeout_ms == 0 → wait forever.
   * Prefer LvglLock.
   * lock() renvoie false si le délai a expiré : le mutex n'est alors PAS tenu
   * et unlock() ne doit pas être appelé. Ignorer ce retour puis relâcher
   * quand même décrémentait le compteur de profondeur à -1 et rendait un mutex
   * non possédé — c'est exactement ce que LvglLock évite désormais. */
  bool lock(uint32_t timeout_ms = 0);
  void unlock();

  /** DIAGNOSTIC : plus longue détention du mutex LVGL (µs) depuis le dernier
   * appel, et nom de la tâche qui la détenait ; remet le maximum à zéro.
   * Sert à identifier qui bloque la tâche de rendu vidéo dans present_frame().
   * Seule la prise EXTERNE est mesurée (le mutex est récursif).
   * `owner` peut être nullptr. Le nom est copié sans verrou : sur égalité de
   * course il peut désigner la mauvaise tâche, ce qui est acceptable pour une
   * mesure lue une fois par seconde. */
  uint32_t lvgl_max_hold_us(char *owner, size_t owner_len);

  /** Nombre de transactions PPA du flush LVGL restées sans acquittement (cf.
   * ppa_lvgl_done_). Doit valoir 0 : toute valeur non nulle signale que le
   * moteur SRM perd des fins de transaction. */
  [[nodiscard]] uint32_t ppa_lvgl_timeouts() const {
    return ppa_lvgl_timeouts_.load(std::memory_order_relaxed);
  }

  /** Idem pour le client vidéo (cf. ppa_video_run). Même contrat : 0 attendu,
   * toute valeur non nulle est le symptôme AVANT-COUREUR d'un moteur SRM qui
   * se verrouille — c'est-à-dire du gel total de l'appareil. */
  [[nodiscard]] uint32_t ppa_video_timeouts() const {
    return ppa_video_timeouts_.load(std::memory_order_relaxed);
  }

  /**
   * Hardware (PPA) render of a packed-YUV420 frame (P4 O_UYY_E_VYY layout,
   * see yuv::i420_to_p4_packed) straight into the DSI framebuffer:
   * color-space conversion + aspect-preserving upscale (1/16 steps) +
   * centering in a single PPA op — no intermediate RGB565 buffer, no
   * draw_bitmap blit. Takes the LVGL mutex internally. Blocking — call
   * only from the video render task (dedicated video PPA client).
   * The input buffer must be PPA-DMA aligned (64 bytes).
   */
  /**
   * Same as above, but the scale-to-fit + centering targets the physical
   * sub-rectangle [dst_x, dst_x+dst_w) x [dst_y, dst_y+dst_h) instead of the
   * full panel. Caller must ensure dst_x+dst_w <= kLcdHRes and
   * dst_y+dst_h <= kLcdVRes.
   */
  esp_err_t render_yuv420_packed_to_panel(const void *packed, uint32_t w,
                                          uint32_t h, uint32_t dst_x,
                                          uint32_t dst_y, uint32_t dst_w,
                                          uint32_t dst_h);

  /**
   * Variante RGB565 pour le pipeline MJPEG : le décodeur JPEG matériel sort du
   * RGB565, plus besoin de conversion couleur (contrairement au YUV420 du
   * décodage H.264). Le PPA ne fait que scale aspect-preserving + centrage dans
   * [dst_x, dst_x+dst_w) x [dst_y, dst_y+dst_h) -> framebuffer DSI courant.
   * `rgb565` : buffer source (w x h), aligné cache (sortie JPEG HW en PSRAM).
   * `src_stride` : largeur réelle en pixels d'une ligne du buffer source. Le
   * décodeur JPEG HW écrit avec un stride aligné 16 px ((w+15)&~15), qui diffère
   * de `w` quand la largeur n'est pas multiple de 16 -> sans ça le PPA lit chaque
   * ligne décalée = image tuilée/cisaillée. 0 = utiliser `w` (source contiguë).
   */
  esp_err_t render_rgb565_to_panel(const void *rgb565, uint32_t w, uint32_t h,
                                   uint32_t dst_x, uint32_t dst_y,
                                   uint32_t dst_w, uint32_t dst_h,
                                   uint32_t src_stride = 0);

  /**
   * Recopie 1:1 (ni échelle, ni centrage, ni conversion) d'un bloc RGB565 en
   * PSRAM vers [dst_x, dst_x+w) x [dst_y, dst_y+h) du back-buffer courant, par
   * le PPA. Pour les superpositions que le chemin vidéo direct écrase à chaque
   * frame (bandeau de contrôles de l'écran caméra V2).
   * Pourquoi le PPA et pas un memcpy CPU : le framebuffer est relu par le
   * scan-out DSI, donc chaque ligne recopiée par le CPU exige un
   * esp_cache_msync — 1280 appels par frame, mesurés à ~20 ms, soit plus que le
   * décodage JPEG matériel lui-même. Le DMA du PPA n'a pas ce problème.
   * `rgb565` doit être aligné 64 o ET déjà rendu à la mémoire par l'écrivain
   * (esp_cache_msync C2M) : le PPA le lit en DMA, pas à travers le cache CPU.
   * Bloquant, même client PPA que render_rgb565_to_panel() -> même tâche
   * appelante (rendu vidéo).
   * Ne prend PAS le mutex LVGL : réservé au direct_mode, où LVGL n'écrit rien
   * dans le framebuffer (cf. direct_video_target). Ne pas appeler hors
   * direct_mode, la superposition entrerait en course avec le flush LVGL.
   */
  esp_err_t blit_rgb565_to_panel(const void *rgb565, uint32_t w, uint32_t h,
                                 uint32_t dst_x, uint32_t dst_y);

  /**
   * Chemin ZÉRO COPIE du pipeline MJPEG : quand la source sort déjà à la
   * largeur du panneau et tient dans la zone cible sans mise à l'échelle, le
   * PPA ne fait plus qu'un memcpy déguisé — mesuré à 22 ms pour un 800x640
   * RGB565, soit le poste dominant du chemin chaud (dec=5 ms). Or si le stride
   * source vaut celui du framebuffer, les lignes décodées tombent PILE sur les
   * lignes du framebuffer : le décodeur JPEG matériel peut écrire directement
   * dedans et le PPA sort du chemin.
   * Renvoie l'adresse de destination dans le back-buffer courant et remplit
   * `out_size` avec la taille exacte à passer à jpeg_decoder_process().
   * Renvoie nullptr si la géométrie ne s'y prête pas (largeur différente, mise
   * à l'échelle nécessaire, dimensions non alignées 16) — l'appelant retombe
   * alors sur render_rgb565_to_panel().
   * Ne prend PAS le mutex LVGL : en direct_mode LVGL n'écrit rien dans le
   * framebuffer, et les sprites de contrôle visent des lignes disjointes de la
   * zone vidéo (donc des lignes de cache disjointes : 1 ligne = 1600 o).
   */
  [[nodiscard]] uint8_t *direct_video_target(uint32_t w, uint32_t h,
                                             uint32_t dst_x, uint32_t dst_y,
                                             uint32_t dst_w, uint32_t dst_h,
                                             size_t *out_size);

  /** Current back-buffer (RGB565, kLcdHRes x kLcdVRes), valid after init().
   * Video/overlay writers (PPA and CPU) target this one; it is never the
   * buffer currently scanned out to the panel. CPU writes into it (stats
   * overlay) must be followed by esp_cache_msync(...,
   * ESP_CACHE_MSYNC_FLAG_DIR_C2M). Call present_frame() once the frame's
   * writes are complete to flip it to the panel and rotate to the other
   * buffer for the next frame. */
  // std::atomic<int>::load with memory_order_relaxed: reads the index
  // atomically (no torn read across threads/cores) but with NO ordering
  // guarantee about surrounding memory operations — cheapest possible
  // atomic read, safe here because it just selects which raw buffer
  // pointer to hand back, it doesn't synchronize buffer *contents*.
  // C# equivalent would be Volatile.Read, but that implies acquire
  // semantics, which is actually stronger than what's used here.
  [[nodiscard]] void *dsi_frame_buffer() const {
    return fbs_[write_idx_.load(std::memory_order_relaxed)];
  }

  /** Index (0/1) du back-buffer courant — apparié à dsi_frame_buffer().
   * Permet à l'overlay de tenir un état par buffer (quel sprite a déjà été
   * blendé dans lequel des deux buffers DSI). */
  [[nodiscard]] int dsi_write_index() const {
    return write_idx_.load(std::memory_order_relaxed);
  }

  /** Raw pointer to DSI buffer `idx` (0 or 1), independent of which one is
   * currently the write target. Lets callers write into a SPECIFIC physical
   * buffer (not just "the current one") — needed to keep static content
   * (camera-screen controls) in sync across both buffers as present_frame()
   * flips between them. */
  [[nodiscard]] void *dsi_frame_buffer_at(int idx) const { return fbs_[idx]; }

  /** Buffer actuellement scanné par le DSI — l'INVERSE de dsi_frame_buffer().
   * C'est la cible du rendu LVGL en mode PARTIAL : LVGL ne repeint que l'aire
   * invalidée, donc écrire dans le back-buffer afficherait un écran dont le
   * reste est périmé. Suit `cur_fb_index` du driver DPI, armé par
   * present_frame() ; vaut fbs_[0] tant qu'aucune trame n'a été présentée. */
  [[nodiscard]] void *dsi_shown_frame_buffer() const {
    return fbs_[shown_idx_.load(std::memory_order_relaxed)];
  }

  /** Rectangle (coords physiques panneau) couvert par la dernière frame
   * vidéo écrite par render_yuv420_packed_to_panel(). w == 0 tant qu'aucune
   * frame n'a été rendue. Écrit et lu uniquement depuis la tâche de rendu
   * vidéo — pas d'atomics nécessaires. */
  void video_block_rect(uint32_t &x, uint32_t &y, uint32_t &w,
                        uint32_t &h) const {
    x = video_rect_x_;
    y = video_rect_y_;
    w = video_rect_w_;
    h = video_rect_h_;
  }

  /**
   * Flip the current back-buffer (dsi_frame_buffer()) to the panel and
   * rotate to the other buffer for the next frame's writes. Two
   * framebuffers (num_fbs=2) let the video render task always write into
   * the buffer that is NOT being scanned out, eliminating the tearing that
   * a single shared framebuffer causes when overlay/video writes race the
   * live DSI scan-out. Ne PAS supposer que la bascule est prise au retour :
   * elle est seulement armée (cf. wait_frame_flip, à appeler avant la
   * première écriture de la frame suivante). Call only from the video render
   * task, after all writes to dsi_frame_buffer() for this frame are done.
   */
  esp_err_t present_frame();

  /**
   * Bloque jusqu'à ce que la bascule demandée par le dernier present_frame()
   * soit RÉELLEMENT prise par le panneau, puis rend la main (retour immédiat
   * si elle l'est déjà, ou si aucun present_frame n'est en attente).
   *
   * esp_lcd_panel_draw_bitmap() ne bascule rien quand on lui passe un pointeur
   * de fbs_[] : il se contente d'armer `cur_fb_index`, et le DMA DSI ne repart
   * sur le nouveau framebuffer qu'à la FIN de la trame en cours (jusqu'à
   * ~31 ms ici : 880 x 1412 px à 40 MHz). L'ancien buffer — celui que
   * present_frame() vient de rendre inscriptible — reste donc à l'écran
   * pendant tout ce temps. À appeler avant la première écriture du back-buffer
   * d'une frame, sinon le contenu en cours de composition apparaît à l'écran.
   */
  esp_err_t wait_frame_flip(uint32_t timeout_ms = 100);

  /**
   * Blocking write of mono 16-bit PCM at 8kHz to the ES8311 speaker.
   * Safe to call from any task. Returns ESP_ERR_INVALID_STATE if the
   * codec failed to initialise (audio is optional; caller should drop
   * the frame and continue).
   */
  esp_err_t play_pcm(const int16_t *pcm, size_t samples);

  /**
   * Blocking read of mono 16-bit PCM at 8kHz from the ES8311 mic.
   * Safe to call from any task. Returns ESP_ERR_INVALID_STATE if the
   * codec failed to initialise.
   */
  esp_err_t read_pcm(int16_t *pcm, size_t samples);

  /**
   * Référence d'écho MATÉRIELLE : l'ES8311 place déjà la sortie de son DAC sur
   * le slot droit de l'I2S (registre 0x44 = 0x58, défaut d'esp_codec_dev avec
   * `no_dac_ref = false`), échantillon pour échantillon avec le micro du slot
   * gauche. C'est la seule référence correctement alignée : celle qu'on
   * pousserait depuis play_pcm part jusqu'à 180 ms (profondeur DMA TX) avant
   * d'être jouée, au-delà de la portée du filtre AEC.
   *
   * `on` passe la capture en stéréo, `off` revient au mono (slot gauche). Ne
   * reconfigure que le canal RX — le HP n'est pas touché. Le DMA RX double
   * (+2,9 Ko de SRAM interne), d'où un mode réservé à la durée de l'AEC.
   * À appeler depuis la tâche qui lit le micro, JAMAIS pendant une lecture.
   */
  esp_err_t set_mic_reference(bool on);

  /**
   * Comme read_pcm, mais rend aussi la référence (ce que joue le HP) dans
   * `ref`. Exige set_mic_reference(true), sinon ESP_ERR_INVALID_STATE.
   */
  esp_err_t read_pcm_ref(int16_t *mic, int16_t *ref, size_t samples);

  /**
   * Monte la carte microSD (slot 0 SDMMC, broches IOMUX, bus 4 bits,
   * alimentation par le LDO on-chip 4 — cf. ESP_LDO_VO4 au schéma) sur
   * /sdcard. Ne formate JAMAIS automatiquement : une carte illisible peut
   * contenir les données de quelqu'un, seul sd_format() l'efface.
   * Le slot 1 porte la liaison Wi-Fi esp_hosted : ne pas y toucher.
   * Échoue proprement sans carte, l'appelant poursuit son boot.
   */
  [[nodiscard]] esp_err_t sd_mount();

  /** Démonte le système de fichiers. Le contrôle d'alimentation LDO est
   * conservé pour un éventuel remontage. */
  [[nodiscard]] esp_err_t sd_unmount();

  /**
   * Formate la carte en FAT puis la remonte. DESTRUCTIF : réservé à une
   * action utilisateur explicitement confirmée.
   * Deux chemins, parce qu'esp_vfs_fat_sdcard_format() exige une partition
   * DÉJÀ montée (ESP_ERR_INVALID_STATE sinon) — ce qui exclut la carte neuve
   * ou corrompue. Dans ce seul cas on remonte avec format_if_mount_failed.
   */
  [[nodiscard]] esp_err_t sd_format();

  /**
   * Initialise le MATÉRIEL de la carte (hôte SDMMC slot 0, LDO,
   * sdmmc_card_init) SANS enregistrer aucun système de fichiers. Destiné au
   * mode clé USB, où c'est esp_tinyusb qui possède le montage FAT pendant
   * toute la session.
   *
   * Refuse (ESP_ERR_INVALID_STATE) si le système de fichiers est monté : deux
   * propriétaires du même slot, c'est de la corruption programmée. Appeler
   * sd_unmount() d'abord.
   *
   * `out_card` reçoit un sdmmc_card_t* alloué ici, à rendre par
   * sd_deinit_card_only(). void* et non le vrai type : sdmmc_card_t est un
   * typedef de struct ANONYME, non déclarable en avant — même raison que le
   * membre sd_card_.
   */
  [[nodiscard]] esp_err_t sd_init_card_only(void **out_card);

  /** Libère la carte allouée par sd_init_card_only et rend le slot 0. */
  [[nodiscard]] esp_err_t sd_deinit_card_only(void *card);

  [[nodiscard]] SdState sd_state() const { return sd_state_; }
  [[nodiscard]] esp_err_t sd_last_error() const { return sd_last_err_; }

  /** Capacité et espace libre, en octets. false si la carte n'est pas
   * montée. Les deux pointeurs peuvent être nullptr. */
  [[nodiscard]] bool sd_capacity(uint64_t *total_bytes, uint64_t *free_bytes);

  // --- Caméra OV02C10 (MIPI-CSI), pour la détection de présence ------------
  // Image BRUTE uniquement : RAW10 compacté MIPI (5 octets = 4 pixels, les 4
  // premiers portent les 8 bits de poids fort), motif de Bayer GBRG. Pas
  // d'ISP : sur ce silicium pré-v3 le pont CSI ne convertit rien
  // (esp_cam_ctlr_csi.c refuse toute conversion sous la rév. 3.0), et une
  // différence d'images n'a besoin que de la luminance approchée.
  // 1280x720 et non les 1288x728 natifs de la table : cf. bsp_camera.cpp
  // (la taille d'image doit être multiple de la ligne de cache, 64 o).
  static constexpr int kCamWidth = 1280;
  static constexpr int kCamHeight = 720;
  static constexpr size_t kCamFrameBytes =
      static_cast<size_t>(kCamWidth) * kCamHeight * 10 / 8;
  static_assert(kCamFrameBytes % 64 == 0,
                "le driver CSI exige une image multiple de la ligne de cache");

  /** Initialise le capteur (I2C 0x36) et le contrôleur CSI, flux à ~5 img/s.
   * Alloue ~1,1 Mo de PSRAM (tampon de secours du driver). Réentrant : sans
   * effet si déjà démarrée. */
  [[nodiscard]] esp_err_t camera_start();
  /** Coupe le flux du capteur et libère le contrôleur CSI. */
  [[nodiscard]] esp_err_t camera_stop();
  [[nodiscard]] bool camera_running() const { return cam_ctlr_ != nullptr; }
  /** Remplit `buf` (kCamFrameBytes, aligné 64, PSRAM) avec la prochaine image
   * complète. Les images produites entre deux appels sont jetées par le
   * driver : on ne paie que celles qu'on lit. */
  [[nodiscard]] esp_err_t camera_capture(uint8_t *buf, size_t len,
                                         uint32_t timeout_ms);
  /** Exposition en lignes (bornée à camera_max_exposure_lines()) et gain
   * analogique en 1/16 (16 = x1, borné à x15,5). */
  [[nodiscard]] esp_err_t camera_set_exposure(uint32_t lines,
                                              uint32_t gain_x16);
  [[nodiscard]] static uint32_t camera_max_exposure_lines();
  /** Trames démarrées par le DMA CSI depuis le boot, lues ou non. Diagnostic :
   * un compteur figé = le capteur n'émet rien (ou le CSI ne le comprend pas). */
  [[nodiscard]] static uint32_t camera_frame_count();
  /** Une ligne de log : état de la liaison MIPI (P4) et du capteur. Chemin
   * froid uniquement (diagnostic d'une capture qui n'aboutit pas). */
  void camera_log_link_state();

  /**
   * Enable/disable direct-draw mode. While active, the LVGL flush callback
   * skips draw_bitmap so camera frames have exclusive DSI access.
   * Call with active=false to force a full LVGL redraw (e.g. when showing
   * errors).
   */
  void set_direct_mode(bool active);

  /** Attend la transaction PPA laissée en vol par le flush LVGL, s'il y en a
   * une. Appelée en tête de chaque soumission (ppa_lvgl_run) ET à la fin de
   * chaque cycle de rafraîchissement (LV_EVENT_REFR_READY) : l'invariant
   * « aucune transaction du flush n'est en vol hors d'un refresh » est ce qui
   * garantit qu'aucune autre tâche (bascule de framebuffer vidéo) ne peut
   * observer un framebuffer en cours d'écriture.
   * Publique uniquement pour le callback REFR_READY, qui est une fonction
   * libre. RÉSERVÉE À lvgl_task : ppa_lvgl_pending_ n'est ni atomique ni
   * protégé, et l'appeler d'ailleurs corromprait le décompte. */
  esp_err_t ppa_lvgl_wait_pending();

  // RAII lock guard — the C++ pattern that replaces C#'s `using (var
  // l = ...)` / IDisposable. There's no `using` statement in C++; instead
  // the mutex is released automatically by the destructor when `lock`
  // goes out of scope, including on early `return` or exception unwind.
  // `explicit` blocks implicit conversion from a bare uint32_t to
  // LvglLock (no accidental `SomeFn(42)` matching a LvglLock parameter).
  // Copy is deleted because releasing the same mutex twice (once per
  // copy's destructor) would be a bug — same rationale as Bsp itself.
  /** RAII guard for the LVGL mutex. timeout_ms == 0 → wait forever.
   * `held()` dit si la prise a réussi : avec un délai non nul, le destructeur
   * ne relâche que ce qui a réellement été pris. */
  class LvglLock {
  public:
    explicit LvglLock(uint32_t timeout_ms = 0)
        : held_(Bsp::instance().lock(timeout_ms)) {}
    ~LvglLock() {
      if (held_) {
        Bsp::instance().unlock();
      }
    }
    [[nodiscard]] bool held() const { return held_; }
    LvglLock(const LvglLock &) = delete;
    LvglLock &operator=(const LvglLock &) = delete;

  private:
    bool held_;
  };

private:
  // Private ctor is what makes `instance()` the only way to get a Bsp —
  // there's no C# "private constructor + static accessor" quirk here
  // beyond the pattern itself, but note `= default` asks the compiler to
  // generate the trivial ctor body instead of writing `Bsp() {}` by hand.
  Bsp() = default;

  // ESP-IDF C APIs (LVGL, i2s, etc.) take a plain function pointer for
  // callbacks, not a bound method/delegate — a non-static C++ member
  // function has an implicit `this` parameter and its pointer type is
  // incompatible with a C function pointer. These `static` methods are
  // "trampolines": free-function-shaped, but they recover the owning Bsp
  // instance from a `void* user_data`/similar the C API round-trips back
  // to them, then forward to the real (non-static) handler below. C#'s
  // delegates/closures capture `this` for you automatically; here it has
  // to be threaded through manually because a static member function has
  // no implicit `this`.
  /* LVGL C callbacks: trampolines recover `this` from the LVGL user data. */
  static uint32_t tick_cb();
  static void flush_cb(lv_display_t *disp, const lv_area_t *area,
                       uint8_t *px_map);
  static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

  void handle_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
  void handle_touch_read(lv_indev_data_t *data);

  /* Exécute une opération SRM sur le client vidéo en NON_BLOCKING avec attente
   * bornée, et retombe sur le mode bloquant si le sémaphore n'a pas pu être
   * installé au boot. Factorisé : les trois entrées vidéo (yuv420, rgb565,
   * blit) partagent exactement la même séquence. */
  esp_err_t ppa_video_run(ppa_srm_oper_config_t &srm);

  /* Même séquence pour le client LVGL, appelée une fois par sous-bande quand
   * le contournement DIG-734 impose de découper le flush (ppa_srm_quirk.h).
   * `w`/`h` ne servent qu'au log de timeout : c'est la géométrie qui identifie
   * une bande fautive.
   * Contrairement à ppa_video_run, l'attente n'est PAS faite ici : la
   * transaction est soumise puis laissée en vol, et c'est l'appel SUIVANT (ou
   * ppa_lvgl_wait_pending) qui l'attend. C'est ce décalage qui fait travailler
   * le rendu logiciel LVGL de la bande n+1 PENDANT que le DMA du PPA traite la
   * bande n — auparavant les deux étaient strictement en série. Il exige le
   * double draw buffer LVGL (cf. init_lvgl) : sans lui, LVGL réécrirait le
   * px_map que le PPA est encore en train de lire. */
  esp_err_t ppa_lvgl_run(ppa_srm_oper_config_t &srm, uint32_t w, uint32_t h);

  esp_err_t init_panel();
  esp_err_t init_lvgl();
  esp_err_t init_touch();
  esp_err_t init_audio();
  void load_prefs();

  // Every handle below is a raw pointer (nullptr default like C#'s null),
  // but unlike a C# reference these are NOT garbage-collected: each one
  // is an opaque handle to a C-allocated resource (LVGL display, FreeRTOS
  // semaphore, ESP-IDF driver handles...) that must be explicitly torn
  // down in bsp.cpp's cleanup paths. No finalizers exist to catch a
  // leaked handle if you forget.
  lv_display_t *display_ = nullptr;
  lv_indev_t *touch_indev_ = nullptr;
  SemaphoreHandle_t lvgl_mux_ = nullptr;
  // Instrumentation du mutex LVGL. lvgl_hold_depth_ / lvgl_hold_start_us_ ne
  // sont touchés que par la tâche PROPRIÉTAIRE, entre take et give : pas
  // d'atomique nécessaire. Le maximum, lui, est écrit par n'importe quelle
  // tâche et lu par la tâche de rendu -> atomique.
  int lvgl_hold_depth_ = 0;
  int64_t lvgl_hold_start_us_ = 0;
  std::atomic<uint32_t> lvgl_max_hold_us_{0};
  char lvgl_max_hold_owner_[16] = {};
  /* Sémaphore de fin de transaction PPA du client LVGL. Le mode BLOCKING du
   * driver attend SANS TIMEOUT (ppa_core.c) : une transaction DMA2D qui ne
   * signale jamais sa fin gelait lvgl_task, et avec elle tout l'écran. On passe
   * donc en NON_BLOCKING + attente bornée ici. */
  SemaphoreHandle_t ppa_lvgl_done_ = nullptr;
  std::atomic<uint32_t> ppa_lvgl_timeouts_{0};
  /* Une transaction du flush a été soumise et pas encore acquittée. Écrit et lu
   * par lvgl_task SEULEMENT (handle_flush + le callback REFR_READY), d'où le
   * bool nu : un atomique laisserait croire que d'autres tâches y touchent. */
  bool ppa_lvgl_pending_ = false;
  /* L'acquittement d'une transaction expirée peut encore arriver après coup et
   * laisser le sémaphore armé ; la soumission suivante le purgerait alors sans
   * attendre quoi que ce soit. On note la dette ici pour la solder à ce
   * moment-là. */
  bool ppa_lvgl_stale_ack_ = false;
  /* Idem pour le client vidéo. Deux clients PPA, mais UN SEUL moteur SRM
   * (ppa_core.c) donc un seul sémaphore de moteur, rendu uniquement par l'ISR
   * de fin de transaction : une transaction perdue par le flush LVGL laissait
   * le moteur verrouillé, et le chemin vidéo — resté bloquant, mutex LVGL tenu
   * — gelait alors lvgl_task par l'autre bout. Le gel que le NON_BLOCKING du
   * flush prétendait supprimer restait donc atteignable. */
  SemaphoreHandle_t ppa_video_done_ = nullptr;
  std::atomic<uint32_t> ppa_video_timeouts_{0};
  esp_lcd_touch_handle_t touch_ = nullptr;
  esp_lcd_panel_handle_t panel_ = nullptr;
  i2c_master_bus_handle_t i2c_bus_ =
      nullptr; /* shared: touch (GSL3680) + audio codec (ES8311) */
  i2s_chan_handle_t audio_tx_chan_ = nullptr;
  i2s_chan_handle_t audio_rx_chan_ = nullptr;
  esp_codec_dev_handle_t audio_codec_ = nullptr;
  esp_codec_dev_handle_t audio_input_codec_ = nullptr;
  /* Capture stéréo (micro + référence DAC) active : lu et écrit par la seule
   * tâche lectrice du micro, d'où l'absence d'atomique. */
  bool mic_ref_ = false;
  /* Trames entrelacées gauche/droite de read_pcm_ref, PSRAM, allouées au
   * premier set_mic_reference(true) et conservées ensuite (1 Ko). */
  int16_t *rx_stereo_ = nullptr;
  uint8_t speaker_volume_pct_ = 50;
  uint8_t mic_gain_pct_ = 83;
  uint8_t backlight_pct_ = 100;
  /* Panel actually lit. Distinct from backlight_pct_, which is the REMEMBERED
   * setting and survives blanking on purpose. Atomic: written from the LVGL
   * task (touch wake), the ESPHome API task (HA light entity) and cam_defer /
   * cam_stop (full brightness during a call). */
  std::atomic<bool> display_on_{true};

  /* Wake-on-touch and its one-shot swallow latch. Both are read and written
   * only from the LVGL task (handle_touch_read, cb_power_off), hence plain
   * bools. */
  bool touch_wake_enabled_ = true;
  bool touch_wake_swallow_ = false;

  /* Sole writer of the LEDC duty. Keeps display_on_ in step with the panel, so
   * that persisting (set_backlight) and not persisting (backlight_off,
   * set_backlight_temporary) differ only by what they do around it. */
  void apply_backlight_duty(uint8_t brightness_pct);

  void *rot_buf_ = nullptr; /* PPA rotation output, one LVGL band */
  size_t rot_buf_size_ = 0;
  ppa_client_handle_t ppa_lvgl_ =
      nullptr; /* used by handle_flush (LVGL task) */
  ppa_client_handle_t ppa_video_ =
      nullptr; /* used by render_yuv420_packed_to_panel (video task) */
  void *fbs_[2] = {nullptr, nullptr}; /* DSI double buffer, owned by esp_lcd DPI panel */
  /* 1 et non 0 : le driver DPI démarre en scannant fbs[0]. À 0, la première
   * frame vidéo (ou le premier render_still) après le boot s'écrivait dans le
   * buffer AFFICHÉ, sous le scan-out et sous le flush LVGL. */
  std::atomic<int> write_idx_{1}; /* index into fbs_ safe to write (not on screen) */
  /* Miroir de `cur_fb_index` du driver DPI (0 au démarrage) : le buffer à
   * l'écran, cible du flush LVGL partiel. Ne pas le déduire de write_idx_ —
   * les deux ne sont opposés qu'APRÈS le premier present_frame(). */
  std::atomic<int> shown_idx_{0};
  /* Donné par l'ISR de fin de trame DSI (on_refresh_done), attendu par
   * wait_frame_flip(). Binaire : seule la PREMIÈRE fin de trame après un
   * present_frame() nous intéresse. */
  SemaphoreHandle_t flip_sem_ = nullptr;
  std::atomic<bool> flip_pending_{false};
  /* Dernier bloc vidéo écrit (coords physiques) — tâche de rendu vidéo
   * uniquement, cf video_block_rect(). */
  uint32_t video_rect_x_ = 0, video_rect_y_ = 0;
  uint32_t video_rect_w_ = 0, video_rect_h_ = 0;

  std::atomic<bool> direct_mode_{false};

  // Handles SD gardés en void* : tirer sd_protocol_types.h ici l'imposerait à
  // tous les consommateurs de bsp.h alors qu'il ne sert qu'à bsp.cpp. Et
  // sdmmc_card_t est un typedef de struct ANONYME — il n'est pas déclarable
  // en avant, void* est la seule option.
  void *sd_card_ = nullptr;      /* sdmmc_card_t* */
  void *sd_pwr_handle_ = nullptr; /* sd_pwr_ctrl_handle_t */
  SdState sd_state_ = SdState::kAbsent;
  esp_err_t sd_last_err_ = ESP_OK;

  // Caméra : handle CSI en type opaque déclaré en avant (esp_cam_ctlr_t), pour
  // ne pas imposer esp_driver_cam à tous les inclueurs de bsp.h.
  ::esp_cam_ctlr_t *cam_ctlr_ = nullptr;
  ::isp_processor_t *cam_isp_ = nullptr; // ISP en bypass, cf. bsp_camera.cpp
  i2c_master_dev_handle_t cam_sccb_ = nullptr;
  SemaphoreHandle_t cam_frame_done_ = nullptr;
  /* Un tampon est dans la file du driver (ou en cours d'écriture DMA). Une
   * capture expirée le laisse en vol : le remettre en file échouerait (file
   * d'un élément), on attend donc simplement sa fin à l'appel suivant. Seule
   * la tâche appelante y touche, d'où l'absence d'atomique. */
  bool cam_pending_ = false;
};

} // namespace bsp
