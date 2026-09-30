// LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC).
// With the default C library malloc, LVGL's many small allocations (objects, styles, label text,
// glyph cache) landed in internal RAM (anything under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL goes
// there first) and filled it up. Internal RAM is kept for Wi-Fi, DMA and task stacks.
#include "lvgl.h"
#include "esp_heap_caps.h"

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes) { (void)mem; (void)bytes; return NULL; }
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : malloc(size);                 // PSRAM full: fall back to any RAM
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, LV_CAPS);
    return q ? q : realloc(p, new_size);
}

void lv_free_core(void *p) { free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t i;
    heap_caps_get_info(&i, LV_CAPS);
    mon->total_size = i.total_free_bytes + i.total_allocated_bytes;
    mon->free_size = i.total_free_bytes;
    mon->free_biggest_size = i.largest_free_block;
    mon->used_cnt = i.allocated_blocks;
    mon->free_cnt = i.free_blocks;
    mon->used_pct = mon->total_size ? 100 - (uint8_t)(100ULL * i.total_free_bytes / mon->total_size) : 0;
    mon->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }
