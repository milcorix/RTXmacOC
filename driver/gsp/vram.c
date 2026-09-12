/* Учёт диапазонов VRAM без выделений в куче/больших локальных массивов. */
#include "vram.h"

static int span_valid(uint64_t base, uint64_t size)
{ return size && size <= UINT64_MAX - base; }

static int alignment_valid(uint64_t alignment)
{ return alignment >= NV_VRAM_PAGE_BYTES && !(alignment & (alignment - 1)); }

static int align_up(uint64_t value, uint64_t alignment, uint64_t *out)
{
    if (value > UINT64_MAX - (alignment - 1)) return -1;
    *out = (value + alignment - 1) & ~(alignment - 1);
    return 0;
}

int nv_vram_find_range(const nv_vram_span *available, uint32_t count,
                       const nv_vram_span *excluded, uint32_t excluded_count,
                       uint64_t bytes, uint64_t alignment, nv_vram_span *out)
{
    if (!out) return -1;
    *out = (nv_vram_span){0};
    if (!available || !count || (excluded_count && !excluded) || !bytes ||
        !alignment_valid(alignment) || (bytes & (NV_VRAM_PAGE_BYTES - 1))) return -1;
    for (uint32_t i = 0; i < count; i++)
        if (!span_valid(available[i].base, available[i].size)) return -1;
    for (uint32_t i = 0; i < excluded_count; i++)
        if (!span_valid(excluded[i].base, excluded[i].size)) return -1;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t candidate, end = available[i].base + available[i].size;
        if (align_up(available[i].base, alignment, &candidate)) continue;
        while (candidate <= end && bytes <= end - candidate) {
            uint64_t next = candidate;
            for (uint32_t j = 0; j < excluded_count; j++) {
                uint64_t ex_end = excluded[j].base + excluded[j].size;
                if (candidate < ex_end && excluded[j].base < candidate + bytes && ex_end > next)
                    next = ex_end;
            }
            if (next == candidate) { out->base = candidate; out->size = bytes; return 0; }
            if (align_up(next, alignment, &candidate)) break;
        }
    }
    return -2;
}

int nv_vram_pool_init(nv_vram_pool *p, uint64_t phys, uint64_t va, uint64_t size)
{
    if (!p || ((phys | va | size) & (NV_VRAM_PAGE_BYTES - 1)) ||
        !span_valid(phys, size) || !span_valid(va, size)) return -1;
    p->phys = phys; p->va = va; p->size = size; p->used = 0; p->next_handle = 1;
    for (unsigned i = 0; i < NV_VRAM_MAX_ALLOCS; i++) p->allocations[i] = (nv_vram_allocation){0};
    return 0;
}

int nv_vram_alloc(nv_vram_pool *p, uint64_t bytes, uint64_t alignment, uint64_t *handle)
{
    if (!handle) return -1;
    *handle = 0;
    if (!p || !p->size || !bytes || !alignment_valid(alignment)) return -1;
    if (align_up(bytes, NV_VRAM_PAGE_BYTES, &bytes)) return -1;
    /* Никогда не выдаём старый handle повторно в пределах жизни пула. */
    if (!p->next_handle || p->next_handle == UINT64_MAX || p->used > p->size ||
        bytes > p->size - p->used) return -2;
    unsigned slot = 0;
    while (slot < NV_VRAM_MAX_ALLOCS && p->allocations[slot].handle) slot++;
    if (slot == NV_VRAM_MAX_ALLOCS) return -2;

    uint64_t address;
    if (align_up(p->va, alignment, &address)) return -2;
    uint64_t candidate = address - p->va;
    while (candidate <= p->size && bytes <= p->size - candidate) {
        uint64_t next = candidate;
        for (unsigned i = 0; i < NV_VRAM_MAX_ALLOCS; i++) {
            const nv_vram_allocation *a = &p->allocations[i];
            if (a->handle && candidate < a->offset + a->size &&
                a->offset < candidate + bytes && a->offset + a->size > next)
                next = a->offset + a->size;
        }
        if (next == candidate) {
            nv_vram_allocation a = { p->next_handle++, candidate, bytes };
            p->allocations[slot] = a; p->used += bytes; *handle = a.handle;
            return 0;
        }
        if (align_up(p->va + next, alignment, &address)) return -2;
        candidate = address - p->va;
    }
    return -2;
}

int nv_vram_free(nv_vram_pool *p, uint64_t handle)
{
    if (!p || !handle) return -1;
    for (unsigned i = 0; i < NV_VRAM_MAX_ALLOCS; i++) {
        nv_vram_allocation *a = &p->allocations[i];
        if (a->handle == handle) {
            p->used -= a->size; *a = (nv_vram_allocation){0}; return 0;
        }
    }
    return -1;
}

int nv_vram_resolve(const nv_vram_pool *p, uint64_t handle, uint64_t offset,
                    uint64_t bytes, uint64_t *phys, uint64_t *va)
{
    if (phys) *phys = 0;
    if (va) *va = 0;
    if (!p || !handle || !bytes) return -1;
    for (unsigned i = 0; i < NV_VRAM_MAX_ALLOCS; i++) {
        const nv_vram_allocation *a = &p->allocations[i];
        if (a->handle != handle) continue;
        if (offset > a->size || bytes > a->size - offset) return -1;
        if (phys) *phys = p->phys + a->offset + offset;
        if (va) *va = p->va + a->offset + offset;
        return 0;
    }
    return -1;
}
