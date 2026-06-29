/*
 * gsp_fw.h — подготовка прошивки GSP-RM к загрузке (задача 6, фаза 4).
 *
 * Включает:
 *  - извлечение секций образа GSP-RM (ELF): .fwimage + .fwsignature_<chip>;
 *  - разбор дескриптора GSP-bootloader (bin-контейнер RM_RISCV_UCODE_DESC);
 *  - построение radix3 (3-уровневые 4K-таблицы над .fwimage для GSP MMU).
 *
 * GspFwWprMeta (раскладка региона WPR2) — отдельный модуль/шаг (математика layout).
 * Источники: nouveau r535 (radix3, RM_RISCV_UCODE_DESC), nova firmware/gsp.rs.
 * Пересказано для соответствия лицензии; раскладки — docs/PORTING-MAP.md.
 */
#ifndef RTXMACOC_GSP_FW_H
#define RTXMACOC_GSP_FW_H

#include <stdint.h>
#include <stddef.h>

#define NV_GSP_OK            0
#define NV_GSP_ERR_ARG     (-1)
#define NV_GSP_ERR_BOUNDS  (-2)
#define NV_GSP_ERR_FORMAT  (-3)
#define NV_GSP_ERR_NOSEC   (-4)
#define NV_GSP_ERR_CAP     (-5)  /* образ больше ёмкости radix3 (1 ГБ) */

#define NV_GSP_PAGE_SHIFT  12u
#define NV_GSP_PAGE_SIZE   (1u << NV_GSP_PAGE_SHIFT)   /* 4096 */
#define NV_GSP_RADIX3_ENTRIES_PER_PAGE (NV_GSP_PAGE_SIZE / 8u) /* 512 u64-записей */

/* Секции образа GSP-RM (ELF): прошивка и подпись под наш чип. */
typedef struct {
    uint64_t fwimage_off, fwimage_size;   /* .fwimage */
    uint64_t sig_off, sig_size;           /* .fwsignature_<chip> */
} nv_gsp_sections_t;

/*
 * Найти .fwimage и .fwsignature_<chip> в ELF GSP-RM. chip_sig — имя секции подписи
 * (напр. ".fwsignature_ad10x" для AD104).
 */
int nv_gsp_rm_sections(const uint8_t *elf, size_t len, const char *chip_sig,
                       nv_gsp_sections_t *out);

/* Дескриптор GSP-bootloader (нужные поля RM_RISCV_UCODE_DESC + payload-регион bin). */
typedef struct {
    uint32_t data_abs;          /* bin data_offset: начало payload bootloader */
    uint32_t data_size;         /* bin data_size */
    uint32_t app_version;       /* appVersion (пишется в FALCON_OS при init) */
    uint32_t code_offset;       /* monitorCodeOffset */
    uint32_t data_offset;       /* monitorDataOffset */
    uint32_t manifest_offset;   /* manifestOffset */
    uint32_t manifest_size;     /* manifestSize */
} nv_gsp_bootloader_t;

/* Разобрать bin-контейнер GSP-bootloader. */
int nv_gsp_bootloader_parse(const uint8_t *buf, size_t len, nv_gsp_bootloader_t *out);

/* ===================== WPR2 layout + GspFwWprMeta (фаза 4 ч.2) ===================== */

/* Константы размера GSP-heap (535.113.01). wpr_heap — ga102/ad10x (nouveau ad102.c);
   per-GB и client_alloc — OGK gsp_fw_heap.h. TODO: verify на HW (фаза 6). */
#define NV_GSP_WPR_HEAP_OS_CARVEOUT   (20u << 20)        /* 20 МиБ */
#define NV_GSP_WPR_HEAP_BASE_SIZE     (8u  << 20)        /* 8 МиБ  */
#define NV_GSP_WPR_HEAP_MIN_SIZE      (84u << 20)        /* 84 МиБ */
#define NV_GSP_HEAP_SIZE_PER_GB_FB    (96u << 10)        /* 96 КиБ на ГБ FB */
#define NV_GSP_HEAP_CLIENT_ALLOC_SIZE (((uint64_t)(48u << 10)) * 2048u) /* 96 МиБ */

#define NV_GSP_WPR_META_SIZE     256u
#define NV_GSP_WPR_META_MAGIC    0xdc3aae21371a60b3ULL
#define NV_GSP_WPR_META_REVISION 1u

/* Раскрой WPR2 в VRAM (адреса — смещения в FB). Порт nouveau r535_gsp_oneinit. */
typedef struct {
    uint64_t fb_size;
    uint64_t frts_addr, frts_size;          /* из nv_fb_compute_frts (HW-проверено) */
    uint64_t boot_addr, boot_size;
    uint64_t elf_addr, elf_size;
    uint64_t heap_addr, heap_size;          /* wpr2 heap */
    uint64_t wpr2_addr, wpr2_size, wpr2_end;
    uint64_t nonwpr_heap_addr, nonwpr_heap_size;
} nv_gsp_fb_layout_t;

/*
 * Рассчитать раскрой WPR2 из размера FB и опорного региона FRTS (frts_addr/size —
 * результат nv_fb_compute_frts, HW-проверенный). boot_size = размер payload
 * GSP-bootloader; elf_size = размер .fwimage (radix3-mapped). Регионы режутся сверху
 * вниз: [non-WPR heap][wpr2: meta|heap|elf|boot|frts][vga]. Возвращает NV_GSP_OK.
 */
int nv_gsp_fb_layout(uint64_t fb_size, uint64_t frts_addr, uint64_t frts_size,
                     uint64_t boot_size, uint64_t elf_size, nv_gsp_fb_layout_t *out);

/* Параметры sysmem-размещения прошивок (DMA-адреса в системной памяти). */
typedef struct {
    uint64_t radix3_lvl0_dma, radix3_elf_size;  /* sysmemAddrOfRadix3Elf, sizeOfRadix3Elf */
    uint64_t bootloader_dma, bootloader_size;   /* sysmemAddrOfBootloader, sizeOfBootloader */
    uint32_t boot_code_offset, boot_data_offset, boot_manifest_offset;
    uint64_t signature_dma, signature_size;     /* sysmemAddrOfSignature, sizeOfSignature */
    uint64_t vga_workspace_addr, vga_workspace_size;
} nv_gsp_wpr_meta_src_t;

/*
 * Заполнить 256-байтную структуру GspFwWprMeta (buf >= 256, обнуляется внутри) из
 * раскроя FB (lay) и sysmem-адресов (src). Порт nouveau r535_gsp_wpr_meta_init.
 */
int nv_gsp_wpr_meta_build(uint8_t *buf, size_t buflen,
                          const nv_gsp_fb_layout_t *lay,
                          const nv_gsp_wpr_meta_src_t *src);

/* Размеры radix3 для образа размером fwimage_size:
 *   n2  = ceil(fwimage_size / 4K)          — записей в lvl2 (по странице образа);
 *   lvl2_pages = ceil(n2*8 / 4K)           — страниц под lvl2 (= записей в lvl1);
 *   lvl1/lvl0 — по 1 странице. Ёмкость: 512*512 страниц = 1 ГБ. */
int nv_gsp_radix3_levels(uint64_t fwimage_size, uint32_t *n2, uint32_t *lvl2_pages);

/*
 * Заполнить radix3 для СПЛОШНЫХ (contiguous) буферов:
 *   lvl2[i] = dma_fwimage + i*4K  (i: 0..n2-1)
 *   lvl1[j] = dma_lvl2     + j*4K  (j: 0..lvl2_pages-1)
 *   lvl0[0] = dma_lvl1
 * Указатели lvlN — на выделенные (обнулённые) буферы; dma_* — их IOVA/физ-адреса.
 * lvl0/lvl1 — по 1 странице; lvl2 — lvl2_pages страниц.
 */
int nv_gsp_radix3_fill(uint64_t fwimage_size,
                       uint8_t *lvl0, uint8_t *lvl1, uint8_t *lvl2,
                       uint64_t dma_fwimage, uint64_t dma_lvl2, uint64_t dma_lvl1);

#endif /* RTXMACOC_GSP_FW_H */
