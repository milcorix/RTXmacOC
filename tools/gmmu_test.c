/*
 * gmmu_test.c — офлайн-проверка прямого GMMU (driver/gsp/gmmu.c). Без GPU.
 *
 * Мокает nv_mmio_t: BAR0 как разреженная память + модель VRAM, куда PRAMIN-окно
 * пишет по (win_base + off). Проверяет: аим-регистр 0x1700, пересечение границы
 * окна, кодирование PTE/PDE (сверка с nouveau), разложение VA по уровням,
 * полную сборку иерархии + read-back, диапазонный маппинг 256 страниц.
 *
 *   make gmmu-test && ./tools/gmmu_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../driver/gsp/gmmu.h"

/* ---- Мок BAR0 + VRAM ---- */
#define BAR0_SIZE   (16u * 1024u * 1024u)
#define VRAM_SIZE   (64ull * 1024 * 1024)   /* моделируем нижние 64 МиБ VRAM */

static uint8_t g_bar0[BAR0_SIZE];
static uint8_t g_vram[VRAM_SIZE];
static uint32_t g_aim_writes;   /* сколько раз аимили окно */
static uint32_t g_last_aim_reg;

static uint32_t mock_rd(void *ctx, uint32_t off)
{
    (void)ctx;
    if (off >= NV_PRAMIN_WINDOW_OFFSET && off < NV_PRAMIN_WINDOW_OFFSET + NV_PRAMIN_WINDOW_SIZE) {
        /* чтение через окно: транслируем в VRAM по текущему аиму */
        uint32_t reg = *(uint32_t *)(g_bar0 + NV_PBUS_BAR0_WINDOW);
        uint64_t base = (uint64_t)(reg & 0x00ffffffu) << NV_PBUS_BAR0_WINDOW_BASE_SHIFT;
        uint64_t pa = base + (off - NV_PRAMIN_WINDOW_OFFSET);
        if (pa + 4 > VRAM_SIZE) { fprintf(stderr, "VRAM OOB rd %llu\n", (unsigned long long)pa); exit(2); }
        return *(uint32_t *)(g_vram + pa);
    }
    return *(uint32_t *)(g_bar0 + off);
}

static void mock_wr(void *ctx, uint32_t off, uint32_t val)
{
    (void)ctx;
    if (off == NV_PBUS_BAR0_WINDOW) {
        g_aim_writes++;
        g_last_aim_reg = val;
        *(uint32_t *)(g_bar0 + off) = val;
        return;
    }
    if (off >= NV_PRAMIN_WINDOW_OFFSET && off < NV_PRAMIN_WINDOW_OFFSET + NV_PRAMIN_WINDOW_SIZE) {
        uint32_t reg = *(uint32_t *)(g_bar0 + NV_PBUS_BAR0_WINDOW);
        uint64_t base = (uint64_t)(reg & 0x00ffffffu) << NV_PBUS_BAR0_WINDOW_BASE_SHIFT;
        uint64_t pa = base + (off - NV_PRAMIN_WINDOW_OFFSET);
        if (pa + 4 > VRAM_SIZE) { fprintf(stderr, "VRAM OOB wr %llu\n", (unsigned long long)pa); exit(2); }
        *(uint32_t *)(g_vram + pa) = val;
        return;
    }
    *(uint32_t *)(g_bar0 + off) = val;
}

static void mock_udelay(void *ctx, uint32_t us) { (void)ctx; (void)us; }

static const nv_mmio_t g_io = { NULL, mock_rd, mock_wr, mock_udelay };

/* Прямое чтение модели VRAM (в обход окна) — для независимой проверки. */
static uint64_t vram_rd64(uint64_t pa)
{
    return (uint64_t)*(uint32_t *)(g_vram + pa)
         | ((uint64_t)*(uint32_t *)(g_vram + pa + 4) << 32);
}

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); g_fail++; } } while (0)

/* ---- Тесты ---- */

static void test_pramin_aim(void)
{
    printf("[pramin_aim]\n");
    uint64_t win = ~0ull;
    memset(g_bar0, 0, sizeof(g_bar0));
    g_aim_writes = 0;

    /* Первая запись аимит окно на страницу phys & ~0xfffff. */
    uint64_t phys = 0x01234560ull;
    nv_pramin_wr32(&g_io, &win, phys, 0xdeadbeef);
    uint64_t expect_base = phys & ~(uint64_t)NV_PRAMIN_WINDOW_MASK;
    uint32_t expect_reg = (uint32_t)(expect_base >> 16) | (0u << 24);
    CHECK(g_aim_writes == 1, "первый доступ должен аимить ровно раз (было %u)", g_aim_writes);
    CHECK(g_last_aim_reg == expect_reg, "reg=0x%08x ожид 0x%08x", g_last_aim_reg, expect_reg);
    CHECK(vram_rd64(phys) == (uint64_t)0xdeadbeef || *(uint32_t*)(g_vram+phys)==0xdeadbeef,
          "данные не легли в VRAM по phys");
    CHECK(*(uint32_t *)(g_vram + phys) == 0xdeadbeef, "VRAM[phys] != записанное");

    /* Второй доступ в то же окно — не переаимливать. */
    nv_pramin_wr32(&g_io, &win, phys + 0x100, 0x11112222);
    CHECK(g_aim_writes == 1, "тот же окно — не переаимливать (было %u)", g_aim_writes);

    /* Доступ за границей окна — переаимить. */
    nv_pramin_wr32(&g_io, &win, expect_base + NV_PRAMIN_WINDOW_SIZE + 0x40, 0x33334444);
    CHECK(g_aim_writes == 2, "выход за окно должен переаимить (было %u)", g_aim_writes);
}

static void test_encode(void)
{
    printf("[encode PTE/PDE]\n");
    /* PTE VRAM: (phys>>4)|VALID, aperture=0, kind=0. */
    uint64_t phys = 0x13100000ull;
    uint64_t pte = nv_gmmu_make_pte_vram(phys, 0, 0);
    CHECK(pte == ((phys >> 4) | 1ull), "PTE=0x%llx ожид 0x%llx",
          (unsigned long long)pte, (unsigned long long)((phys >> 4) | 1ull));
    CHECK((pte & (3ull << 1)) == 0, "aperture должно быть VIDMEM(0)");

    /* RO/priv биты. */
    uint64_t pte_ro = nv_gmmu_make_pte_vram(phys, 1, 0);
    CHECK(pte_ro & (1ull << 6), "RO bit6 не установлен");
    uint64_t pte_pv = nv_gmmu_make_pte_vram(phys, 0, 1);
    CHECK(pte_pv & (1ull << 5), "PRIV bit5 не установлен");

    /* PDE VRAM: (pt>>4)|(1<<1). */
    uint64_t pt = 0x00200000ull;
    uint64_t pde = nv_gmmu_make_pde_vram(pt);
    CHECK(pde == ((pt >> 4) | (1ull << 1)), "PDE=0x%llx ожид 0x%llx",
          (unsigned long long)pde, (unsigned long long)((pt >> 4) | (1ull << 1)));
}

static void test_va_index(void)
{
    printf("[VA decomposition]\n");
    /* Сконструировать VA с известными индексами на каждом уровне. */
    uint64_t va = ((uint64_t)2  << NV_GMMU_PD3_SHIFT)   /* PD3=2 (макс 3) */
                | ((uint64_t)5  << NV_GMMU_PD2_SHIFT)
                | ((uint64_t)7  << NV_GMMU_PD1_SHIFT)
                | ((uint64_t)9  << NV_GMMU_PD0_SHIFT)
                | ((uint64_t)11 << NV_GMMU_SPT_SHIFT)
                | 0x040;
    CHECK(nv_gmmu_pde_index(va, NV_GMMU_PD3_SHIFT, NV_GMMU_PD3_ENTRIES) == 2, "PD3 idx");
    CHECK(nv_gmmu_pde_index(va, NV_GMMU_PD2_SHIFT, NV_GMMU_PD2_ENTRIES) == 5, "PD2 idx");
    CHECK(nv_gmmu_pde_index(va, NV_GMMU_PD1_SHIFT, NV_GMMU_PD1_ENTRIES) == 7, "PD1 idx");
    CHECK(nv_gmmu_pde_index(va, NV_GMMU_PD0_SHIFT, NV_GMMU_PD0_ENTRIES) == 9, "PD0 idx");
    CHECK(nv_gmmu_pde_index(va, NV_GMMU_SPT_SHIFT, NV_GMMU_SPT_ENTRIES) == 11, "SPT idx");
}

/* Разложить таблицы по VRAM (страничновыровненные, непересекающиеся). */
static nv_gmmu_tables mk_tables(void)
{
    nv_gmmu_tables t;
    t.pd3_phys = 0x00100000ull;
    t.pd2_phys = 0x00101000ull;
    t.pd1_phys = 0x00102000ull;
    t.pd0_phys = 0x00103000ull;
    t.spt_phys = 0x00104000ull;
    return t;
}

static void test_build_1page(void)
{
    printf("[build_1page + readback]\n");
    memset(g_vram, 0xff, sizeof(g_vram));  /* грязь: проверим, что таблицы обнуляются */
    memset(g_bar0, 0, sizeof(g_bar0));
    uint64_t win = ~0ull;
    nv_gmmu_tables t = mk_tables();

    uint64_t va = ((uint64_t)1 << NV_GMMU_PD3_SHIFT)
                | ((uint64_t)3 << NV_GMMU_PD0_SHIFT)
                | ((uint64_t)17 << NV_GMMU_SPT_SHIFT);
    uint64_t page = 0x13100000ull;

    nv_gmmu_build_1page(&g_io, &win, &t, va, page);

    /* PD3[1] → PD2 */
    uint32_t i3 = nv_gmmu_pde_index(va, NV_GMMU_PD3_SHIFT, NV_GMMU_PD3_ENTRIES);
    CHECK(vram_rd64(t.pd3_phys + i3*8) == nv_gmmu_make_pde_vram(t.pd2_phys), "PD3 entry");
    /* прочие записи PD3 обнулены */
    CHECK(vram_rd64(t.pd3_phys + ((i3+1)&3)*8) == 0, "PD3 сосед не обнулён");

    uint32_t i2 = nv_gmmu_pde_index(va, NV_GMMU_PD2_SHIFT, NV_GMMU_PD2_ENTRIES);
    CHECK(vram_rd64(t.pd2_phys + i2*8) == nv_gmmu_make_pde_vram(t.pd1_phys), "PD2 entry");
    uint32_t i1 = nv_gmmu_pde_index(va, NV_GMMU_PD1_SHIFT, NV_GMMU_PD1_ENTRIES);
    CHECK(vram_rd64(t.pd1_phys + i1*8) == nv_gmmu_make_pde_vram(t.pd0_phys), "PD1 entry");
    /* PD0 dual: small → SPT, big=0 */
    uint32_t i0 = nv_gmmu_pde_index(va, NV_GMMU_PD0_SHIFT, NV_GMMU_PD0_ENTRIES);
    CHECK(vram_rd64(t.pd0_phys + i0*16) == nv_gmmu_make_pde_vram(t.spt_phys), "PD0 small");
    CHECK(vram_rd64(t.pd0_phys + i0*16 + 8) == 0, "PD0 big должно быть 0");
    /* SPT[17] → page */
    uint64_t pte = nv_gmmu_read_pte(&g_io, &win, t.spt_phys, va);
    CHECK(pte == nv_gmmu_make_pte_vram(page, 0, 0), "SPT PTE readback=0x%llx",
          (unsigned long long)pte);
}

static void test_map_range(void)
{
    printf("[map_range 256 pages]\n");
    memset(g_vram, 0xff, sizeof(g_vram));
    memset(g_bar0, 0, sizeof(g_bar0));
    uint64_t win = ~0ull;
    nv_gmmu_tables t = mk_tables();

    uint64_t va = ((uint64_t)1 << NV_GMMU_PD3_SHIFT);   /* SPT idx 0 */
    uint64_t page = 0x13100000ull;
    int rc = nv_gmmu_map_range(&g_io, &win, &t, va, page, 256);
    CHECK(rc == 0, "map_range rc=%d", rc);

    /* Все 256 PTE подряд, корректные адреса. */
    int ok = 1;
    for (uint32_t k = 0; k < 256; k++) {
        uint64_t pte = vram_rd64(t.spt_phys + k*8);
        if (pte != nv_gmmu_make_pte_vram(page + (uint64_t)k*0x1000, 0, 0)) { ok = 0; break; }
    }
    CHECK(ok, "непрерывные PTE диапазона");
    /* 257-я запись должна быть нулём (не тронута). */
    CHECK(vram_rd64(t.spt_phys + 256*8) == 0, "PTE за диапазоном не обнулён/затёрт");

    /* Выход за границу SPT должен вернуть -1. */
    uint64_t va2 = va | ((uint64_t)300 << NV_GMMU_SPT_SHIFT);
    CHECK(nv_gmmu_map_range(&g_io, &win, &t, va2, page, 300) == -1,
          "пересечение границы SPT должно дать -1");
}

int main(void)
{
    printf("=== gmmu offline test ===\n");
    test_pramin_aim();
    test_encode();
    test_va_index();
    test_build_1page();
    test_map_range();
    if (g_fail) { printf("\n%d проверок провалено\n", g_fail); return 1; }
    printf("\nвсе проверки пройдены\n");
    return 0;
}
