/*
 * LVGL allocator (CONFIG_LV_USE_CUSTOM_MALLOC): widgets, styles and draw layers live in PSRAM, so the scarce
 * internal RAM stays free for Wi-Fi, lwIP and task stacks. The DMA draw buffers are allocated by the LVGL port.
 */
#include "esp_heap_caps.h"
#include "lvgl.h"

#define LV_CAPS     (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
}

void lv_mem_deinit(void)
{
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    LV_UNUSED(pool);
}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);    /* PSRAM full: fall back to internal */
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *n = heap_caps_realloc(p, new_size, LV_CAPS);
    return n ? n : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, LV_CAPS);
    mon_p->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon_p->free_size = info.total_free_bytes;
    mon_p->free_biggest_size = info.largest_free_block;
    mon_p->used_cnt = info.allocated_blocks;
    mon_p->free_cnt = info.free_blocks;
    mon_p->max_used = info.total_free_bytes + info.total_allocated_bytes - info.minimum_free_bytes;
    mon_p->used_pct = mon_p->total_size ? 100 - (uint8_t)(100ULL * info.total_free_bytes / mon_p->total_size) : 0;
    mon_p->frag_pct = 0;
}

lv_result_t lv_mem_test_core(void)
{
    return heap_caps_check_integrity(LV_CAPS, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
