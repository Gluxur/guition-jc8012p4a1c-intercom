/* Allocateur LVGL (CONFIG_LV_USE_CUSTOM_MALLOC) : tout en PSRAM.
 *
 * Pourquoi : avec l'allocateur clib, chaque objet, style local ou texte LVGL
 * de moins de 16 Ko (SPIRAM_MALLOC_ALWAYSINTERNAL) partait en SRAM interne —
 * quelques centaines d'objets au boot, plus le brassage des pages
 * reconstruites (journal d'appels, textarea du log). C'est la SRAM que
 * mbedtls réclame pour le DTLS des sessions WebRTC, et dont l'épuisement donne
 * « Erreur ouverture WebRTC ». Repli en interne seulement si la PSRAM manque.
 *
 * Fichier C et non C++ : ce sont des définitions à ABI C réclamées par la
 * bibliothèque LVGL ; `-u lv_malloc_core` (CMakeLists du composant) force le
 * linker à prendre cet objet bien qu'aucun fichier du BSP ne le référence. */
#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "esp_heap_caps.h"

#define LV_PSRAM_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) {
  void *p = heap_caps_malloc(size, LV_PSRAM_CAPS);
  return p != NULL ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size) {
  void *q = heap_caps_realloc(p, new_size, LV_PSRAM_CAPS);
  return q != NULL ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

#endif
