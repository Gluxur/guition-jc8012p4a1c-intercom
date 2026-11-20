#include "bsp.h"

#include "driver/isp_core.h"
#include "esp_attr.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "soc/mipi_csi_bridge_struct.h"
#include "soc/mipi_csi_host_struct.h"

#include "ov02c10_init.h"

#include <atomic>

namespace bsp {

namespace {

constexpr const char *TAG = "bsp_cam";

// Adresse SCCB mesurée au scan I2C du boot (ID 0x560243 lu en 0x300A..C) :
// même bus que le tactile et l'ES8311, le driver I2C sérialise les accès.
constexpr uint16_t kSccbAddr = 0x36;
constexpr uint32_t kSccbHz = 100000;

// Débit de la lane MIPI fixé par la PLL de la table Guition (IDI 100 MHz x 4).
constexpr int kLaneMbps = 400;

// VTS = 6 x 1164 : la table tourne à 30 img/s, on en veut ~5. Allonger la
// trame plutôt que laisser le driver jeter 5 images sur 6 : le capteur, la
// PHY et le DMA travaillent 6 fois moins, et l'exposition maximale passe de
// ~32 à ~195 ms — c'est elle qui rend une pièce peu éclairée exploitable.
// Mesure de référence : PR espressif/esp-video-components#90 (5,02 img/s).
constexpr uint32_t kVts = 6984;
// Marge exposition/VTS du driver Linux ov02c10 (OV02C10_EXPOSURE_MAX_MARGIN).
constexpr uint32_t kExposureMargin = 8;
constexpr uint32_t kExposureMin = 4;
// Gain analogique : registre 0x3508:0x3509 = gain x16 << 4 (driver Linux,
// confirmé par mesure dans la PR #90 — la table de gain Guition est fausse,
// on ne l'utilise pas). Plage x1..x15,5.
constexpr uint32_t kGainMinX16 = 16;
constexpr uint32_t kGainMaxX16 = 248;

esp_err_t sccb_read(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t *val) {
  const uint8_t b[2] = {static_cast<uint8_t>(reg >> 8),
                        static_cast<uint8_t>(reg & 0xFF)};
  return i2c_master_transmit_receive(dev, b, sizeof(b), val, 1, 50);
}

esp_err_t sccb_write(i2c_master_dev_handle_t dev, uint16_t reg, uint8_t val) {
  const uint8_t b[3] = {static_cast<uint8_t>(reg >> 8),
                        static_cast<uint8_t>(reg & 0xFF), val};
  return i2c_master_transmit(dev, b, sizeof(b), 50);
}

// État partagé avec l'ISR DW-GDMA. Statiques de fichier et non membres : le
// BSP est un singleton, et les callbacks C n'ont que `user_data` (pris par le
// sémaphore).
// s_req_buf : tampon prêté par camera_capture, consommé par la PROCHAINE
// trame. s_frames : trames démarrées par le DMA, toutes destinations
// confondues — c'est le seul moyen de savoir si le capteur émet quand aucune
// capture n'aboutit (le driver ne signale pas les trames de son tampon de
// secours).
std::atomic<uint8_t *> s_req_buf{nullptr};
std::atomic<uint32_t> s_frames{0};

// Appelée sous interruption au début de chaque trame : IRAM, rien de
// bloquant. Sans tampon demandé, le driver retombe sur son tampon de secours
// et la trame est perdue — voulu, on ne paie que les images qu'on lit.
IRAM_ATTR bool on_get_new_trans(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *t,
                                void *) {
  s_frames.fetch_add(1, std::memory_order_relaxed);
  uint8_t *buf = s_req_buf.exchange(nullptr, std::memory_order_acq_rel);
  if (buf != nullptr) {
    t->buffer = buf;
    t->buflen = Bsp::kCamFrameBytes;
  }
  return false;
}

// Appelée sous interruption, uniquement pour NOS tampons (le driver ne la
// déclenche pas pour son tampon de secours) : IRAM + API FromISR.
IRAM_ATTR bool on_trans_finished(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *,
                                 void *user) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(static_cast<SemaphoreHandle_t>(user), &woken);
  return woken == pdTRUE;
}

} // namespace

uint32_t Bsp::camera_max_exposure_lines() { return kVts - kExposureMargin; }

void Bsp::camera_log_link_state() {
  // Côté P4 : registres bruts de l'hôte CSI et du pont. Lus et non pilotés
  // par la HAL, qui n'expose pas ces états — c'est ce qui distingue « rien
  // sur la lane » (horloge HS inactive, lanes en stop-state) de « paquets
  // reçus mais rejetés » (erreurs ECC/CRC/ID, discard du pont).
  const uint32_t phy_rx = MIPI_CSI_HOST.phy_rx.val;
  const uint32_t stop = MIPI_CSI_HOST.phy_stopstate.val;
  const uint32_t st_main = MIPI_CSI_HOST.int_st_main.val; // lecture = RAZ
  const uint32_t brg_raw = MIPI_CSI_BRIDGE.int_raw.val;
  // Côté capteur : en flux ? quelle taille de sortie réellement programmée ?
  uint8_t mode = 0xEE, w_h = 0xEE, w_l = 0xEE, h_h = 0xEE, h_l = 0xEE,
          mipi = 0xEE;
  if (cam_sccb_ != nullptr) {
    (void)sccb_read(cam_sccb_, 0x0100, &mode);
    (void)sccb_read(cam_sccb_, 0x3808, &w_h);
    (void)sccb_read(cam_sccb_, 0x3809, &w_l);
    (void)sccb_read(cam_sccb_, 0x380a, &h_h);
    (void)sccb_read(cam_sccb_, 0x380b, &h_l);
    (void)sccb_read(cam_sccb_, 0x4800, &mipi);
  }
  ESP_LOGI(TAG,
           "lien : phy_rx=0x%08X (clk HS=%u) stopstate=0x%08X int_st_main=0x%08X"
           " brg_raw=0x%02X | capteur 0x0100=%02X taille=%u x %u 0x4800=%02X"
           " | trames=%u",
           (unsigned)phy_rx, (unsigned)((phy_rx >> 17) & 1), (unsigned)stop,
           (unsigned)st_main, (unsigned)brg_raw, mode, (w_h << 8) | w_l,
           (h_h << 8) | h_l, mipi, (unsigned)s_frames.load());
}

uint32_t Bsp::camera_frame_count() {
  return s_frames.load(std::memory_order_relaxed);
}

esp_err_t Bsp::camera_start() {
  if (cam_ctlr_ != nullptr) {
    return ESP_OK;
  }
  ESP_RETURN_ON_FALSE(i2c_bus_ != nullptr, ESP_ERR_INVALID_STATE, TAG,
                      "bus I2C absent");
  esp_err_t ret = ESP_OK;

  if (cam_sccb_ == nullptr) {
    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = kSccbAddr;
    dev_cfg.scl_speed_hz = kSccbHz;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &cam_sccb_),
                        TAG, "add SCCB");
  }
  if (cam_frame_done_ == nullptr) {
    cam_frame_done_ = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(cam_frame_done_ != nullptr, ESP_ERR_NO_MEM, TAG,
                        "semaphore");
  }
  // Une fin de trame tardive de la session précédente ne doit pas valider la
  // première capture de celle-ci.
  xSemaphoreTake(cam_frame_done_, 0);
  cam_pending_ = false;
  s_req_buf.store(nullptr);

  // --- Capteur : reset, table Guition, puis nos corrections ---------------
  for (const ov02c10::Reg &r : ov02c10::kReset) {
    ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, r.reg, r.val), TAG, "reset");
  }
  vTaskDelay(pdMS_TO_TICKS(ov02c10::kResetDelayMs));
  ESP_RETURN_ON_ERROR(
      sccb_write(cam_sccb_, ov02c10::kMipiIdle.reg, ov02c10::kMipiIdle.val),
      TAG, "mipi idle");
  for (const ov02c10::Reg &r : ov02c10::k1288x728Raw10) {
    ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, r.reg, r.val), TAG,
                        "init 0x%04X", r.reg);
  }
  // Sortie 1280x720 au lieu des 1288x728 de la table. Pas cosmétique : le
  // driver CSI alloue son tampon de secours à la taille exacte de l'image et
  // le synchronise avec le cache, ce qui exige un multiple de la ligne (64 o).
  // 1288x728 en RAW10 = 1 172 080 o (reste 48) -> esp_cam_ctlr_start échoue
  // en ESP_ERR_INVALID_ARG (« failed to sync(M2C) trans buffer », mesuré
  // 2026-11-15) ; 1280x720 = 1 152 000 o = 64 x 18 000. Fenêtre recentrée de
  // +4/+4 : décalages PAIRS, l'ordre du motif de Bayer (GBRG) est conservé.
  {
    static constexpr ov02c10::Reg kWindow[] = {
        {0x3808, kCamWidth >> 8},  {0x3809, kCamWidth & 0xFF},
        {0x380a, kCamHeight >> 8}, {0x380b, kCamHeight & 0xFF},
        {0x3811, 0x07 + 4},        {0x3813, 0x04 + 4},
    };
    for (const ov02c10::Reg &r : kWindow) {
      ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, r.reg, r.val), TAG, "window");
    }
  }
  ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, 0x380e, kVts >> 8), TAG, "vts");
  ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, 0x380f, kVts & 0xFF), TAG, "vts");

  // --- Contrôleur CSI (avant le flux : il doit voir la lane en LP-11) ------
  // RAW10 -> RAW10 : seule configuration acceptée sous la rév. 3.0 (bypass du
  // pont). bk_buffer laissé actif : entre deux captures le DMA a besoin d'une
  // destination, c'est ce tampon de secours qui absorbe les images ignorées.
  esp_cam_ctlr_csi_config_t csi = {};
  csi.ctlr_id = 0;
  csi.h_res = kCamWidth;
  csi.v_res = kCamHeight;
  csi.data_lane_num = 1;
  csi.lane_bit_rate_mbps = kLaneMbps;
  csi.input_data_color_type = CAM_CTLR_COLOR_RAW10;
  csi.output_data_color_type = CAM_CTLR_COLOR_RAW10;
  csi.queue_items = 1;
  esp_cam_ctlr_handle_t h = nullptr;
  ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi, &h), TAG, "csi new");

  esp_cam_ctlr_evt_cbs_t cbs = {};
  cbs.on_get_new_trans = on_get_new_trans;
  cbs.on_trans_finished = on_trans_finished;
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_register_event_callbacks(h, &cbs,
                                                          cam_frame_done_),
                    fail_del, TAG, "csi cbs");
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_enable(h), fail_del, TAG, "csi enable");

  // ISP en BYPASS, obligatoire même sans traitement d'image : sur le P4 les
  // données vont de l'hôte CSI au pont DMA EN TRAVERSANT le module d'entrée de
  // l'ISP (pont partagé, SOC_ISP_SHARE_CSI_BRG). Sans processeur ISP ouvert,
  // le capteur émet (0x0100=1, horloge HS vue active), l'hôte CSI ne signale
  // aucune erreur, et pourtant le DMA ne termine jamais une trame — mesuré le
  // 2026-11-15. Montage calqué sur esp_driver_cam/test_apps/csi.
  // Line start/end : NON. 0x4800 = 0x64 = bits 6|5|2, or la line sync est le
  // bit 4 (OV02C10_MIPI_CTRL00_LINE_SYNC_ENABLE chez Guition) : le capteur
  // n'émet pas ces paquets courts. Les attendre faisait compter de travers les
  // lignes à l'entrée ISP : pont en « vadr_num_gt » + « discard » (int_raw =
  // 0x25), chaque trame jetée comme incomplète — mesuré 2026-11-15.
  {
    esp_isp_processor_cfg_t isp = {};
    isp.clk_hz = 80 * 1000 * 1000;
    isp.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
    isp.input_data_color_type = ISP_COLOR_RAW10;
    isp.output_data_color_type = ISP_COLOR_RAW10;
    isp.has_line_start_packet = false;
    isp.has_line_end_packet = false;
    isp.h_res = kCamWidth;
    isp.v_res = kCamHeight;
    isp.bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG;
    isp.flags.bypass_isp = 1;
    isp_proc_handle_t p = nullptr;
    ESP_GOTO_ON_ERROR(esp_isp_new_processor(&isp, &p), fail_disable, TAG,
                      "isp new");
    cam_isp_ = p;
  }
  // PAS d'esp_isp_enable/disable en bypass : l'IDF 6.0.1 refuse l'enable,
  // mais APRÈS avoir déjà basculé l'état en ENABLE (isp_core.c) — le
  // processeur ne peut alors plus être supprimé, et l'unique ISP reste pris
  // jusqu'au redémarrage (« no available isp processor », vécu).
  ESP_GOTO_ON_ERROR(esp_cam_ctlr_start(h), fail_isp_del, TAG, "csi start");

  // --- Flux : séquence set_stream(1) du driver Guition ---------------------
  // 0x4800=0x64 : line sync ; 0x3002/0x3010/0x300D : sorties de pad du
  // module (pilotage VCM côté Guition) gardées pour reproduire la config qui
  // marche sur les cartes sœurs ; 0x0100=1 : sortie du sommeil.
  {
    static constexpr ov02c10::Reg kStreamOn[] = {
        {0x4800, 0x64}, {0x3002, 0x01}, {0x3010, 0x01},
        {0x300D, 0x01}, {0x0100, 0x01},
    };
    for (const ov02c10::Reg &r : kStreamOn) {
      ESP_GOTO_ON_ERROR(sccb_write(cam_sccb_, r.reg, r.val), fail_stop, TAG,
                        "stream on");
    }
  }
  cam_ctlr_ = h;
  ESP_LOGI(TAG, "OV02C10 %dx%d RAW10 démarré, VTS=%u (~5 img/s)", kCamWidth,
           kCamHeight, (unsigned)kVts);
  return ESP_OK;

fail_stop:
  esp_cam_ctlr_stop(h);
fail_isp_del:
  esp_isp_del_processor(cam_isp_);
  cam_isp_ = nullptr;
fail_disable:
  esp_cam_ctlr_disable(h);
fail_del:
  esp_cam_ctlr_del(h);
  sccb_write(cam_sccb_, 0x0100, 0x00);
  return ret;
}

esp_err_t Bsp::camera_stop() {
  if (cam_ctlr_ == nullptr) {
    return ESP_OK;
  }
  // Capteur d'abord : couper le DMA pendant que la lane émet encore laisserait
  // le pont CSI au milieu d'une trame.
  esp_err_t first = sccb_write(cam_sccb_, 0x0100, 0x00);
  vTaskDelay(pdMS_TO_TICKS(250)); // > une trame à 5 img/s
  esp_err_t e = esp_cam_ctlr_stop(cam_ctlr_);
  if (first == ESP_OK) {
    first = e;
  }
  e = esp_cam_ctlr_disable(cam_ctlr_);
  if (first == ESP_OK) {
    first = e;
  }
  e = esp_cam_ctlr_del(cam_ctlr_);
  if (first == ESP_OK) {
    first = e;
  }
  if (cam_isp_ != nullptr) {
    e = esp_isp_del_processor(cam_isp_);
    if (first == ESP_OK) {
      first = e;
    }
    cam_isp_ = nullptr;
  }
  cam_ctlr_ = nullptr;
  cam_pending_ = false;
  s_req_buf.store(nullptr);
  if (first != ESP_OK) {
    ESP_LOGW(TAG, "arrêt caméra : %s", esp_err_to_name(first));
  }
  return first;
}

esp_err_t Bsp::camera_capture(uint8_t *buf, size_t len, uint32_t timeout_ms) {
  ESP_RETURN_ON_FALSE(cam_ctlr_ != nullptr, ESP_ERR_INVALID_STATE, TAG,
                      "caméra arrêtée");
  ESP_RETURN_ON_FALSE(buf != nullptr && len >= kCamFrameBytes,
                      ESP_ERR_INVALID_ARG, TAG, "tampon");
  // Pas esp_cam_ctlr_receive : avec on_get_new_trans enregistré, le driver
  // ne lit plus sa file (esp_cam_ctlr_csi.c, csi_dma_trans_done_callback).
  if (!cam_pending_) {
    s_req_buf.store(buf, std::memory_order_release);
    cam_pending_ = true;
  }
  if (xSemaphoreTake(cam_frame_done_, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    return ESP_ERR_TIMEOUT;
  }
  cam_pending_ = false;
  return ESP_OK;
}

esp_err_t Bsp::camera_set_exposure(uint32_t lines, uint32_t gain_x16) {
  ESP_RETURN_ON_FALSE(cam_sccb_ != nullptr, ESP_ERR_INVALID_STATE, TAG,
                      "SCCB absent");
  const uint32_t max = camera_max_exposure_lines();
  lines = lines < kExposureMin ? kExposureMin : (lines > max ? max : lines);
  gain_x16 = gain_x16 < kGainMinX16
                 ? kGainMinX16
                 : (gain_x16 > kGainMaxX16 ? kGainMaxX16 : gain_x16);
  const uint32_t g = gain_x16 << 4;
  // Écritures directes, sans group hold (0x3208) : la PR #90 a mesuré que le
  // chemin Guition à group hold ne faisait jamais bouger l'exposition.
  ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, 0x3501, (lines >> 8) & 0xFF), TAG,
                      "exp");
  ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, 0x3502, lines & 0xFF), TAG, "exp");
  ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, 0x3508, (g >> 8) & 0xFF), TAG,
                      "gain");
  ESP_RETURN_ON_ERROR(sccb_write(cam_sccb_, 0x3509, g & 0xFF), TAG, "gain");
  return ESP_OK;
}

} // namespace bsp
