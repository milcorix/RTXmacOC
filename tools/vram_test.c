/* Офлайн: ресурсы >=1 ГиБ, запрещённые области, stale handles и фрагментация.
   Не исполняет GPU-команды и не служит HW-доказательством. */
#include <stdio.h>
#include <string.h>
#include "../driver/gsp/vram.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)
static nv_vram_pool pool;

static void test_selection(void)
{
    nv_vram_span available[] = {{0x100000, 0x200000000ull}};
    /* Исключения нарочно перекрываются и идут не по порядку. */
    nv_vram_span excluded[] = {{0x14000000, 0x1000000}, {0, 0x8000000},
                               {0x7000000, 0x1000000}, {0x80000000, 0x10000000}};
    nv_vram_span out;
    CHECK(nv_vram_find_range(available, 1, excluded, 4, 1ull<<30, 0x10000, &out) == 0);
    CHECK(out.base == 0x15000000 && out.size == (1ull<<30));
    for (unsigned i = 0; i < 4; i++)
        CHECK(out.base + out.size <= excluded[i].base ||
              out.base >= excluded[i].base + excluded[i].size);
    CHECK(nv_vram_find_range(available, 1, excluded, 4, 1ull<<35, 4096, &out) == -2);
    CHECK(nv_vram_find_range(available, 1, NULL, 1, 4096, 4096, &out) == -1);
    CHECK(nv_vram_find_range(available, 1, NULL, 0, 4096, 12288, &out) == -1);
    available[0] = (nv_vram_span){UINT64_MAX-4095, 8192};
    CHECK(nv_vram_find_range(available, 1, NULL, 0, 4096, 4096, &out) == -1);
}

static void test_gigabyte(void)
{
    const uint64_t gib = NV_VRAM_MIN_APP_BYTES;
    CHECK(nv_vram_pool_init(&pool, 0x180000000ull, 0x40000000, gib + 65536) == 0);
    uint64_t first = 0, tail = 0, phys, va;
    CHECK(nv_vram_alloc(&pool, gib, 65536, &first) == 0);
    CHECK(pool.used == gib);
    CHECK(nv_vram_alloc(&pool, 65536, 65536, &tail) == 0);
    CHECK(nv_vram_resolve(&pool, tail, 65535, 1, &phys, &va) == 0);
    CHECK(phys == 0x180000000ull + gib + 65535 && va == 0x40000000 + gib + 65535);
    CHECK(nv_vram_resolve(&pool, first, gib-1, 2, &phys, &va) == -1);
    CHECK(nv_vram_resolve(&pool, first, UINT64_MAX-4, 16, &phys, &va) == -1);
    CHECK(nv_vram_resolve(&pool, first, gib, 0, &phys, &va) == -1);
    uint64_t extra = 123;
    CHECK(nv_vram_alloc(&pool, 1, 4096, &extra) == -2 && extra == 0);
    CHECK(nv_vram_free(&pool, first) == 0);
    CHECK(nv_vram_resolve(&pool, first, 0, 1, &phys, &va) == -1);
    CHECK(nv_vram_alloc(&pool, gib, 4096, &extra) == 0 && extra != first);
    CHECK(nv_vram_free(&pool, first) == -1);
    CHECK(nv_vram_free(&pool, extra) == 0 && nv_vram_free(&pool, tail) == 0 && pool.used == 0);
}

static void test_slots(void)
{
    CHECK(nv_vram_pool_init(&pool, 0x1000, 0x5000, 1ull<<30) == 0);
    uint64_t handles[NV_VRAM_MAX_ALLOCS], extra;
    for (unsigned i = 0; i < NV_VRAM_MAX_ALLOCS; i++)
        CHECK(nv_vram_alloc(&pool, 1, 65536, &handles[i]) == 0);
    CHECK(nv_vram_alloc(&pool, 4096, 4096, &extra) == -2);
    for (unsigned i = 0; i < NV_VRAM_MAX_ALLOCS; i++) {
        uint64_t va;
        CHECK(nv_vram_resolve(&pool, handles[i], 0, 4096, NULL, &va) == 0 && !(va & 65535));
        CHECK(nv_vram_free(&pool, handles[i]) == 0);
    }
    CHECK(pool.used == 0);
    pool.next_handle = UINT64_MAX;
    CHECK(nv_vram_alloc(&pool, 4096, 4096, &extra) == -2);
    CHECK(nv_vram_alloc(&pool, UINT64_MAX, 4096, &extra) == -1);
}

static void test_fragmentation(void)
{
    /* Независимый побайтовый (по страницам) учёт: после каждой операции
       восстанавливаем занятость только по выданным handle/resolve. */
    enum { PAGES = 1024, ITEMS = 64 };
    uint64_t handles[ITEMS] = {0}, sizes[ITEMS] = {0};
    unsigned char pages[PAGES];
    uint32_t rng = 17;
    CHECK(nv_vram_pool_init(&pool, 0x100000000ull, 0x20000000, PAGES * 4096ull) == 0);
    for (unsigned step = 0; step < 4000; step++) {
        rng = rng * 1664525u + 1013904223u;
        unsigned item = (rng >> 16) % ITEMS;
        if (handles[item]) {
            CHECK(nv_vram_free(&pool, handles[item]) == 0);
            handles[item] = 0;
        } else {
            uint64_t size = ((rng >> 8) % 32 + 1) * 4096ull;
            int rc = nv_vram_alloc(&pool, size, 4096ull << (rng & 3), &handles[item]);
            CHECK(rc == 0 || rc == -2);
            sizes[item] = size;
        }
        memset(pages, 0, sizeof(pages));
        uint64_t used = 0;
        for (unsigned i = 0; i < ITEMS; i++) {
            if (!handles[i]) continue;
            uint64_t phys, va;
            CHECK(nv_vram_resolve(&pool, handles[i], 0, sizes[i], &phys, &va) == 0);
            CHECK(phys - 0x100000000ull == va - 0x20000000);
            uint64_t start = (va - 0x20000000) / 4096, count = sizes[i] / 4096;
            CHECK(start + count <= PAGES);
            if (start + count > PAGES) return;
            for (uint64_t n = start; n < start + count; n++) { CHECK(!pages[n]); pages[n] = 1; }
            used += sizes[i];
        }
        CHECK(pool.used == used);
    }
}

int main(void)
{
    test_selection(); test_gigabyte(); test_slots(); test_fragmentation();
    printf("VRAM OFFLINE: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
