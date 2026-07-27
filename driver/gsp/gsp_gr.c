/*
 * gsp_gr.c — реализация привилегированных запросов GR/прерываний (см. gsp_gr.h).
 *
 * Здесь только разбор ответов. Отправка — общим путём nv_gsp_rm_control, тем
 * самым, которым уже работают FB_GET_INFO_V2 и дисплейные INTERNAL-контролы.
 */
#include "gsp_gr.h"

static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint16_t ld16(const uint8_t *p)
{ return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1]<<8)); }

int nv_gsp_gr_get_ctxbuf_info(nv_gsp_rpc_chan *ch, uint32_t hIntClient,
                              uint32_t hIntSubdevice, uint32_t gr_index,
                              nv_gsp_gr_ctxbufs *out, uint32_t *status)
{
    if (!ch || !out || gr_index >= NV_GR_CTXBUF_MAX_ENGINES) return NV_GSP_RM_ERR_ARG;

    static uint8_t p[NV_GR_CTXBUF_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;

    int rc = nv_gsp_rm_control(ch, hIntClient, hIntSubdevice,
                               NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO,
                               p, sizeof(p), status);

    nv_gsp_gr_parse_ctxbuf_info(p, gr_index, out);
    if (rc != NV_GSP_RM_OK) {
        for (unsigned i = 0; i < NV_GR_CTXBUF_MAX_IDS; i++) {
            out->buf[i].size = 0; out->buf[i].alignment = 0;
        }
        out->count_nonzero = 0;
    }
    return rc;
}

void nv_gsp_gr_parse_ctxbuf_info(const uint8_t *params, uint32_t gr_index,
                                 nv_gsp_gr_ctxbufs *out)
{
    if (!params || !out || gr_index >= NV_GR_CTXBUF_MAX_ENGINES) return;
    out->count_nonzero = 0;
    const uint8_t *eng = params + (size_t)gr_index * NV_GR_CTXBUF_ENGINE_STRIDE;
    for (unsigned i = 0; i < NV_GR_CTXBUF_MAX_IDS; i++) {
        out->buf[i].size      = ld32(eng + i * 8u + 0u);
        out->buf[i].alignment = ld32(eng + i * 8u + 4u);
        if (out->buf[i].size) out->count_nonzero++;
    }
}

int nv_gsp_intr_get_table(nv_gsp_rpc_chan *ch, uint32_t hIntClient,
                          uint32_t hIntSubdevice,
                          nv_gsp_intr_table *out, uint32_t *status)
{
    if (!ch || !out) return NV_GSP_RM_ERR_ARG;

    static uint8_t p[NV_INTR_TABLE_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;

    int rc = nv_gsp_rm_control(ch, hIntClient, hIntSubdevice,
                               NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE,
                               p, sizeof(p), status);
    out->count = 0;
    if (rc != NV_GSP_RM_OK) return rc;
    nv_gsp_intr_parse_table(p, out);
    return rc;
}

void nv_gsp_intr_parse_table(const uint8_t *params, nv_gsp_intr_table *out)
{
    if (!params || !out) return;
    uint32_t n = ld32(params + 0);
    if (n > NV_INTR_TABLE_MAX_ENTRIES) n = NV_INTR_TABLE_MAX_ENTRIES;
    out->count = n;
    /* Записи начинаются с offset 4; шаг 16 (engineIdx 16-битный + 2 байта пада). */
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = params + 4u + (size_t)i * NV_INTR_TABLE_ENTRY_STRIDE;
        out->entry[i].engine_idx      = ld16(e + 0u);
        out->entry[i].pmc_intr_mask   = ld32(e + 4u);
        out->entry[i].vector_stall    = ld32(e + 8u);
        out->entry[i].vector_nonstall = ld32(e + 12u);
    }
}
