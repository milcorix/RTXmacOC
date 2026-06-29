/*
 * gsp_fw.c — реализация подготовки GSP-RM (см. gsp_fw.h).
 *
 * RM_RISCV_UCODE_DESC (nouveau nvrm/gsp.h, 21×u32 от header_offset bin-контейнера):
 *   version@0, bootloaderOffset@4, bootloaderSize@8, bootloaderParamOffset@12,
 *   bootloaderParamSize@16, riscvElfOffset@20, riscvElfSize@24, appVersion@28,
 *   manifestOffset@32, manifestSize@36, monitorDataOffset@40, monitorDataSize@44,
 *   monitorCodeOffset@48, monitorCodeSize@52, ... (всего 84 байта).
 * radix3 — nouveau r535 (3 уровня, 4K, u64-записи).
 */
#include "gsp_fw.h"
#include "elf64.h"

static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void st64(uint8_t *p, uint64_t v)
{ for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static int fits(uint64_t off, uint64_t need, size_t len)
{ return off <= len && need <= (uint64_t)len - off; }

#define BIN_MAGIC 0x10deu

int nv_gsp_rm_sections(const uint8_t *elf, size_t len, const char *chip_sig,
                       nv_gsp_sections_t *out)
{
    if (!elf || !chip_sig || !out) return NV_GSP_ERR_ARG;
    int rc = nv_elf64_section(elf, len, ".fwimage", &out->fwimage_off, &out->fwimage_size);
    if (rc == NV_ELF_ERR_NOSEC) return NV_GSP_ERR_NOSEC;
    if (rc != NV_ELF_OK) return (rc == NV_ELF_ERR_BOUNDS) ? NV_GSP_ERR_BOUNDS : NV_GSP_ERR_FORMAT;
    rc = nv_elf64_section(elf, len, chip_sig, &out->sig_off, &out->sig_size);
    if (rc == NV_ELF_ERR_NOSEC) return NV_GSP_ERR_NOSEC;
    if (rc != NV_ELF_OK) return (rc == NV_ELF_ERR_BOUNDS) ? NV_GSP_ERR_BOUNDS : NV_GSP_ERR_FORMAT;
    return NV_GSP_OK;
}

int nv_gsp_bootloader_parse(const uint8_t *buf, size_t len, nv_gsp_bootloader_t *out)
{
    if (!buf || !out) return NV_GSP_ERR_ARG;
    if (!fits(0, 24, len)) return NV_GSP_ERR_BOUNDS;
    if (ld32(buf + 0) != BIN_MAGIC) return NV_GSP_ERR_FORMAT;
    uint32_t hdr_off  = ld32(buf + 12);
    uint32_t data_off = ld32(buf + 16);
    uint32_t data_sz  = ld32(buf + 20);
    if (!fits(data_off, data_sz, len)) return NV_GSP_ERR_BOUNDS;
    if (!fits(hdr_off, 84, len)) return NV_GSP_ERR_BOUNDS; /* RM_RISCV_UCODE_DESC = 84б */

    const uint8_t *d = buf + hdr_off;
    out->data_abs        = data_off;
    out->data_size       = data_sz;
    out->app_version     = ld32(d + 28);
    out->manifest_offset = ld32(d + 32);
    out->manifest_size   = ld32(d + 36);
    out->data_offset     = ld32(d + 40);  /* monitorDataOffset */
    out->code_offset     = ld32(d + 48);  /* monitorCodeOffset */
    return NV_GSP_OK;
}

/* ===================== WPR2 layout + GspFwWprMeta ===================== */

static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1u); }
static uint64_t align_up(uint64_t v, uint64_t a)   { return (v + a - 1u) & ~(a - 1u); }

int nv_gsp_fb_layout(uint64_t fb_size, uint64_t frts_addr, uint64_t frts_size,
                     uint64_t boot_size, uint64_t elf_size, nv_gsp_fb_layout_t *out)
{
    if (!out || fb_size == 0 || frts_size == 0 || boot_size == 0 || elf_size == 0)
        return NV_GSP_ERR_ARG;

    out->fb_size   = fb_size;
    out->frts_addr = frts_addr;
    out->frts_size = frts_size;
    /* WPR2 верхняя граница = конец FRTS (= align_down(vga,128K), как в nouveau). */
    out->wpr2_end  = frts_addr + frts_size;

    /* boot — ниже FRTS, выравнивание 4K. */
    out->boot_size = boot_size;
    out->boot_addr = align_down(frts_addr - boot_size, 0x1000u);
    /* elf — ниже boot, выравнивание 64K. */
    out->elf_size  = elf_size;
    out->elf_addr  = align_down(out->boot_addr - elf_size, 0x10000u);

    /* heap — размер по формуле (535.113.01), затем расположение ниже elf (1M). */
    uint64_t fb_gb = (fb_size + (1ull << 30) - 1) >> 30;          /* DIV_ROUND_UP до ГБ */
    uint64_t heap = (uint64_t)NV_GSP_WPR_HEAP_OS_CARVEOUT
                  + (uint64_t)NV_GSP_WPR_HEAP_BASE_SIZE
                  + align_up((uint64_t)NV_GSP_HEAP_SIZE_PER_GB_FB * fb_gb, 1u << 20)
                  + align_up(NV_GSP_HEAP_CLIENT_ALLOC_SIZE, 1u << 20);
    if (heap < NV_GSP_WPR_HEAP_MIN_SIZE) heap = NV_GSP_WPR_HEAP_MIN_SIZE;
    out->heap_addr = align_down(out->elf_addr - heap, 0x100000u);
    out->heap_size = align_down(out->elf_addr - out->heap_addr, 0x100000u);

    /* wpr2-контейнер: ниже heap, с местом под GspFwWprMeta; 1M. */
    out->wpr2_addr = align_down(out->heap_addr - NV_GSP_WPR_META_SIZE, 0x100000u);
    out->wpr2_size = out->wpr2_end - out->wpr2_addr;

    /* non-WPR heap (1M) сразу под WPR2. */
    out->nonwpr_heap_size = 0x100000u;
    out->nonwpr_heap_addr = out->wpr2_addr - out->nonwpr_heap_size;

    /* Санити: всё внутри FB и упорядочено. */
    if (out->wpr2_end > fb_size) return NV_GSP_ERR_BOUNDS;
    if (!(out->nonwpr_heap_addr < out->wpr2_addr &&
          out->wpr2_addr <= out->heap_addr &&
          out->heap_addr < out->elf_addr &&
          out->elf_addr < out->boot_addr &&
          out->boot_addr < out->frts_addr))
        return NV_GSP_ERR_BOUNDS;
    return NV_GSP_OK;
}

int nv_gsp_wpr_meta_build(uint8_t *buf, size_t buflen,
                          const nv_gsp_fb_layout_t *lay,
                          const nv_gsp_wpr_meta_src_t *src)
{
    if (!buf || !lay || !src || buflen < NV_GSP_WPR_META_SIZE) return NV_GSP_ERR_ARG;
    for (size_t i = 0; i < NV_GSP_WPR_META_SIZE; i++) buf[i] = 0;

    st64(buf + 0,   NV_GSP_WPR_META_MAGIC);
    st64(buf + 8,   NV_GSP_WPR_META_REVISION);
    st64(buf + 16,  src->radix3_lvl0_dma);     /* sysmemAddrOfRadix3Elf */
    st64(buf + 24,  src->radix3_elf_size);     /* sizeOfRadix3Elf */
    st64(buf + 32,  src->bootloader_dma);      /* sysmemAddrOfBootloader */
    st64(buf + 40,  src->bootloader_size);     /* sizeOfBootloader */
    st64(buf + 48,  src->boot_code_offset);    /* bootloaderCodeOffset */
    st64(buf + 56,  src->boot_data_offset);    /* bootloaderDataOffset */
    st64(buf + 64,  src->boot_manifest_offset);/* bootloaderManifestOffset */
    st64(buf + 72,  src->signature_dma);       /* sysmemAddrOfSignature */
    st64(buf + 80,  src->signature_size);      /* sizeOfSignature */
    st64(buf + 88,  lay->nonwpr_heap_addr);    /* gspFwRsvdStart */
    st64(buf + 96,  lay->nonwpr_heap_addr);    /* nonWprHeapOffset */
    st64(buf + 104, lay->nonwpr_heap_size);    /* nonWprHeapSize */
    st64(buf + 112, lay->wpr2_addr);           /* gspFwWprStart */
    st64(buf + 120, lay->heap_addr);           /* gspFwHeapOffset */
    st64(buf + 128, lay->heap_size);           /* gspFwHeapSize */
    st64(buf + 136, lay->elf_addr);            /* gspFwOffset */
    st64(buf + 144, lay->boot_addr);           /* bootBinOffset */
    st64(buf + 152, lay->frts_addr);           /* frtsOffset */
    st64(buf + 160, lay->frts_size);           /* frtsSize */
    st64(buf + 168, lay->wpr2_end);            /* gspFwWprEnd */
    st64(buf + 176, lay->fb_size);             /* fbSize */
    st64(buf + 184, src->vga_workspace_addr);  /* vgaWorkspaceOffset */
    st64(buf + 192, src->vga_workspace_size);  /* vgaWorkspaceSize */
    /* bootCount@200, union@208..240, vfPartitionCount@240, verified@248 — нули. */
    return NV_GSP_OK;
}

int nv_gsp_radix3_levels(uint64_t fwimage_size, uint32_t *n2, uint32_t *lvl2_pages)
{
    if (!n2 || !lvl2_pages || fwimage_size == 0) return NV_GSP_ERR_ARG;
    uint64_t pages = (fwimage_size + NV_GSP_PAGE_SIZE - 1) / NV_GSP_PAGE_SIZE; /* записей в lvl2 */
    uint64_t l2_bytes = pages * 8u;
    uint64_t l2_pages = (l2_bytes + NV_GSP_PAGE_SIZE - 1) / NV_GSP_PAGE_SIZE;  /* страниц lvl2 = записей lvl1 */
    /* lvl1 — одна страница: до 512 записей. Ёмкость radix3 = 512*512 страниц (1 ГБ). */
    if (l2_pages > NV_GSP_RADIX3_ENTRIES_PER_PAGE) return NV_GSP_ERR_CAP;
    *n2 = (uint32_t)pages;
    *lvl2_pages = (uint32_t)l2_pages;
    return NV_GSP_OK;
}

int nv_gsp_radix3_fill(uint64_t fwimage_size,
                       uint8_t *lvl0, uint8_t *lvl1, uint8_t *lvl2,
                       uint64_t dma_fwimage, uint64_t dma_lvl2, uint64_t dma_lvl1)
{
    if (!lvl0 || !lvl1 || !lvl2) return NV_GSP_ERR_ARG;
    uint32_t n2 = 0, l2_pages = 0;
    int rc = nv_gsp_radix3_levels(fwimage_size, &n2, &l2_pages);
    if (rc != NV_GSP_OK) return rc;

    /* lvl2: запись на каждую 4K-страницу образа. */
    for (uint32_t i = 0; i < n2; i++)
        st64(lvl2 + (size_t)i * 8u, dma_fwimage + (uint64_t)i * NV_GSP_PAGE_SIZE);
    /* lvl1: запись на каждую страницу lvl2. */
    for (uint32_t j = 0; j < l2_pages; j++)
        st64(lvl1 + (size_t)j * 8u, dma_lvl2 + (uint64_t)j * NV_GSP_PAGE_SIZE);
    /* lvl0: единственная запись → страница lvl1. */
    st64(lvl0 + 0, dma_lvl1);
    return NV_GSP_OK;
}
