/*
 * gsp_rm.c — реализация двустороннего RPC к GSP-RM + базовых RM-объектов (см. gsp_rm.h).
 * Порт nouveau r535.c. Платформенная привязка (MMIO/задержка) — через колбэки канала.
 *
 * PROBE: смещения GspStaticConfigInfo (NV_GSP_SCI_*) получены компиляцией
 * структуры из gsp_static_config.h (535.113.01) против SDK-заголовков с offsetof:
 *   sizeof=2168, fbRegionInfoParams=840, bar1PdeBase=2088, bar2PdeBase=2096,
 *   hInternalClient=2152, hInternalDevice=2156, hInternalSubdevice=2160.
 * На железе подкрепляются самопроверкой FB-карты (см. nv_gsp_get_static_info).
 */
#include "gsp_rm.h"

#define GSP_PAGE 0x1000u

static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static uint64_t ld64(const uint8_t *p)
{ uint64_t v=0; for (int i=0;i<8;i++) v |= (uint64_t)p[i]<<(8*i); return v; }
static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void st64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

/* Прочитать msgqRxHeader.readPtr нашего msgq (мы — потребитель статус-очереди). */
static void msgq_set_readptr(nv_gsp_rpc_chan *ch, uint32_t rptr)
{
    volatile uint8_t *q = ch->shm + ch->lay.msgq_off + NV_GSP_MSGQ_RXHDROFF;
    st32((uint8_t *)q, rptr);
}

int nv_gsp_rpc_call(nv_gsp_rpc_chan *ch, uint32_t fn,
                    const uint8_t *req, uint32_t req_len,
                    uint8_t *reply_out, uint32_t reply_cap, uint32_t *reply_len,
                    uint32_t *rpc_result, uint32_t timeout_us)
{
    if (!ch || !ch->shm) return NV_GSP_RM_ERR_ARG;
    if (!timeout_us) timeout_us = 4000000u;

    /* --- send: построить элемент в текущем слоте cmdq, продвинуть writePtr, позвонить --- */
    uint8_t *cmdq = ch->shm + ch->lay.cmdq_off;
    uint32_t ec = nv_gsp_cmdq_write(cmdq, ch->cmdq_wptr, ch->seq, fn, req, req_len);
    ch->cmdq_wptr += ec;
    while (ch->cmdq_wptr >= ch->lay.msg_count) ch->cmdq_wptr -= ch->lay.msg_count;
    nv_gsp_cmdq_set_writeptr(ch->shm, &ch->lay, ch->cmdq_wptr);
    ch->seq++;
    if (ch->ring) ch->ring(ch->io_ctx);

    /* --- recv: опрос msgq до ответа той же функции (async-события по пути — пропуск) --- */
    const uint32_t step = 1000u;           /* шаг опроса, мкс */
    uint32_t waited = 0;
    for (;;) {
        uint32_t wptr = nv_gsp_msgq_writeptr(ch->shm, &ch->lay);
        while (ch->msgq_rptr != wptr) {
            const uint8_t *e = ch->shm + ch->lay.msgq_off + NV_GSP_QUEUE_ENTRYOFF
                             + (size_t)ch->msgq_rptr * NV_GSP_QUEUE_MSGSIZE;
            uint32_t mec = ld32(e + NV_GSP_MSG_ELEMCOUNT_OFF);
            if (!mec || mec > ch->lay.msg_count) mec = 1;
            const uint8_t *r = e + NV_GSP_RPC_HDR_OFF;
            uint32_t mfn  = ld32(r + NV_GSP_RPC_F_FUNCTION);
            uint32_t mlen = ld32(r + NV_GSP_RPC_F_LENGTH);
            uint32_t mres = ld32(r + 16);   /* rpc_result @ rpc-заголовок+16 */

            if (mfn == fn) {
                uint32_t plen = (mlen >= NV_GSP_RPC_HDR_SIZE) ? mlen - NV_GSP_RPC_HDR_SIZE : 0u;
                /* Защита от заворачивания элемента за конец кольца (в этом проходе
                   ответы ≤ 1 страницы и rptr мал → не срабатывает). */
                if (ch->msgq_rptr + mec > ch->lay.msg_count) return NV_GSP_RM_ERR_BOUNDS;
                if (reply_out && reply_cap) {
                    uint32_t n = plen < reply_cap ? plen : reply_cap;
                    const uint8_t *p = e + NV_GSP_RPC_PAYLOAD_OFF;
                    for (uint32_t i = 0; i < n; i++) reply_out[i] = p[i];
                }
                if (reply_len)  *reply_len  = plen;
                if (rpc_result) *rpc_result = mres;
                ch->msgq_rptr += mec;
                while (ch->msgq_rptr >= ch->lay.msg_count) ch->msgq_rptr -= ch->lay.msg_count;
                msgq_set_readptr(ch, ch->msgq_rptr);
                return NV_GSP_RM_OK;
            }

            /* Не наш ответ — async-событие GSP. Пропускаем (consume), продолжаем. */
            ch->msgq_rptr += mec;
            while (ch->msgq_rptr >= ch->lay.msg_count) ch->msgq_rptr -= ch->lay.msg_count;
            msgq_set_readptr(ch, ch->msgq_rptr);
        }
        if (waited >= timeout_us) return NV_GSP_RM_ERR_TIMEOUT;
        if (ch->udelay) ch->udelay(ch->io_ctx, step);
        waited += step;
        if ((waited % 200000u) == 0u && ch->ring) ch->ring(ch->io_ctx); /* периодич. звонок */
    }
}

int nv_gsp_get_static_info(nv_gsp_rpc_chan *ch, nv_gsp_static_info *out)
{
    if (!ch || !out) return NV_GSP_RM_ERR_ARG;

    static uint8_t req[NV_GSP_SCI_SIZE];
    static uint8_t rep[NV_GSP_SCI_SIZE];
    for (unsigned i = 0; i < NV_GSP_SCI_SIZE; i++) req[i] = 0;

    uint32_t rlen = 0, rres = 0xffffffffu;
    int rc = nv_gsp_rpc_call(ch, NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO,
                             req, NV_GSP_SCI_SIZE, rep, NV_GSP_SCI_SIZE,
                             &rlen, &rres, 4000000u);
    if (rc != NV_GSP_RM_OK) return rc;
    if (rres != 0) return NV_GSP_RM_ERR_BOUNDS;          /* GSP вернул ошибку RPC */
    if (rlen < NV_GSP_SCI_SIZE) return NV_GSP_RM_ERR_BOUNDS;

    for (unsigned i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

    const uint8_t *fb = rep + NV_GSP_SCI_FBREGION_OFF;
    uint32_t n = ld32(fb + NV_GSP_FBREGION_NUM_OFF);
    if (n == 0 || n > NV_GSP_FBREGION_MAX) return NV_GSP_RM_ERR_BOUNDS;  /* самопроверка */
    out->num_regions = n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *rg = fb + NV_GSP_FBREGION_ARR_OFF + (size_t)i * NV_GSP_FBREGION_STRIDE;
        out->regions[i].base       = ld64(rg + 0);
        out->regions[i].limit      = ld64(rg + 8);
        out->regions[i].reserved   = ld64(rg + 16);
        out->regions[i].performance= ld32(rg + 24);
        out->regions[i].compressed = rg[28];
        out->regions[i].iso        = rg[29];
        out->regions[i].prot       = rg[30];
    }
    out->bar1_pde     = ld64(rep + NV_GSP_SCI_BAR1PDE_OFF);
    out->bar2_pde     = ld64(rep + NV_GSP_SCI_BAR2PDE_OFF);
    out->h_client     = ld32(rep + NV_GSP_SCI_HINTCLIENT_OFF);
    out->h_device     = ld32(rep + NV_GSP_SCI_HINTDEVICE_OFF);
    out->h_subdevice  = ld32(rep + NV_GSP_SCI_HINTSUBDEV_OFF);
    return NV_GSP_RM_OK;
}

int nv_gsp_rm_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hParent,
                    uint32_t hObject, uint32_t hClass,
                    uint8_t *params, uint32_t params_len, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;

    /* req = rpc_gsp_rm_alloc_v03_00 (шапка 32б) + params. */
    static uint8_t req[NV_RM_ALLOC_HDR_SIZE + 256u];
    static uint8_t rep[NV_RM_ALLOC_HDR_SIZE + 256u];
    if (params_len > 256u) return NV_GSP_RM_ERR_BOUNDS;
    uint32_t req_len = NV_RM_ALLOC_HDR_SIZE + params_len;
    for (uint32_t i = 0; i < req_len; i++) req[i] = 0;
    st32(req + NV_RM_ALLOC_HCLIENT_OFF,   hClient);
    st32(req + NV_RM_ALLOC_HPARENT_OFF,   hParent);
    st32(req + NV_RM_ALLOC_HOBJECT_OFF,   hObject);
    st32(req + NV_RM_ALLOC_HCLASS_OFF,    hClass);
    st32(req + NV_RM_ALLOC_STATUS_OFF,    0u);
    st32(req + NV_RM_ALLOC_PARAMSIZE_OFF, params_len);
    st32(req + NV_RM_ALLOC_FLAGS_OFF,     0u);
    if (params && params_len)
        for (uint32_t i = 0; i < params_len; i++) req[NV_RM_ALLOC_HDR_SIZE + i] = params[i];

    uint32_t rlen = 0, rres = 0xffffffffu;
    int rc = nv_gsp_rpc_call(ch, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC,
                             req, req_len, rep, req_len, &rlen, &rres, 4000000u);
    if (rc != NV_GSP_RM_OK) return rc;
    if (rres != 0) return NV_GSP_RM_ERR_BOUNDS;          /* транспорт RPC дал ошибку */
    if (rlen < NV_RM_ALLOC_HDR_SIZE) return NV_GSP_RM_ERR_BOUNDS;
    if (status) *status = ld32(rep + NV_RM_ALLOC_STATUS_OFF);  /* NV_OK==0 */
    /* params — IN/OUT: вернуть OUT-поля ответа (offset/size и т.п.) в буфер вызывающего. */
    if (params && params_len) {
        uint32_t avail = rlen - NV_RM_ALLOC_HDR_SIZE;
        uint32_t n = params_len < avail ? params_len : avail;
        for (uint32_t i = 0; i < n; i++) params[i] = rep[NV_RM_ALLOC_HDR_SIZE + i];
    }
    return NV_GSP_RM_OK;
}

int nv_gsp_rm_client_ctor(nv_gsp_rpc_chan *ch, uint32_t *out_client, uint32_t *status)
{
    uint32_t h = NV_GSP_RM_CLIENT_HANDLE | 0u;
    /* NV0000_ALLOC_PARAMETERS (108б): hClient@0, processID@4=~0, processName[100]@8. */
    uint8_t p[108];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + 0, h);
    st32(p + 4, 0xffffffffu);                            /* processID = ~0 */
    int rc = nv_gsp_rm_alloc(ch, h, h, h, NV01_ROOT, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_client) *out_client = h;
    return rc;
}

int nv_gsp_rm_device_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient,
                          uint32_t *out_device, uint32_t *status)
{
    uint32_t h = NV_GSP_RM_DEVICE_HANDLE;
    /* NV0080_ALLOC_PARAMETERS (56б): deviceId@0, hClientShare@4, …, vaSpaceSize@24… */
    uint8_t p[56];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + 4, hClient);                                /* hClientShare = client */
    int rc = nv_gsp_rm_alloc(ch, hClient, hClient, h, NV01_DEVICE_0, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_device) *out_device = h;
    return rc;
}

int nv_gsp_rm_subdevice_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                             uint32_t *out_subdev, uint32_t *status)
{
    uint32_t h = NV_GSP_RM_SUBDEV_HANDLE;
    /* NV2080_ALLOC_PARAMETERS (4б): subDeviceId@0 = 0. */
    uint8_t p[4] = {0,0,0,0};
    int rc = nv_gsp_rm_alloc(ch, hClient, hDevice, h, NV20_SUBDEVICE_0, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_subdev) *out_subdev = h;
    return rc;
}

/* ===================== Проход B: RM_CONTROL + VA-пространство ===================== */

int nv_gsp_rm_control(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hObject, uint32_t cmd,
                      uint8_t *params, uint32_t params_len, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;

    /* req = rpc_gsp_rm_control_v03_00 (шапка 24б) + params. */
    static uint8_t req[NV_RM_CTRL_HDR_SIZE + 512u];
    static uint8_t rep[NV_RM_CTRL_HDR_SIZE + 512u];
    if (params_len > 512u) return NV_GSP_RM_ERR_BOUNDS;
    uint32_t req_len = NV_RM_CTRL_HDR_SIZE + params_len;
    for (uint32_t i = 0; i < req_len; i++) req[i] = 0;
    st32(req + NV_RM_CTRL_HCLIENT_OFF,   hClient);
    st32(req + NV_RM_CTRL_HOBJECT_OFF,   hObject);
    st32(req + NV_RM_CTRL_CMD_OFF,       cmd);
    st32(req + NV_RM_CTRL_STATUS_OFF,    0u);
    st32(req + NV_RM_CTRL_PARAMSIZE_OFF, params_len);
    st32(req + NV_RM_CTRL_FLAGS_OFF,     0u);
    if (params && params_len)
        for (uint32_t i = 0; i < params_len; i++) req[NV_RM_CTRL_HDR_SIZE + i] = params[i];

    uint32_t rlen = 0, rres = 0xffffffffu;
    int rc = nv_gsp_rpc_call(ch, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL,
                             req, req_len, rep, req_len, &rlen, &rres, 4000000u);
    if (rc != NV_GSP_RM_OK) return rc;
    if (rres != 0) return NV_GSP_RM_ERR_BOUNDS;
    if (rlen < NV_RM_CTRL_HDR_SIZE) return NV_GSP_RM_ERR_BOUNDS;
    if (status) *status = ld32(rep + NV_RM_CTRL_STATUS_OFF);
    /* Ответные params (IN/OUT) — обратно в буфер вызывающего. */
    if (params && params_len) {
        uint32_t avail = rlen - NV_RM_CTRL_HDR_SIZE;
        uint32_t n = params_len < avail ? params_len : avail;
        for (uint32_t i = 0; i < n; i++) params[i] = rep[NV_RM_CTRL_HDR_SIZE + i];
    }
    return NV_GSP_RM_OK;
}

int nv_gsp_fb_get_info(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hSubdevice,
                       const uint32_t *indices, uint32_t *out_data, uint32_t n,
                       uint32_t *status)
{
    if (!ch || !indices || !out_data) return NV_GSP_RM_ERR_ARG;
    if (n == 0 || n > NV2080_CTRL_FB_INFO_MAX_LIST) return NV_GSP_RM_ERR_ARG;

    /* NV2080_CTRL_FB_GET_INFO_V2_PARAMS: fbInfoListSize@0 + fbInfoList[54]@4 ({index,data}). */
    static uint8_t p[NV2080_CTRL_FB_GET_INFO_V2_SIZE];
    for (unsigned i = 0; i < NV2080_CTRL_FB_GET_INFO_V2_SIZE; i++) p[i] = 0;
    st32(p + 0, n);                                       /* fbInfoListSize */
    for (uint32_t i = 0; i < n; i++)
        st32(p + 4 + i * 8, indices[i]);                  /* fbInfoList[i].index (data=0) */

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hSubdevice, NV2080_CTRL_CMD_FB_GET_INFO_V2,
                               p, NV2080_CTRL_FB_GET_INFO_V2_SIZE, &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    for (uint32_t i = 0; i < n; i++)
        out_data[i] = ld32(p + 4 + i * 8 + 4);            /* fbInfoList[i].data */
    return NV_GSP_RM_OK;
}

int nv_gsp_rm_vaspace_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                           uint32_t *out_vaspace, uint32_t *status)
{
    uint32_t h = NV_GSP_RM_VASPACE_HANDLE;
    /* NV_VASPACE_ALLOCATION_PARAMETERS (48б): index=GPU_NEW, прочее 0 (дефолт). */
    uint8_t p[NV_VASPACE_ALLOC_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + 0, NV_VASPACE_ALLOCATION_INDEX_GPU_NEW);     /* index */
    int rc = nv_gsp_rm_alloc(ch, hClient, hDevice, h, FERMI_VASPACE_A, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_vaspace) *out_vaspace = h;
    return rc;
}

int nv_gsp_rm_vram_memlist(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                           uint64_t phys, uint64_t size,
                           uint32_t *out_handle, uint32_t *rpc_result)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    if (size == 0 || (size & 0xfffu) || (phys & 0xfffu)) return NV_GSP_RM_ERR_ARG;
    uint32_t pages = (uint32_t)(size >> NV_GSP_PAGE_SHIFT);
    uint32_t req_len = NV_ALLOCMEM_PTEARR_OFF + pages * 8u;

    static uint8_t req[8192];                       /* до ~500 страниц (≈2 МиБ) в один элемент */
    if (req_len > sizeof(req)) return NV_GSP_RM_ERR_BOUNDS;
    for (uint32_t i = 0; i < req_len; i++) req[i] = 0;

    uint32_t h = NV_GSP_RM_VRAM_HANDLE;
    st32(req + NV_ALLOCMEM_HCLIENT_OFF,   hClient);
    st32(req + NV_ALLOCMEM_HDEVICE_OFF,   hDevice);
    st32(req + NV_ALLOCMEM_HMEMORY_OFF,   h);
    st32(req + NV_ALLOCMEM_HCLASS_OFF,    NV01_MEMORY_LIST_FBMEM);
    st32(req + NV_ALLOCMEM_FLAGS_OFF,     NVOS02_FLAGS_FBMEM_CONTIG_NOMAP);
    st32(req + NV_ALLOCMEM_PTEADJUST_OFF, 0u);
    st32(req + NV_ALLOCMEM_FORMAT_OFF,    NV_MMU_PTE_KIND_GENERIC_MEMORY);
    st64(req + NV_ALLOCMEM_LENGTH_OFF,    size);
    st32(req + NV_ALLOCMEM_PAGECOUNT_OFF, pages);
    /* pte_desc: idr=0, reserved1=0, length=pages → в старших 16 битах u32. */
    st32(req + NV_ALLOCMEM_PTEDESC_OFF,   (uint32_t)(pages & 0xffffu) << 16);
    /* pte_pde[i] = (phys>>12) + i (contiguous), u64 каждый. */
    uint64_t pfn = phys >> NV_GSP_PAGE_SHIFT;
    for (uint32_t i = 0; i < pages; i++)
        st64(req + NV_ALLOCMEM_PTEARR_OFF + (size_t)i * 8u, pfn + i);

    uint32_t rlen = 0, rres = 0xffffffffu;
    int rc = nv_gsp_rpc_call(ch, NV_VGPU_MSG_FUNCTION_ALLOC_MEMORY,
                             req, req_len, NULL, 0, &rlen, &rres, 4000000u);
    if (rc != NV_GSP_RM_OK) return rc;
    if (rpc_result) *rpc_result = rres;
    if (out_handle) *out_handle = h;
    return (rres == 0) ? NV_GSP_RM_OK : NV_GSP_RM_ERR_BOUNDS;
}

int nv_gsp_rm_vmem_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                        uint32_t hVASpace, uint32_t *out_vmem, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint32_t h = NV_GSP_RM_VMEM_HANDLE;
    /* NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS (24б): offset=0, limit=0 (всё пространство),
       hVASpace = наш FERMI_VASPACE_A. */
    uint8_t p[NV_VMEM_ALLOC_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st64(p + NV_VMEM_OFFSET_OFF,   0u);
    st64(p + NV_VMEM_LIMIT_OFF,    0u);
    st32(p + NV_VMEM_HVASPACE_OFF, hVASpace);
    /* Родитель VirtualMemory — device (как и у vaspace). */
    int rc = nv_gsp_rm_alloc(ch, hClient, hDevice, h, NV01_MEMORY_VIRTUAL,
                             p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_vmem) *out_vmem = h;
    return rc;
}

int nv_gsp_rm_map_memory_dma(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                             uint32_t hDma, uint32_t hMemory,
                             uint64_t offset, uint64_t length, uint32_t flags,
                             uint64_t *dma_offset, uint32_t *status,
                             uint32_t *rpc_result)
{
    if (!ch || !dma_offset || length == 0) return NV_GSP_RM_ERR_ARG;
    if (offset + length < offset) return NV_GSP_RM_ERR_ARG;

    /* rpc_map_memory_dma_v03_00 содержит только NVOS46_PARAMETERS_v03_00. */
    uint8_t req[NV_MAP_MEMORY_DMA_PARAMS_SIZE];
    uint8_t rep[NV_MAP_MEMORY_DMA_PARAMS_SIZE];
    for (uint32_t i = 0; i < sizeof(req); i++) req[i] = 0;
    st32(req + NV_MAP_MEMORY_DMA_HCLIENT_OFF, hClient);
    st32(req + NV_MAP_MEMORY_DMA_HDEVICE_OFF, hDevice);
    st32(req + NV_MAP_MEMORY_DMA_HDMA_OFF,    hDma);
    st32(req + NV_MAP_MEMORY_DMA_HMEMORY_OFF, hMemory);
    st64(req + NV_MAP_MEMORY_DMA_OFFSET_OFF,  offset);
    st64(req + NV_MAP_MEMORY_DMA_LENGTH_OFF,  length);
    st32(req + NV_MAP_MEMORY_DMA_FLAGS_OFF,   flags);
    st64(req + NV_MAP_MEMORY_DMA_DMAOFFSET_OFF, *dma_offset);

    uint32_t rlen = 0, rres = 0xffffffffu;
    int rc = nv_gsp_rpc_call(ch, NV_VGPU_MSG_FUNCTION_MAP_MEMORY_DMA,
                             req, sizeof(req), rep, sizeof(rep),
                             &rlen, &rres, 4000000u);
    if (rpc_result) *rpc_result = rres;
    if (rc != NV_GSP_RM_OK) return rc;
    if (rres != 0) return NV_GSP_RM_ERR_BOUNDS;
    if (rlen < NV_MAP_MEMORY_DMA_PARAMS_SIZE) return NV_GSP_RM_ERR_BOUNDS;

    *dma_offset = ld64(rep + NV_MAP_MEMORY_DMA_DMAOFFSET_OFF);
    if (status) *status = ld32(rep + NV_MAP_MEMORY_DMA_STATUS_OFF);
    return NV_GSP_RM_OK;
}
