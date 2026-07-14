/*
 * gmmu.h — слой 3, проход D: прямой GMMU-маппинг (host-side page-tables).
 *
 * Заход через MAP_MEMORY_DMA (RPC fn=14) на железе отвергнут GSP как
 * NV_ERR_INVALID_FUNCTION (это путь vGPU/SR-IOV, см. gsp-layer3-rpc.md §4D.1).
 * Правильная модель GSP-client (bare-metal): КЛИЕНТ владеет page-tables —
 * сам пишет иерархию PDE/PTE в VRAM, а GSP получает физадреса корневых уровней
 * через RM_CONTROL VASPACE_COPY_SERVER_RESERVED_PDES (0x90f10106), чтобы прошить
 * instance-block PDB. Листовые PTE — общий физический VRAM, HW ходит по ним сам.
 *
 * Источники (пересказано для лицензии): nouveau nvkm/subdev/mmu/vmmgp100.c
 * (gp100_vmm_pgt_pte/pd0_pte/pde, gp100_vmm_desc_12), nvkm/subdev/mmu/r535.c
 * (r535_mmu_promote_vmm), nvkm/subdev/instmem/nv50.c (PRAMIN-окно), OGK
 * bar2_walk.c (BAR0-window bootstrap), dev_bus.h (NV_PBUS_BAR0_WINDOW=0x1700),
 * ctrl90f1.h (NV90F1_CTRL_*). Ada AD104 = формат Ampere ga10x = tu102_vmm.
 */
#ifndef RTXMACOC_GMMU_H
#define RTXMACOC_GMMU_H

#include <stdint.h>
#include "falcon.h"   /* nv_mmio_t */

/* ===================== PRAMIN: доступ CPU→VRAM через BAR0-окно ===================== */

/* Регистр аима окна (dev_bus.h, Maxwell→Ada). _BASE=phys>>16 @[23:0],
   _TARGET @[25:24]: 0=VID_MEM, 2=SYS_COHERENT, 3=SYS_NONCOHERENT. */
#define NV_PBUS_BAR0_WINDOW          0x00001700u
#define NV_PBUS_BAR0_WINDOW_BASE_SHIFT       16u
#define NV_PBUS_BAR0_WINDOW_TARGET_VID_MEM    0u
#define NV_PBUS_BAR0_WINDOW_TARGET_SYS_COH    2u
#define NV_PBUS_BAR0_WINDOW_TARGET_SYS_NCOH   3u
/* Окно данных PRAMIN в BAR0 (1 МиБ). */
#define NV_PRAMIN_WINDOW_OFFSET      0x00700000u
#define NV_PRAMIN_WINDOW_SIZE        0x00100000u   /* 1 МиБ */
#define NV_PRAMIN_WINDOW_MASK        (NV_PRAMIN_WINDOW_SIZE - 1u)

/* Доступ к VRAM по физическому адресу через PRAMIN-окно.
   Аимят окно на (phys & ~mask), пишут/читают по BAR0+0x700000+(phys&mask).
   Кэшируют текущий базовый адрес окна в *win_base (init: ~0), чтобы не
   переаимливать без нужды. io — тот же BAR0-доступ, что у falcon/gsp. */
uint32_t nv_pramin_rd32(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys);
void     nv_pramin_wr32(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys,
                        uint32_t val);
/* Заполнить [phys, phys+len) значением val (len кратно 4). */
void     nv_pramin_fill(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys,
                        uint32_t len, uint32_t val);
/* Записать 64-битное слово (little-endian, двумя wr32). */
void     nv_pramin_wr64(const nv_mmio_t *io, uint64_t *win_base, uint64_t phys,
                        uint64_t val);

/* ===================== Раскладка GMMU для Ada (формат ga10x/gp100) ===================== */

/* Радикс 4К-листа (nouveau gp100_vmm_desc_12), уровни сверху вниз:
   PD3[48:47](2 бита, 8б/зап) → PD2[46:38](9,8б) → PD1[37:29](9,8б) →
   PD0[28:21](8, dual-PDE 16б) → SPT[20:12](9, PTE 8б); смещение [11:0]. */
#define NV_GMMU_VA_BITS            49u
#define NV_GMMU_PAGE_SHIFT         12u
#define NV_GMMU_LEVELS              5u   /* PD3,PD2,PD1,PD0,SPT */

/* Границы битов индекса каждого уровня (virtAddrBitLo уровня). */
#define NV_GMMU_PD3_SHIFT          47u
#define NV_GMMU_PD2_SHIFT          38u
#define NV_GMMU_PD1_SHIFT          29u
#define NV_GMMU_PD0_SHIFT          21u
#define NV_GMMU_SPT_SHIFT          12u

/* Число записей на уровень (2^bits). */
#define NV_GMMU_PD3_ENTRIES        (1u << 2)   /* [48:47] */
#define NV_GMMU_PD2_ENTRIES        (1u << 9)
#define NV_GMMU_PD1_ENTRIES        (1u << 9)
#define NV_GMMU_PD0_ENTRIES        (1u << 8)   /* [28:21] */
#define NV_GMMU_SPT_ENTRIES        (1u << 9)

/* Размер записи в байтах. */
#define NV_GMMU_PDE_SIZE            8u    /* PD3/PD2/PD1 */
#define NV_GMMU_PD0E_SIZE          16u    /* dual small+big */
#define NV_GMMU_PTE_SIZE            8u

/* Биты PTE (gp100_vmm_pgt_pte + gp100_vmm_valid), 64-бит слово:
   VALID@0, APERTURE@[2:1], VOL@3, PRIV@5, RO@6, ADDR=(phys>>4), KIND@[63:56]. */
#define NV_GMMU_PTE_VALID          (1ull << 0)
#define NV_GMMU_PTE_APERTURE_SHIFT  1
#define NV_GMMU_PTE_APERTURE_VIDMEM   0ull   /* VID_MEM */
#define NV_GMMU_PTE_APERTURE_SYS_COH  2ull
#define NV_GMMU_PTE_APERTURE_SYS_NCOH 3ull
#define NV_GMMU_PTE_VOL            (1ull << 3)
#define NV_GMMU_PTE_PRIV           (1ull << 5)
#define NV_GMMU_PTE_RO             (1ull << 6)
#define NV_GMMU_PTE_ADDR_SHIFT      4    /* phys >> 4 в слове */
#define NV_GMMU_PTE_KIND_SHIFT     56

/* Биты PDE (gp100_vmm_pde): APERTURE@[2:1] (1=VRAM, 2=SYS_COH|VOL, 3=SYS_NCOH),
   адрес нижней таблицы = (pt_phys>>4). VALID неявный (ненулевое слово). */
#define NV_GMMU_PDE_APERTURE_VIDMEM   (1ull << 1)
#define NV_GMMU_PDE_APERTURE_SYS_COH  (2ull << 1)
#define NV_GMMU_PDE_APERTURE_SYS_NCOH (3ull << 1)
#define NV_GMMU_PDE_VOL            (1ull << 3)
#define NV_GMMU_PDE_ADDR_SHIFT      4

/* Индекс записи уровня для виртуального адреса. */
uint32_t nv_gmmu_pde_index(uint64_t va, uint32_t level_shift, uint32_t entries);

/* Собрать 64-битную PTE для VRAM-страницы phys (kind=0, RW, aperture=VIDMEM). */
uint64_t nv_gmmu_make_pte_vram(uint64_t phys, int read_only, int priv);

/* Собрать 64-битную PDE, указывающую на нижнюю таблицу pt_phys во VRAM. */
uint64_t nv_gmmu_make_pde_vram(uint64_t pt_phys);

/* ===================== Построение page-tables через PRAMIN ===================== */

/* Физические адреса выделенных под уровни таблиц (гость выбирает из usable FB). */
typedef struct {
    uint64_t pd3_phys;   /* корень (верхний уровень) */
    uint64_t pd2_phys;
    uint64_t pd1_phys;
    uint64_t pd0_phys;
    uint64_t spt_phys;   /* лист (страничная таблица 4К) */
} nv_gmmu_tables;

/* Обнулить одну таблицу (entries * entry_size байт) через PRAMIN. */
void nv_gmmu_zero_table(const nv_mmio_t *io, uint64_t *win_base,
                        uint64_t table_phys, uint32_t bytes);

/*
 * Построить полную одноветочную иерархию PD3→PD2→PD1→PD0→SPT для одной страницы
 * виртуального адреса va, указывающей на физическую VRAM-страницу page_phys.
 * Обнуляет все пять таблиц, затем пишет по одной записи на каждом уровне (PD0 —
 * dual: small-half указывает на SPT). Всё через PRAMIN (win_base кэшируется).
 * Возврат 0. Порт nouveau gp100_vmm_pgt_pte/pd0_pde/pd1_pde.
 */
int nv_gmmu_build_1page(const nv_mmio_t *io, uint64_t *win_base,
                        const nv_gmmu_tables *t, uint64_t va, uint64_t page_phys);

/*
 * Замаппить непрерывный диапазон из npages 4К-страниц, начиная с va →
 * физический VRAM [page_phys, page_phys + npages*4К). Требует, чтобы диапазон
 * умещался в одну ветку PD3→PD0 (npages ≤ 512, va выровнен на 4К и не пересекает
 * границу SPT [20:12]). Обнуляет таблицы один раз, затем пишет PDE-ветку и
 * npages PTE подряд. Для нашего объекта прохода C (1 МиБ = 256 стр.) достаточно.
 * Возврат 0 при успехе, -1 если диапазон выходит за одну SPT.
 */
int nv_gmmu_map_range(const nv_mmio_t *io, uint64_t *win_base,
                      const nv_gmmu_tables *t, uint64_t va,
                      uint64_t page_phys, uint32_t npages);

/* Прочитать записанную SPT-PTE для va (для read-back самопроверки на железе). */
uint64_t nv_gmmu_read_pte(const nv_mmio_t *io, uint64_t *win_base,
                          uint64_t spt_phys, uint64_t va);

#endif /* RTXMACOC_GMMU_H */
