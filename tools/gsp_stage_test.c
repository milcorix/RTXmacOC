/*
 * gsp_stage_test.c — офлайн-проверка подготовки GSP-RM (задача 6, фаза 4, часть 1).
 *
 * Грузит gsp + bootloader через fw_blob, извлекает секции ELF (.fwimage,
 * .fwsignature_ad10x), разбирает дескриптор bootloader и строит radix3 над .fwimage
 * со СИНТЕТИЧЕСКОЙ базой (GPU не нужен). Проверяет цепочку уровней и инварианты.
 *
 * Сборка:  make gsp-stage-test
 * Запуск:  ./tools/gsp_stage_test   (env RTX_FW_DIR/RTX_FW_VERSION при необходимости)
 */
#include "../driver/gsp/fw_blob.h"
#include "../driver/gsp/gsp_fw.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long ull;

static uint64_t ld64(const uint8_t *p)
{ uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v; }

int main(void)
{
    int ok = 1;

    /* --- GSP-RM ELF: секции --- */
    uint8_t *gsp = NULL; size_t gsp_len = 0;
    if (nv_fw_blob_get("gsp", &gsp, &gsp_len) != NV_FW_BLOB_OK) {
        fprintf(stderr, "FAIL: не загрузить gsp (zstd? путь?)\n"); return 1; }
    printf("gsp-rm ELF: %zu байт\n", gsp_len);

    nv_gsp_sections_t s;
    int rc = nv_gsp_rm_sections(gsp, gsp_len, ".fwsignature_ad10x", &s);
    if (rc != NV_GSP_OK) { fprintf(stderr, "FAIL: секции GSP-RM rc=%d\n", rc); nv_fw_blob_free(gsp); return 1; }
    printf(".fwimage:            off=0x%llx size=0x%llx (%llu МБ)\n",
           (unsigned long long)s.fwimage_off, (unsigned long long)s.fwimage_size,
           (unsigned long long)(s.fwimage_size >> 20));
    printf(".fwsignature_ad10x:  off=0x%llx size=0x%llx\n",
           (unsigned long long)s.sig_off, (unsigned long long)s.sig_size);
    if (s.fwimage_off + s.fwimage_size > gsp_len) { printf("  ✗ .fwimage вне блоба\n"); ok = 0; }
    if (s.sig_size == 0) { printf("  ✗ пустая подпись\n"); ok = 0; }
    nv_fw_blob_free(gsp);

    /* --- bootloader desc --- */
    uint8_t *bl = NULL; size_t bl_len = 0;
    if (nv_fw_blob_get("bootloader", &bl, &bl_len) != NV_FW_BLOB_OK) {
        fprintf(stderr, "FAIL: не загрузить bootloader\n"); return 1; }
    nv_gsp_bootloader_t b;
    rc = nv_gsp_bootloader_parse(bl, bl_len, &b);
    nv_fw_blob_free(bl);
    if (rc != NV_GSP_OK) { fprintf(stderr, "FAIL: разбор bootloader rc=%d\n", rc); return 1; }
    printf("bootloader: %zu байт; data_abs=%u data_size=%u app_version=%u\n",
           bl_len, b.data_abs, b.data_size, b.app_version);
    printf("  code_offset=%u data_offset=%u manifest_offset=%u manifest_size=%u\n",
           b.code_offset, b.data_offset, b.manifest_offset, b.manifest_size);

    /* --- radix3 над .fwimage (синтетическая база) --- */
    uint32_t n2 = 0, l2p = 0;
    rc = nv_gsp_radix3_levels(s.fwimage_size, &n2, &l2p);
    if (rc != NV_GSP_OK) { fprintf(stderr, "FAIL: radix3 levels rc=%d\n", rc); return 1; }
    printf("radix3: lvl2 записей(n2)=%u, страниц lvl2=%u, lvl1 записей=%u, lvl0=1\n", n2, l2p, l2p);

    /* Заполнить со синтетическими contiguous-базами и проверить несколько записей. */
    const uint64_t FW = 0x20000000ULL, L2 = 0x10001000ULL, L1 = 0x10000000ULL;
    uint8_t *lvl0 = calloc(1, NV_GSP_PAGE_SIZE);
    uint8_t *lvl1 = calloc(1, NV_GSP_PAGE_SIZE);
    uint8_t *lvl2 = calloc(l2p, NV_GSP_PAGE_SIZE);
    if (!lvl0 || !lvl1 || !lvl2) { fprintf(stderr, "FAIL: alloc\n"); return 1; }
    rc = nv_gsp_radix3_fill(s.fwimage_size, lvl0, lvl1, lvl2, FW, L2, L1);
    if (rc != NV_GSP_OK) { fprintf(stderr, "FAIL: radix3 fill rc=%d\n", rc); return 1; }

    /* Инварианты. */
    if (ld64(lvl0) != L1) { printf("  ✗ lvl0[0] != lvl1 base\n"); ok = 0; }
    if (ld64(lvl1) != L2) { printf("  ✗ lvl1[0] != lvl2 base\n"); ok = 0; }
    if (ld64(lvl1 + (size_t)(l2p - 1) * 8) != L2 + (uint64_t)(l2p - 1) * NV_GSP_PAGE_SIZE)
        { printf("  ✗ lvl1[last] неверна\n"); ok = 0; }
    if (ld64(lvl2) != FW) { printf("  ✗ lvl2[0] != fwimage base\n"); ok = 0; }
    if (ld64(lvl2 + (size_t)(n2 - 1) * 8) != FW + (uint64_t)(n2 - 1) * NV_GSP_PAGE_SIZE)
        { printf("  ✗ lvl2[last] неверна\n"); ok = 0; }
    uint64_t expect_n2 = (s.fwimage_size + NV_GSP_PAGE_SIZE - 1) / NV_GSP_PAGE_SIZE;
    if (n2 != expect_n2) { printf("  ✗ n2 != ceil(fwimage/4K)\n"); ok = 0; }
    printf("  lvl0[0]=0x%llx lvl1[0]=0x%llx lvl2[0]=0x%llx lvl2[last]=0x%llx\n",
           (unsigned long long)ld64(lvl0), (unsigned long long)ld64(lvl1),
           (unsigned long long)ld64(lvl2),
           (unsigned long long)ld64(lvl2 + (size_t)(n2 - 1) * 8));

    free(lvl0); free(lvl1); free(lvl2);

    /* --- WPR2 layout + GspFwWprMeta (FB и FRTS — HW-значения из FWSEC-лога) --- */
    const uint64_t FB   = 12282ull << 20;     /* 12282 МиБ (NV_USABLE_FB_SIZE) */
    const uint64_t FRTS = 0x2ff800000ull;     /* nv_fb_compute_frts (HW-проверено) */
    const uint64_t FRTS_SZ = 0x100000ull;
    nv_gsp_fb_layout_t lay;
    rc = nv_gsp_fb_layout(FB, FRTS, FRTS_SZ, b.data_size, s.fwimage_size, &lay);
    if (rc != NV_GSP_OK) { fprintf(stderr, "FAIL: nv_gsp_fb_layout rc=%d\n", rc); return 1; }
    printf("WPR2 layout (FB=%llu МиБ):\n", (unsigned long long)(FB >> 20));
    printf("  non-WPR heap @0x%llx sz=0x%llx\n", (ull)lay.nonwpr_heap_addr, (ull)lay.nonwpr_heap_size);
    printf("  wpr2 @0x%llx sz=0x%llx end=0x%llx\n", (ull)lay.wpr2_addr, (ull)lay.wpr2_size, (ull)lay.wpr2_end);
    printf("  heap @0x%llx sz=0x%llx (%llu МиБ)\n", (ull)lay.heap_addr, (ull)lay.heap_size, (ull)(lay.heap_size>>20));
    printf("  elf  @0x%llx sz=0x%llx\n", (ull)lay.elf_addr, (ull)lay.elf_size);
    printf("  boot @0x%llx sz=0x%llx\n", (ull)lay.boot_addr, (ull)lay.boot_size);
    printf("  frts @0x%llx sz=0x%llx\n", (ull)lay.frts_addr, (ull)lay.frts_size);

    uint8_t meta[NV_GSP_WPR_META_SIZE];
    nv_gsp_wpr_meta_src_t src = {
        .radix3_lvl0_dma = L1, .radix3_elf_size = s.fwimage_size,
        .bootloader_dma = 0x30000000ull, .bootloader_size = b.data_size,
        .boot_code_offset = b.code_offset, .boot_data_offset = b.data_offset,
        .boot_manifest_offset = b.manifest_offset,
        .signature_dma = 0x31000000ull, .signature_size = s.sig_size,
        .vga_workspace_addr = FRTS + FRTS_SZ, .vga_workspace_size = 0,
    };
    rc = nv_gsp_wpr_meta_build(meta, sizeof(meta), &lay, &src);
    if (rc != NV_GSP_OK) { fprintf(stderr, "FAIL: wpr_meta_build rc=%d\n", rc); return 1; }
    if (ld64(meta + 0)   != NV_GSP_WPR_META_MAGIC) { printf("  ✗ magic\n"); ok = 0; }
    if (ld64(meta + 24)  != s.fwimage_size)        { printf("  ✗ sizeOfRadix3Elf\n"); ok = 0; }
    if (ld64(meta + 112) != lay.wpr2_addr)         { printf("  ✗ gspFwWprStart\n"); ok = 0; }
    if (ld64(meta + 168) != lay.wpr2_end)          { printf("  ✗ gspFwWprEnd\n"); ok = 0; }
    if (ld64(meta + 176) != FB)                    { printf("  ✗ fbSize\n"); ok = 0; }
    printf("GspFwWprMeta: magic=0x%llx wprStart=0x%llx wprEnd=0x%llx heapSize=0x%llx (256б)\n",
           (ull)ld64(meta+0), (ull)ld64(meta+112), (ull)ld64(meta+168), (ull)ld64(meta+128));

    printf(ok ? "\n=== РЕЗУЛЬТАТ: OK (секции/desc/radix3/WPR2-layout/WprMeta согласованы) ===\n"
              : "\n=== РЕЗУЛЬТАТ: FAIL (см. ✗) ===\n");
    return ok ? 0 : 1;
}
