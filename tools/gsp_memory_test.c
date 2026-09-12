/* OFFLINE: раскрой по карте GSP, отдельные служебные/пользовательские области. */
#include <stdio.h>
#include <string.h>
#include "../driver/gsp/gsp_memory.h"
static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); failures++; } } while (0)

int main(void)
{
    /* Физические границы из сохранённого лога RTX 4070S 2026-07-16.
       Применение нового раскроя к этим данным не является HW-тестом. */
    nv_gsp_static_info si;
    memset(&si, 0, sizeof(si));
    si.num_regions = 3;
    si.regions[0].base = 0; si.regions[0].limit = 0x30fffff; si.regions[0].reserved = 0x3100000;
    si.regions[1].base = 0x3100000; si.regions[1].limit = 0x2f034ffffull;
    si.regions[2].base = 0x2f0350000ull; si.regions[2].limit = 0x2ff9fffffull; si.regions[2].prot = 1;
    nv_gsp_memory_layout p;
    CHECK(nv_gsp_memory_plan(&si, 1ull<<30, NULL, &p) == 0);
    CHECK(p.owned.base == 0x20000000);
    CHECK(p.app_bytes == (1ull<<30));
    CHECK(p.app_phys == p.service_phys + 0x100000);
    CHECK(p.app_va == 0x20100000);
    CHECK(p.tables.bytes == 0x40100000);
    CHECK(p.tables.level_count[3] == 3 && p.tables.level_count[4] == 513);
    CHECK(p.tables.table_phys == p.app_phys + p.app_bytes);
    CHECK(p.channel_phys == p.tables.table_phys + p.tables.table_bytes);
    CHECK(p.channel_phys + NV_GSP_CHANNEL_BYTES == p.owned.base + p.owned.size);
    CHECK(p.owned.base + p.owned.size <= si.regions[1].limit + 1);
    nv_vram_span console = {0x50000000, 0x8000000};
    CHECK(nv_gsp_memory_plan(&si, 1ull<<30, &console, &p) == 0);
    CHECK(p.owned.base == 0x58000000);
    CHECK(nv_gsp_memory_plan(&si, 4ull<<30, &console, &p) == 0);
    CHECK(nv_gsp_memory_plan(&si, (1ull<<30)-4096, NULL, &p) == -1);
    si.regions[1].prot = 1;
    CHECK(nv_gsp_memory_plan(&si, 1ull<<30, NULL, &p) == -2 && p.owned.size == 0);
    si.regions[1].prot = 0;
    si.regions[1].limit = 0x500fffff; /* данные влезают, но таблицы + канал уже нет */
    CHECK(nv_gsp_memory_plan(&si, 1ull<<30, NULL, &p) == -2);
    si.regions[1].limit = 0x2f034ffffull;
    si.regions[2].base = 0x30000000; /* перекрытие с защищённым регионом */
    CHECK(nv_gsp_memory_plan(&si, 1ull<<30, NULL, &p) == -1);
    printf("GSP memory layout OFFLINE: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
