/* Выбор свободного GSP FB-региона и непересекающийся раскрой памяти. */
#include "gsp_memory.h"

int nv_gsp_memory_plan(const nv_gsp_static_info *si, uint64_t app_bytes,
                       const nv_vram_span *extra, nv_gsp_memory_layout *out)
{
    if (!out) return -1;
    *out = (nv_gsp_memory_layout){0};
    if (!si || !si->num_regions || si->num_regions > NV_GSP_FBREGION_MAX ||
        app_bytes < NV_VRAM_MIN_APP_BYTES || (app_bytes & 0xffffull) ||
        app_bytes > (1ull << 39)) return -1;

    nv_gsp_memory_layout p = {0};
    uint64_t mapped = NV_GSP_SERVICE_BYTES + app_bytes;
    if (nv_gmmu_range_plan(NV_GSP_APP_VA_BASE, mapped, 0, 0, &p.tables) != -2) return -1;
    uint64_t total = mapped + p.tables.table_bytes + NV_GSP_CHANNEL_BYTES;
    nv_vram_span available[NV_GSP_FBREGION_MAX];
    uint32_t count = 0;
    for (uint32_t i = 0; i < si->num_regions; i++) {
        const nv_gsp_fb_region *r = &si->regions[i];
        if (r->base > r->limit || r->limit >= (1ull << 40)) return -1;
        for (uint32_t j = 0; j < i; j++)
            if (r->base <= si->regions[j].limit && si->regions[j].base <= r->limit)
                return -1;
        /* Консервативно исключаем регион целиком при ненулевом reserved:
           размер резерва не сообщает расположение его отдельных страниц. */
        if (r->reserved || r->prot) continue;
        available[count++] = (nv_vram_span){r->base, r->limit - r->base + 1};
    }
    if (!count) return -2;
    nv_vram_span excluded[2] = {{0, 0x20000000ull}, {0, 0}};
    uint32_t excluded_count = 1;
    if (extra && extra->size) excluded[excluded_count++] = *extra;
    int rc = nv_vram_find_range(available, count, excluded, excluded_count,
                                total, 0x10000, &p.owned);
    if (rc) return rc;
    uint64_t table_bytes = p.tables.table_bytes;
    if (nv_gmmu_range_plan(NV_GSP_APP_VA_BASE, mapped, p.owned.base + mapped,
                           table_bytes, &p.tables)) return -1;
    p.service_phys = p.owned.base;
    p.app_phys = p.owned.base + NV_GSP_SERVICE_BYTES;
    p.app_va = NV_GSP_APP_VA_BASE + NV_GSP_SERVICE_BYTES;
    p.app_bytes = app_bytes;
    p.channel_phys = p.tables.table_phys + table_bytes;
    *out = p;
    return 0;
}
