#include "test_helpers.h"

#include "esp_log.h"

#include <cstdlib> // free() : jpeg_alloc_decoder_mem s'apparie à free(), pas heap_caps_free

namespace tst {
namespace {

constexpr const char *TAG = "tst";

jpeg_decoder_handle_t s_dec = nullptr;
bool s_bsp_ready = false;

} // namespace

jpeg_decoder_handle_t decoder() { return s_dec; }

bool init_decoder() {
  jpeg_decode_engine_cfg_t eng = {};
  // Large devant les ~8,5 ms d'une frame réelle : on mesure des durées ici, on
  // ne veut pas qu'un timeout serré transforme une mesure lente en échec.
  eng.timeout_ms = 2000;
  esp_err_t err = jpeg_new_decoder_engine(&eng, &s_dec);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_new_decoder_engine: %s", esp_err_to_name(err));
    s_dec = nullptr;
    return false;
  }
  return true;
}

bool bsp_ready() { return s_bsp_ready; }
void set_bsp_ready(bool ready) { s_bsp_ready = ready; }

bool decode_asset(const uint8_t *jpeg, size_t len, Decoded *out) {
  if (s_dec == nullptr || out == nullptr) {
    return false;
  }
  *out = Decoded{};

  jpeg_decode_picture_info_t info = {};
  esp_err_t err = jpeg_decoder_get_info(jpeg, len, &info);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_decoder_get_info: %s", esp_err_to_name(err));
    return false;
  }

  // Le moteur aligne w ET h sur 16 px et écrit TOUT le rectangle aligné :
  // dimensionner sur les dimensions brutes donne ESP_ERR_INVALID_ARG.
  const uint32_t aw = (info.width + 15) & ~15u;
  const uint32_t ah = (info.height + 15) & ~15u;
  const size_t need = static_cast<size_t>(aw) * ah * sizeof(uint16_t);

  jpeg_decode_memory_alloc_cfg_t mem = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
  size_t got = 0;
  auto *buf = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(need, &mem, &got));
  if (buf == nullptr) {
    ESP_LOGE(TAG, "alloc sortie %u o échouée", static_cast<unsigned>(need));
    return false;
  }

  jpeg_decode_cfg_t cfg = {};
  cfg.output_format = JPEG_DECODE_OUT_FORMAT_RGB565;
  // BGR = "small endian" dans la convention du driver = RGB565 little-endian,
  // l'ordre attendu par le PPA et le panneau. RGB sortirait byte-swappé
  // (couleurs en arc-en-ciel).
  cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
  cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;

  uint32_t out_size = 0;
  err = jpeg_decoder_process(s_dec, &cfg, jpeg, len, buf, got, &out_size);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "jpeg_decoder_process: %s", esp_err_to_name(err));
    free(buf);
    return false;
  }

  out->buf = buf;
  out->width = info.width;
  out->height = info.height;
  out->stride = aw;
  out->size = got;
  out->decoded_bytes = out_size;
  return true;
}

void free_decoded(Decoded *d) {
  if (d != nullptr && d->buf != nullptr) {
    free(d->buf);
    d->buf = nullptr;
  }
}

uint16_t pixel_at(const Decoded &d, uint32_t x, uint32_t y) {
  const auto *px = reinterpret_cast<const uint16_t *>(d.buf);
  return px[static_cast<size_t>(y) * d.stride + x];
}

} // namespace tst
