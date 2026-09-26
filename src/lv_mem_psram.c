/* LVGL's allocator (LV_USE_STDLIB_MALLOC = LV_STDLIB_CUSTOM): everything
 * LVGL allocates — every widget, style and label string — prefers PSRAM.
 *
 * With plain malloc, arduino-esp32 keeps anything under 4KB in internal RAM,
 * so thousands of small widget allocations ate it: ~30KB was left with the
 * draw buffer in internal RAM, too little for Wi-Fi (found live 2026-09-27).
 * The draw buffer itself is allocated directly in internal RAM (board.cpp),
 * where rendering speed and PSRAM bandwidth actually matter. Falls back to
 * internal RAM if PSRAM is ever exhausted. */

#include <esp_heap_caps.h>
#include <lvgl.h>

#define PREFER_PSRAM 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) {
  LV_UNUSED(mem);
  LV_UNUSED(bytes);
  return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

void *lv_malloc_core(size_t size) { return heap_caps_malloc_prefer(size, PREFER_PSRAM); }

void *lv_realloc_core(void *p, size_t new_size) { return heap_caps_realloc_prefer(p, new_size, PREFER_PSRAM); }

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p) { LV_UNUSED(mon_p); }

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
