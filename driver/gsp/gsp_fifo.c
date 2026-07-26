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

static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

int nv_gsp_fifo_get_device_info(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hSubdevice,
                                nv_gsp_fifo_devinfo *out, uint32_t *status)
{
    if (!ch || !out) return NV_GSP_RM_ERR_ARG;
    for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    /* NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS (3212б): baseIndex=0 (первая пачка). */
    static uint8_t p[NV_FIFO_DEVINFO_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV_FIFO_DEVINFO_BASEINDEX_OFF, 0u);

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hSubdevice,
                               NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE,
                               p, NV_FIFO_DEVINFO_PARAMS_SIZE, &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    if (st != 0) return NV_GSP_RM_ERR_BOUNDS;

    uint32_t n = ld32(p + NV_FIFO_DEVINFO_NUMENTRIES_OFF);
    if (n > NV_FIFO_DEVINFO_MAX_ENTRIES) n = NV_FIFO_DEVINFO_MAX_ENTRIES;  /* самозащита */
    out->count = n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *e = p + NV_FIFO_DEVINFO_ENTRIES_OFF
                         + (size_t)i * NV_FIFO_DEVINFO_ENTRY_SIZE;   /* engineData[16]@0 */
        out->engines[i].eng_desc =
            ld32(e + ENGINE_INFO_TYPE_ENG_DESC * 4u);
        out->engines[i].rm_engine_type =
            ld32(e + ENGINE_INFO_TYPE_RM_ENGINE_TYPE * 4u);
        out->engines[i].runlist =
            ld32(e + ENGINE_INFO_TYPE_RUNLIST * 4u);
        out->engines[i].runlist_pri_base =
            ld32(e + ENGINE_INFO_TYPE_RUNLIST_PRI_BASE * 4u);
    }
    return NV_GSP_RM_OK;
}

int nv_gsp_fifo_find_engine(const nv_gsp_fifo_devinfo *di, uint32_t rm_engine_type)
{
    if (!di) return -1;
    for (uint32_t i = 0; i < di->count; i++)
        if (di->engines[i].rm_engine_type == rm_engine_type) return (int)i;
    return -1;
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

uint32_t nv_gsp_fifo_build_sem_release(uint32_t *pb, uint64_t sem_va, uint32_t payload)
{
    uint32_t n = 0;
    /* INC_METHOD: 5 методов подряд с SEM_ADDR_LO (0x5c>>2=0x17), subch 0. */
    pb[n++] = (NVC56F_DMA_INCR_OPCODE_VALUE << 29) | (5u << 16) | (0u << 13)
            | ((NVC56F_SEM_ADDR_LO >> 2) & 0xfffu);
    pb[n++] = (uint32_t)(sem_va & 0xfffffffcu);         /* SEM_ADDR_LO (31:2) */
    pb[n++] = (uint32_t)((sem_va >> 32) & 0xffu);       /* SEM_ADDR_HI (7:0) */
    pb[n++] = payload;                                  /* SEM_PAYLOAD_LO */
    pb[n++] = 0u;                                       /* SEM_PAYLOAD_HI (32-бит payload) */
    pb[n++] = NVC56F_SEM_EXECUTE_OPERATION_RELEASE | NVC56F_SEM_EXECUTE_RELEASE_WFI_EN;
    return n;   /* 6 */
}

/* Заголовок метода пушбуфера: SEC_OP=INC_METHOD(1), count методов, подканал, adr=off>>2.
   Формат NV906F_DMA_* неизменен от Fermi до Ada. */
static uint32_t pb_hdr(uint32_t subch, uint32_t mthd, uint32_t count)
{
    return (NVC56F_DMA_INCR_OPCODE_VALUE << 29) | ((count & 0x1fffu) << 16)
         | ((subch & 0x7u) << 13) | ((mthd >> 2) & 0xfffu);
}

int nv_gsp_fifo_get_class_engineid(nv_gsp_rpc_chan *ch, uint32_t hClient,
                                   uint32_t hChannel, uint32_t hObject,
                                   uint32_t *out_class_engine_id, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV906F_CTRL_CLASS_ENGINEID_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + 0, hObject);          /* hObject */
    int rc = nv_gsp_rm_control(ch, hClient, hChannel,
                               NV906F_CTRL_GET_CLASS_ENGINEID,
                               p, sizeof(p), status);
    if (out_class_engine_id) *out_class_engine_id = ld32(p + 4);   /* classEngineID */
    return rc;
}

uint32_t nv_gsp_fifo_build_ce_copy(uint32_t *pb, uint32_t class_engine_id,
                                   uint64_t src_va, uint64_t dst_va, uint32_t bytes,
                                   uint64_t sem_va, uint32_t payload)
{
    if (!pb) return 0;
    const uint32_t sc = NV_FIFO_SUBCH_COPY;
    uint32_t n = 0;

    /* Привязать класс копирования к подканалу (один раз на канал). */
    if (class_engine_id) {
        pb[n++] = pb_hdr(sc, NVC56F_SET_OBJECT, 1u);
        pb[n++] = class_engine_id;
    }

    /* Семафор завершения: A=старшие 32 бита, B=младшие, затем payload. */
    pb[n++] = pb_hdr(sc, NVC7B5_SET_SEMAPHORE_A, 3u);
    pb[n++] = (uint32_t)((sem_va >> 32) & 0x1ffffu);
    pb[n++] = (uint32_t)(sem_va & 0xffffffffu);
    pb[n++] = payload;

    /* Адреса источника и приёмника — четыре метода подряд. */
    pb[n++] = pb_hdr(sc, NVC7B5_OFFSET_IN_UPPER, 4u);
    pb[n++] = (uint32_t)((src_va >> 32) & 0x1ffffu);
    pb[n++] = (uint32_t)(src_va & 0xffffffffu);
    pb[n++] = (uint32_t)((dst_va >> 32) & 0x1ffffu);
    pb[n++] = (uint32_t)(dst_va & 0xffffffffu);

    /* Одна строка длиной bytes. MULTI_LINE выключен, поэтому PITCH_* не нужны. */
    pb[n++] = pb_hdr(sc, NVC7B5_LINE_LENGTH_IN, 1u);
    pb[n++] = bytes;

    /* Запуск. Флаш + релиз семафора в одном LAUNCH_DMA — см. комментарий в
       заголовке про баг 1709888. DISABLE_PLC ставим как NVIDIA для этого класса. */
    pb[n++] = pb_hdr(sc, NVC7B5_LAUNCH_DMA, 1u);
    pb[n++] = NVC7B5_LD_XFER_NON_PIPELINED
            | NVC7B5_LD_FLUSH_ENABLE
            | NVC7B5_LD_SEM_RELEASE_ONE_WORD
            | NVC7B5_LD_SRC_LAYOUT_PITCH
            | NVC7B5_LD_DST_LAYOUT_PITCH
            | NVC7B5_LD_DISABLE_PLC;
    return n;
}

void nv_gsp_fifo_gpfifo_entry(uint64_t pb_va, uint32_t pb_dwords, uint32_t *e0, uint32_t *e1)
{
    if (e0) *e0 = (uint32_t)(pb_va & 0xfffffffcu);                  /* GET (31:2) */
    if (e1) *e1 = (uint32_t)((pb_va >> 32) & 0xffu)                 /* GET_HI (7:0) */
              | ((pb_dwords & 0x1fffffu) << 10);                    /* LENGTH (30:10) */
}

int nv_gsp_rm_ce_obj_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hChannel,
                           uint32_t hObject, uint32_t engineClass, uint32_t engineType,
                           uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    /* NVB0B5_ALLOCATION_PARAMETERS: version=1 (engineType = NV2080-ординал). */
    uint8_t p[NVB0B5_ALLOC_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NVB0B5_ALLOC_VERSION_OFF,    NVB0B5_ALLOCATION_PARAMETERS_VERSION_1);
    st32(p + NVB0B5_ALLOC_ENGINETYPE_OFF, engineType);
    return nv_gsp_rm_alloc(ch, hClient, hChannel, hObject, engineClass, p, sizeof(p), status);
}
