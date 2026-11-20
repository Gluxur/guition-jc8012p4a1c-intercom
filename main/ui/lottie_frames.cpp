#include "lottie_frames.h"

#include <bitset>
#include <new>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "libs/thorvg/thorvg_capi.h"

#include "premul_blend.h"

namespace lottie_frames {

namespace {

constexpr const char *TAG = "lottie_frames";

// 6,4 s à 15 img/s : couvre les boucles meteocons (360 images à 60 img/s).
constexpr uint32_t kMaxFrames = 96;
// Hero + 6 jours, pire cas tous différents.
constexpr int kMaxAnims = 7;
// Le cache ne descend jamais la PSRAM libre sous ce plancher : une session
// vidéo (mailbox MJPEG, tampons audio, DTLS) doit pouvoir s'ouvrir par-dessus
// l'écran météo, qui reste chargé derrière la sonnerie le temps de la bascule.
constexpr size_t kPsramFloorBytes = 8u * 1024 * 1024;
// lv_lottie suppose 60 img/s pour la durée d'une Lottie (lv_lottie.c) : même
// hypothèse ici, sinon la cadence des icônes changerait.
constexpr float kLottieFps = 60.0f;

} // namespace

struct Anim {
  const uint8_t *src = nullptr;
  int32_t px = 0;
  uint32_t bg = 0;
  int refs = 0;
  uint32_t n = 0;        // images par boucle
  float total = 0;       // images Lottie
  bool cached = false;   // false = rendu direct dans pixels[0]
  uint16_t *pixels = nullptr;
  std::bitset<kMaxFrames> filled;
  uint32_t filled_count = 0;
  Tvg_Animation *tvg_anim = nullptr; // libérés dès la boucle complète
  Tvg_Canvas *tvg_canvas = nullptr;
  bool has_last = false;
  uint32_t last_tick = 0;
  lv_image_dsc_t dsc[kMaxFrames];
};

namespace {

// Structures en PSRAM (~2,7 Ko chacune, descripteurs compris) : la SRAM
// interne est réservée au DTLS.
Anim *s_pool[kMaxAnims] = {};
// Cible ThorVG partagée, ARGB8888 prémultiplié, à la taille de la plus grande
// icône vivante : une seule entrée rend à la fois (tâche LVGL).
uint32_t *s_scratch = nullptr;
int32_t s_scratch_px = 0;

void free_tvg(Anim &a) {
  // Ordre de lv_lottie_destructor : l'animation possède l'image poussée.
  if (a.tvg_anim) {
    tvg_animation_del(a.tvg_anim);
    a.tvg_anim = nullptr;
  }
  if (a.tvg_canvas) {
    tvg_canvas_destroy(a.tvg_canvas);
    a.tvg_canvas = nullptr;
  }
}

bool ensure_scratch(int32_t px) {
  if (s_scratch && s_scratch_px >= px) {
    return true;
  }
  heap_caps_free(s_scratch);
  s_scratch = static_cast<uint32_t *>(heap_caps_malloc(
      static_cast<size_t>(px) * px * 4, MALLOC_CAP_SPIRAM));
  s_scratch_px = s_scratch ? px : 0;
  return s_scratch != nullptr;
}

void maybe_free_scratch() {
  for (Anim *a : s_pool) {
    if (a) {
      return;
    }
  }
  heap_caps_free(s_scratch);
  s_scratch = nullptr;
  s_scratch_px = 0;
}

void destroy(Anim *a) {
  free_tvg(*a);
  heap_caps_free(a->pixels);
  heap_caps_free(a);
}

void render(Anim &a, uint32_t idx, uint16_t *dst) {
  const size_t npx = static_cast<size_t>(a.px) * a.px;
  // La cible est partagée : chaque entrée la réattache à sa taille.
  tvg_swcanvas_set_target(a.tvg_canvas, s_scratch, a.px, a.px, a.px,
                          TVG_COLORSPACE_ARGB8888);
  std::memset(s_scratch, 0, npx * 4);
  tvg_animation_set_frame(a.tvg_anim,
                          static_cast<float>(idx) * a.total / a.n);
  tvg_canvas_update(a.tvg_canvas);
  tvg_canvas_draw(a.tvg_canvas);
  tvg_canvas_sync(a.tvg_canvas);
  premul_blend::over_rgb565(s_scratch, dst, npx, a.bg);
}

void init_dsc(lv_image_dsc_t &d, int32_t px, const uint16_t *data) {
  std::memset(&d, 0, sizeof d);
  d.header.magic = LV_IMAGE_HEADER_MAGIC;
  d.header.cf = LV_COLOR_FORMAT_RGB565;
  d.header.w = px;
  d.header.h = px;
  d.header.stride = px * 2;
  d.data_size = static_cast<uint32_t>(px) * px * 2;
  d.data = reinterpret_cast<const uint8_t *>(data);
}

} // namespace

Anim *acquire(const uint8_t *data, size_t size, int32_t px, uint32_t bg_rgb) {
  int free_slot = -1;
  for (int i = 0; i < kMaxAnims; i++) {
    Anim *a = s_pool[i];
    if (a && a->src == data && a->px == px && a->bg == bg_rgb) {
      a->refs++;
      return a;
    }
    if (!a && free_slot < 0) {
      free_slot = i;
    }
  }
  if (free_slot < 0 || !ensure_scratch(px)) {
    ESP_LOGE(TAG, "icône %ldpx : plus de place", (long)px);
    return nullptr;
  }

  Anim *a = static_cast<Anim *>(
      heap_caps_calloc(1, sizeof(Anim), MALLOC_CAP_SPIRAM));
  if (!a) {
    ESP_LOGE(TAG, "icône %ldpx : pas de PSRAM", (long)px);
    return nullptr;
  }
  new (a) Anim();
  a->src = data;
  a->px = px;
  a->bg = bg_rgb;
  a->refs = 1;

  a->tvg_anim = tvg_animation_new();
  a->tvg_canvas = tvg_swcanvas_create();
  Tvg_Paint *pic =
      a->tvg_anim ? tvg_animation_get_picture(a->tvg_anim) : nullptr;
  // copy = true comme lv_lottie : la source est en flash, et le chargeur
  // Lottie n'a pas à pouvoir l'écrire.
  if (!pic || !a->tvg_canvas ||
      tvg_picture_load_data(pic, reinterpret_cast<const char *>(data),
                            static_cast<uint32_t>(size), "lottie",
                            true) != TVG_RESULT_SUCCESS) {
    ESP_LOGE(TAG, "icône %ldpx : source Lottie refusée", (long)px);
    destroy(a);
    return nullptr;
  }
  tvg_swcanvas_set_target(a->tvg_canvas, s_scratch, px, px, px,
                          TVG_COLORSPACE_ARGB8888);
  tvg_canvas_push(a->tvg_canvas, pic);
  tvg_picture_set_size(pic, px, px);
  tvg_animation_get_total_frame(a->tvg_anim, &a->total);

  const float n = a->total * kFps / kLottieFps + 0.5f;
  a->n = n < 1 ? 1 : (n > kMaxFrames ? kMaxFrames : static_cast<uint32_t>(n));

  const size_t frame_bytes = static_cast<size_t>(px) * px * 2;
  const size_t want = frame_bytes * a->n;
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= want + kPsramFloorBytes) {
    a->pixels =
        static_cast<uint16_t *>(heap_caps_malloc(want, MALLOC_CAP_SPIRAM));
  }
  a->cached = a->pixels != nullptr;
  if (!a->cached) {
    a->pixels = static_cast<uint16_t *>(
        heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM));
    if (!a->pixels) {
      ESP_LOGE(TAG, "icône %ldpx : pas de PSRAM", (long)px);
      destroy(a);
      return nullptr;
    }
    ESP_LOGW(TAG, "icône %ldpx : PSRAM courte, rendu direct", (long)px);
  }
  for (uint32_t i = 0; i < (a->cached ? a->n : 1); i++) {
    init_dsc(a->dsc[i], px,
             a->pixels + static_cast<size_t>(i) * px * px);
  }
  if (a->cached) {
    ESP_LOGI(TAG, "icône %ldpx : %lu images, %u Ko", (long)px,
             (unsigned long)a->n, (unsigned)(want / 1024));
  }
  s_pool[free_slot] = a;
  return a;
}

void release(Anim *a) {
  if (!a || --a->refs > 0) {
    return;
  }
  for (Anim *&p : s_pool) {
    if (p == a) {
      p = nullptr;
    }
  }
  destroy(a);
  maybe_free_scratch();
}

const lv_image_dsc_t *frame(Anim *a, uint32_t tick, bool *in_place) {
  const uint32_t idx = tick % a->n;
  const lv_image_dsc_t *d = a->cached ? &a->dsc[idx] : &a->dsc[0];
  *in_place = !a->cached;
  // Deuxième icône de la même entrée dans le même top : déjà prête.
  if (a->has_last && a->last_tick == tick) {
    return d;
  }
  a->has_last = true;
  a->last_tick = tick;

  if (!a->cached) {
    render(*a, idx, a->pixels);
    return d;
  }
  if (!a->filled[idx]) {
    render(*a, idx, a->pixels + static_cast<size_t>(idx) * a->px * a->px);
    a->filled[idx] = true;
    if (++a->filled_count == a->n) {
      // Boucle complète : ThorVG ne sert plus (le JSON copié et l'arbre de
      // scène repartent au tas).
      free_tvg(*a);
    }
  }
  return d;
}

} // namespace lottie_frames
