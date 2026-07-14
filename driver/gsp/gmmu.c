/*
 * gmmu.c — прямой GMMU-маппинг (host-side page-tables) для Ada. См. gmmu.h.
 *
 * Порт nouveau vmmgp100.c (gp100_vmm_pgt_pte/pd0_pde/pd1_pde/valid, desc_12) +
 * nv50.c/bar2_walk.c (PRAMIN-окно). Все биты/индексы сверены по исходникам
 * 535.113.01 и nouveau v6.11 (см. комментарии «SRC»). Значения-константы —
 * в gmmu.h; здесь только логика записи через колбэки BAR0 (nv_mmio_t).
 */
#include "gmmu.h"

/* ===================== PRAMIN ===================== */

/* Навести окно PRAMIN на страницу, содержащую phys (VRAM-таргет).
   Кэш текущей базы в *win_base, чтобы не писать 0x1700 без нужды. */
static void pramin_aim(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys)
{
    uint64_t base = phys & ~(uint64_t)NV_PRAMIN_WINDOW_MASK;
    if (*win_base == base) return;
    uint32_t reg = (uint32_t)(base >> NV_PBUS_BAR0_WINDOW_BASE_SHIFT)
                 | (NV_PBUS_BAR0_WINDOW_TARGET_VID_MEM << 24);
    io->wr(io->ctx, NV_PBUS_BAR0_WINDOW, reg);
    *win_base = base;
}

uint32_t nv_pramin_rd32(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys)
{
    pramin_aim(io, win_base, phys);
    uint32_t off = (uint32_t)(phys & NV_PRAMIN_WINDOW_MASK);
    return io->rd(io->ctx, NV_PRAMIN_WINDOW_OFFSET + off);
}

void nv_pramin_wr32(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys, uint32_t val)
{
    pramin_aim(io, win_base, phys);
    uint32_t off = (uint32_t)(phys & NV_PRAMIN_WINDOW_MASK);
    io->wr(io->ctx, NV_PRAMIN_WINDOW_OFFSET + off, val);
}

void nv_pramin_wr64(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys, uint64_t val)
{
    nv_pramin_wr32(io, win_base, phys,     (uint32_t)(val & 0xffffffffu));
    nv_pramin_wr32(io, win_base, phys + 4, (uint32_t)(val >> 32));
}

void nv_pramin_fill(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys,
                    uint32_t len, uint32_t val)
{
    for (uint32_t i = 0; i < len; i += 4)
        nv_pramin_wr32(io, win_base, phys + i, val);
}

/* ===================== Кодирование записей ===================== */

uint32_t nv_gmmu_pde_index(uint64_t va, uint32_t level_shift, uint32_t entries)
{
    return (uint32_t)((va >> level_shift) & (entries - 1u));
}

/* SRC gp100_vmm_valid + gf100_vmm_aper: VRAM aperture=0, kind=0 (pitch/generic).
   PTE = (phys>>4) | VALID | (vol<<3) | (priv<<5) | (ro<<6). */
uint64_t nv_gmmu_make_pte_vram(uint64_t phys, int read_only, int priv)
{
    uint64_t data = (phys >> NV_GMMU_PTE_ADDR_SHIFT) | NV_GMMU_PTE_VALID;
    /* aperture VIDMEM = 0 → биты [2:1] не ставим; kind=0 */
    if (priv)      data |= NV_GMMU_PTE_PRIV;
    if (read_only) data |= NV_GMMU_PTE_RO;
    return data;
}

/* SRC gp100_vmm_pde (VRAM-ветвь): APERTURE=1<<1, адрес = (pt_phys>>4). */
uint64_t nv_gmmu_make_pde_vram(uint64_t pt_phys)
{
    return (pt_phys >> NV_GMMU_PDE_ADDR_SHIFT) | NV_GMMU_PDE_APERTURE_VIDMEM;
}

/* ===================== Построение таблиц ===================== */

void nv_gmmu_zero_table(const nv_mmio_t *io, uint64_t *win_base,
                        uint64_t table_phys, uint32_t bytes)
{
    nv_pramin_fill(io, win_base, table_phys, bytes, 0u);
}

/* Записать записи-указатели верхних уровней PD3→PD2→PD1→PD0 (одна ветвь).
   PD0 — dual-PDE 16 байт: small-half (data[0]) → SPT, big-half (data[1])=0. */
static void gmmu_write_pde_chain(const nv_mmio_t *io, uint64_t *win_base,
                                 const nv_gmmu_tables *t, uint64_t va)
{
    /* PD3[idx] → PD2 */
    uint32_t i3 = nv_gmmu_pde_index(va, NV_GMMU_PD3_SHIFT, NV_GMMU_PD3_ENTRIES);
    nv_pramin_wr64(io, win_base, t->pd3_phys + (uint64_t)i3 * NV_GMMU_PDE_SIZE,
                   nv_gmmu_make_pde_vram(t->pd2_phys));
    /* PD2[idx] → PD1 */
    uint32_t i2 = nv_gmmu_pde_index(va, NV_GMMU_PD2_SHIFT, NV_GMMU_PD2_ENTRIES);
    nv_pramin_wr64(io, win_base, t->pd2_phys + (uint64_t)i2 * NV_GMMU_PDE_SIZE,
                   nv_gmmu_make_pde_vram(t->pd1_phys));
    /* PD1[idx] → PD0 */
    uint32_t i1 = nv_gmmu_pde_index(va, NV_GMMU_PD1_SHIFT, NV_GMMU_PD1_ENTRIES);
    nv_pramin_wr64(io, win_base, t->pd1_phys + (uint64_t)i1 * NV_GMMU_PDE_SIZE,
                   nv_gmmu_make_pde_vram(t->pd0_phys));
    /* PD0[idx] — dual 16б: small (data[0]) → SPT, big (data[1]) = 0. */
    uint32_t i0 = nv_gmmu_pde_index(va, NV_GMMU_PD0_SHIFT, NV_GMMU_PD0_ENTRIES);
    uint64_t pd0e = t->pd0_phys + (uint64_t)i0 * NV_GMMU_PD0E_SIZE;
    nv_pramin_wr64(io, win_base, pd0e,     nv_gmmu_make_pde_vram(t->spt_phys)); /* small */
    nv_pramin_wr64(io, win_base, pd0e + 8, 0ull);                              /* big */
}

int nv_gmmu_build_1page(const nv_mmio_t *io, uint64_t *win_base,
                        const nv_gmmu_tables *t, uint64_t va, uint64_t page_phys)
{
    /* Обнулить все пять таблиц. */
    nv_gmmu_zero_table(io, win_base, t->pd3_phys, NV_GMMU_PD3_ENTRIES * NV_GMMU_PDE_SIZE);
    nv_gmmu_zero_table(io, win_base, t->pd2_phys, NV_GMMU_PD2_ENTRIES * NV_GMMU_PDE_SIZE);
    nv_gmmu_zero_table(io, win_base, t->pd1_phys, NV_GMMU_PD1_ENTRIES * NV_GMMU_PDE_SIZE);
    nv_gmmu_zero_table(io, win_base, t->pd0_phys, NV_GMMU_PD0_ENTRIES * NV_GMMU_PD0E_SIZE);
    nv_gmmu_zero_table(io, win_base, t->spt_phys, NV_GMMU_SPT_ENTRIES * NV_GMMU_PTE_SIZE);

    gmmu_write_pde_chain(io, win_base, t, va);

    /* SPT[idx] → страница. */
    uint32_t is = nv_gmmu_pde_index(va, NV_GMMU_SPT_SHIFT, NV_GMMU_SPT_ENTRIES);
    nv_pramin_wr64(io, win_base, t->spt_phys + (uint64_t)is * NV_GMMU_PTE_SIZE,
                   nv_gmmu_make_pte_vram(page_phys, 0, 0));
    return 0;
}

int nv_gmmu_map_range(const nv_mmio_t *io, uint64_t *win_base,
                      const nv_gmmu_tables *t, uint64_t va, uint64_t page_phys,
                      uint32_t npages)
{
    /* Одна ветвь PD3..PD0 покрывает [va-выравн. по PD0] = 2 МиБ (512×4К) —
       для 1 МиБ (256 стр.) хватает одной PD0/SPT. Проверка: все страницы в
       пределах одной SPT (256 ≤ 512) и не пересекают границу PD0. */
    uint32_t is0 = nv_gmmu_pde_index(va, NV_GMMU_SPT_SHIFT, NV_GMMU_SPT_ENTRIES);
    if ((uint64_t)is0 + npages > NV_GMMU_SPT_ENTRIES)
        return -1;  /* пересекает границу таблицы — нужна многоветочная версия */

    /* Обнулить таблицы и построить общую цепочку PDE один раз. */
    nv_gmmu_zero_table(io, win_base, t->pd3_phys, NV_GMMU_PD3_ENTRIES * NV_GMMU_PDE_SIZE);
    nv_gmmu_zero_table(io, win_base, t->pd2_phys, NV_GMMU_PD2_ENTRIES * NV_GMMU_PDE_SIZE);
    nv_gmmu_zero_table(io, win_base, t->pd1_phys, NV_GMMU_PD1_ENTRIES * NV_GMMU_PDE_SIZE);
    nv_gmmu_zero_table(io, win_base, t->pd0_phys, NV_GMMU_PD0_ENTRIES * NV_GMMU_PD0E_SIZE);
    nv_gmmu_zero_table(io, win_base, t->spt_phys, NV_GMMU_SPT_ENTRIES * NV_GMMU_PTE_SIZE);

    gmmu_write_pde_chain(io, win_base, t, va);

    /* Заполнить npages PTE начиная с индекса is0 (contiguous VRAM). */
    for (uint32_t k = 0; k < npages; k++) {
        uint64_t pte = nv_gmmu_make_pte_vram(page_phys + (uint64_t)k * 0x1000ull, 0, 0);
        nv_pramin_wr64(io, win_base,
                       t->spt_phys + (uint64_t)(is0 + k) * NV_GMMU_PTE_SIZE, pte);
    }
    return 0;
}

uint64_t nv_gmmu_read_pte(const nv_mmio_t *io, uint64_t *win_base,
                          uint64_t spt_phys, uint64_t va)
{
    uint32_t is = nv_gmmu_pde_index(va, NV_GMMU_SPT_SHIFT, NV_GMMU_SPT_ENTRIES);
    uint64_t pa = spt_phys + (uint64_t)is * NV_GMMU_PTE_SIZE;
    uint64_t lo = nv_pramin_rd32(io, win_base, pa);
    uint64_t hi = nv_pramin_rd32(io, win_base, pa + 4);
    return lo | (hi << 32);
}
