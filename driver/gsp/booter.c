/*
 * booter.c — реализация разбора контейнера Booter (см. booter.h).
 *
 * Формат (сверено с дампом booter_load-535.113.01.bin):
 *   [0]   nvfw_bin_hdr: magic u32(0x10de), ver, size, header_offset, data_offset, data_size
 *   [hdr] nvfw_hs_header_v2 (9×u32): sig_prod_offset, sig_prod_size, patch_loc_off,
 *         patch_sig_off, meta_data_offset, meta_data_size, num_sig_off, load_hdr_off, header_size
 *         — поля *_off — это АБСОЛЮТНЫЕ смещения в блобе к значениям/структурам.
 *   [load_hdr] nvfw_hs_load_header_v2: os_code_offset, os_code_size, os_data_offset,
 *         os_data_size, num_apps, app[]{offset,size,data_offset,data_size}
 *         — смещения os_code/os_data/app относительны региона data (data_offset).
 *   pkc_data_offset = patch_loc - os_data_offset (вход для BROM при запуске).
 * Источник: nouveau include/nvfw/{fw,hs}.h, nova-core firmware/{hs,booter}.rs.
 */
#include "booter.h"

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* off..off+need должно укладываться в len. */
static int fits(size_t off, size_t need, size_t len)
{
    return off <= len && need <= len - off;
}

int nv_booter_parse(const uint8_t *buf, size_t len, nv_booter_desc_t *out)
{
    if (!buf || !out) return NV_BOOTER_ERR_ARG;
    for (size_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    /* --- bin header (24 байта) --- */
    if (!fits(0, 24, len)) return NV_BOOTER_ERR_BOUNDS;
    if (ld32(buf + 0) != NV_BOOTER_BIN_MAGIC) return NV_BOOTER_ERR_MAGIC;
    uint32_t bin_hdr_off  = ld32(buf + 12);
    uint32_t data_off     = ld32(buf + 16);
    uint32_t data_size    = ld32(buf + 20);
    if (!fits(data_off, data_size, len)) return NV_BOOTER_ERR_BOUNDS;
    out->data_abs   = data_off;
    out->data_size  = data_size;

    /* --- HS header v2 (36 байт = 9×u32) --- */
    if (!fits(bin_hdr_off, 36, len)) return NV_BOOTER_ERR_BOUNDS;
    const uint8_t *h = buf + bin_hdr_off;
    uint32_t sig_prod_off  = ld32(h + 0);
    uint32_t sig_prod_size = ld32(h + 4);
    uint32_t patch_loc_off = ld32(h + 8);
    uint32_t patch_sig_off = ld32(h + 12);
    uint32_t meta_off      = ld32(h + 16);
    uint32_t meta_size     = ld32(h + 20);
    uint32_t num_sig_off   = ld32(h + 24);
    uint32_t load_hdr_off  = ld32(h + 28);
    /* h+32 = header_size (информативно) */

    /* Подписи. */
    if (!fits(sig_prod_off, sig_prod_size, len)) return NV_BOOTER_ERR_BOUNDS;
    out->sig_prod_abs  = sig_prod_off;
    out->sig_prod_size = sig_prod_size;

    /* Косвенные значения (по абсолютным смещениям). */
    if (!fits(patch_loc_off, 4, len) || !fits(patch_sig_off, 4, len) ||
        !fits(num_sig_off, 4, len))
        return NV_BOOTER_ERR_BOUNDS;
    out->patch_loc = ld32(buf + patch_loc_off);
    out->patch_sig = ld32(buf + patch_sig_off);
    out->num_sig   = ld32(buf + num_sig_off);

    /* num_sig должен соответствовать размеру блока подписей. */
    if ((uint64_t)out->num_sig * NV_BOOTER_SIG_SIZE != sig_prod_size)
        return NV_BOOTER_ERR_LAYOUT;

    /* meta_data (параметры подписи: engine_id/ucode_id и пр. — TODO: verify). */
    out->meta_abs  = meta_off;
    out->meta_size = meta_size;
    for (unsigned i = 0; i < 3 && (i + 1) * 4 <= meta_size; i++) {
        if (!fits(meta_off + i * 4, 4, len)) return NV_BOOTER_ERR_BOUNDS;
        out->meta[i] = ld32(buf + meta_off + i * 4);
    }

    /* --- load header v2 --- */
    if (!fits(load_hdr_off, 20, len)) return NV_BOOTER_ERR_BOUNDS;
    const uint8_t *lh = buf + load_hdr_off;
    out->os_code_offset = ld32(lh + 0);
    out->os_code_size   = ld32(lh + 4);
    out->os_data_offset = ld32(lh + 8);
    out->os_data_size   = ld32(lh + 12);
    out->num_apps       = ld32(lh + 16);
    if (out->num_apps > NV_BOOTER_MAX_APPS) return NV_BOOTER_ERR_LAYOUT;
    if (!fits(load_hdr_off + 20, (size_t)out->num_apps * 16, len))
        return NV_BOOTER_ERR_BOUNDS;
    for (uint32_t i = 0; i < out->num_apps; i++) {
        const uint8_t *a = lh + 20 + i * 16;
        out->app[i].code_offset = ld32(a + 0);
        out->app[i].code_size   = ld32(a + 4);
        out->app[i].data_offset = ld32(a + 8);
        out->app[i].data_size   = ld32(a + 12);
    }

    /* Регион данных должен вмещать os_code и os_data. */
    if ((uint64_t)out->os_code_offset + out->os_code_size > data_size ||
        (uint64_t)out->os_data_offset + out->os_data_size > data_size)
        return NV_BOOTER_ERR_LAYOUT;

    /* pkc_data_offset для BROM (как у FWSEC: смещение подписи в DMEM). */
    if (out->patch_loc < out->os_data_offset) return NV_BOOTER_ERR_LAYOUT;
    out->pkc_data_offset = out->patch_loc - out->os_data_offset;

    return NV_BOOTER_OK;
}
