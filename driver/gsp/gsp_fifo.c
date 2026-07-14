/*
 * gsp_fifo.c — слой 4: канал GPFIFO + движок через GSP-RM (см. gsp_fifo.h).
 * Порт nouveau r535_chan_ramfc_write. Аллокации/контролы — через gsp_rm.{c,h}.
 */
#include "gsp_fifo.h"

static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void st64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

/* Записать NV_MEMORY_DESC_PARAMS (24б) по смещению off. */
static void wr_memdesc(uint8_t *p, uint32_t off, uint64_t base, uint64_t size,
                       uint32_t addrspace, uint32_t cacheattrib)
{
    st64(p + off + NV_MEMDESC_BASE_OFF,        base);
    st64(p + off + NV_MEMDESC_SIZE_OFF,        size);
    st32(p + off + NV_MEMDESC_ADDRSPACE_OFF,   addrspace);
    st32(p + off + NV_MEMDESC_CACHEATTRIB_OFF, cacheattrib);
}

int nv_gsp_rm_channel_alloc(nv_gsp_rpc_chan *ch, const nv_gsp_chan_cfg *cfg,
                            uint32_t *out_channel, uint32_t *status)
{
    if (!ch || !cfg) return NV_GSP_RM_ERR_ARG;

    uint8_t p[NV_CHANNEL_ALLOC_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;

    /* Кольцо GPFIFO в GPU-VA + число записей (8 байт/запись). */
    st64(p + NV_CHAN_GPFIFOOFFSET_OFF,  cfg->gpfifo_va);
    st32(p + NV_CHAN_GPFIFOENTRIES_OFF, cfg->gpfifo_entries);

    /* flags: PRIVILEGED (для kernel-канала) + USERD_INDEX_PAGE_FIXED; USERD индексы
       из chid (userd_i = chid%8, userd_p = chid/8), как r535_chan (CHID_PER_USERD=8). */
    uint32_t userd_i = cfg->chid % 8u;
    uint32_t userd_p = cfg->chid / 8u;
    uint32_t flags = NVOS04_FLAGS_USERD_INDEX_PAGE_FIXED_BIT
                   | (userd_i << NVOS04_FLAGS_USERD_INDEX_VALUE_SHIFT)
                   | (userd_p << NVOS04_FLAGS_USERD_INDEX_PAGE_VALUE_SHIFT);
    if (cfg->priv) flags |= NVOS04_FLAGS_PRIVILEGED_CHANNEL_BIT;
    st32(p + NV_CHAN_FLAGS_OFF, flags);

    st32(p + NV_CHAN_HVASPACE_OFF,   cfg->hVASpace);
    st32(p + NV_CHAN_ENGINETYPE_OFF, cfg->engineType);

    /* instance block (RAMFC внутри, off 0), USERD, RAMFC, method-buffer. */
    wr_memdesc(p, NV_CHAN_INSTANCEMEM_OFF, cfg->inst_phys, cfg->inst_size,
               NV_MEMDESC_ADDRSPACE_VIDMEM, 1u);
    wr_memdesc(p, NV_CHAN_USERDMEM_OFF, cfg->userd_phys, cfg->userd_size,
               NV_MEMDESC_ADDRSPACE_VIDMEM, 1u);
    wr_memdesc(p, NV_CHAN_RAMFCMEM_OFF, cfg->inst_phys,
               cfg->ramfc_size ? cfg->ramfc_size : 0x200u,
               NV_MEMDESC_ADDRSPACE_VIDMEM, 1u);
    {
        uint32_t as = cfg->mthdbuf_sysmem ? NV_MEMDESC_ADDRSPACE_SYSMEM
                                          : NV_MEMDESC_ADDRSPACE_VIDMEM;
        uint32_t ca = cfg->mthdbuf_sysmem ? 0u : 1u;
        wr_memdesc(p, NV_CHAN_MTHDBUFMEM_OFF, cfg->mthdbuf_phys, cfg->mthdbuf_size, as, ca);
    }
    /* internalFlags=0: PRIVILEGE=USER, ERROR/ECC_NOTIFIER_TYPE=NONE (TODO: verify HW). */

    uint32_t h = NV_GSP_RM_CHANNEL_HANDLE | cfg->chid;
    int rc = nv_gsp_rm_alloc(ch, cfg->hClient, cfg->hDevice, h,
                             AMPERE_CHANNEL_GPFIFO_A, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_channel) *out_channel = h;
    return rc;
}

int nv_gsp_rm_channel_bind(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hChannel,
                           uint32_t engineType, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NVA06F_CTRL_BIND_PARAMS_SIZE];
    st32(p, engineType);
    return nv_gsp_rm_control(ch, hClient, hChannel, NVA06F_CTRL_CMD_BIND,
                             p, sizeof(p), status);
}

int nv_gsp_rm_channel_schedule(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hChannel,
                               int enable, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NVA06F_CTRL_SCHEDULE_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    p[0] = enable ? 1u : 0u;   /* bEnable (NvBool) */
    p[1] = 0u;                 /* bSkipSubmit */
    return nv_gsp_rm_control(ch, hClient, hChannel, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE,
                             p, sizeof(p), status);
}

int nv_gsp_rm_engine_obj_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hChannel,
                               uint32_t hObject, uint32_t engineClass, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    /* Объект движка вешается на канал (hParent=канал). params пустые. */
    return nv_gsp_rm_alloc(ch, hClient, hChannel, hObject, engineClass, NULL, 0, status);
}
