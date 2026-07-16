/*
 * gsp_disp.c — слой 5: дисплей через GSP-RM (см. gsp_disp.h).
 * Порт nouveau r535_disp_oneinit. Аллокации/контролы — через gsp_rm.{c,h}.
 */
#include "gsp_disp.h"

static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

int nv_gsp_disp_common_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                             uint32_t *out_disp, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint32_t h = NV_GSP_RM_DISPCOMMON_HANDLE;
    /* NV04_DISPLAY_COMMON без alloc-params (как nouveau: paramsSize 0). */
    int rc = nv_gsp_rm_alloc(ch, hClient, hDevice, h, NV04_DISPLAY_COMMON, NULL, 0, status);
    if (rc == NV_GSP_RM_OK && out_disp) *out_disp = h;
    return rc;
}

int nv_gsp_disp_get_num_heads(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                              uint32_t *out_num_heads, uint32_t *status)
{
    if (!ch || !out_num_heads) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV0073_NUM_HEADS_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV0073_NUM_HEADS_SUBDEV_OFF, 0u);   /* subDeviceInstance=0 */
    st32(p + NV0073_NUM_HEADS_FLAGS_OFF,  0u);   /* flags=0 → всего heads */

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hDispCommon, NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS,
                               p, sizeof(p), &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    *out_num_heads = ld32(p + NV0073_NUM_HEADS_NUMHEADS_OFF);
    return NV_GSP_RM_OK;
}

int nv_gsp_disp_get_supported(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                              uint32_t *out_display_mask, uint32_t *out_display_mask_ddc,
                              uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV0073_SUPPORTED_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV0073_SUPPORTED_SUBDEV_OFF, 0u);

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hDispCommon, NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED,
                               p, sizeof(p), &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    if (out_display_mask)     *out_display_mask     = ld32(p + NV0073_SUPPORTED_MASK_OFF);
    if (out_display_mask_ddc) *out_display_mask_ddc = ld32(p + NV0073_SUPPORTED_MASKDDC_OFF);
    return NV_GSP_RM_OK;
}

int nv_gsp_disp_or_get_info(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                            uint32_t displayId, uint32_t *out_type, uint32_t *out_protocol,
                            uint32_t *out_index, uint32_t *out_location, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV0073_OR_GET_INFO_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV0073_OR_GI_DISPLAYID_OFF, displayId);   /* один displayId (не маска) */

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hDispCommon, NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO,
                               p, sizeof(p), &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    if (out_type)     *out_type     = ld32(p + NV0073_OR_GI_TYPE_OFF);
    if (out_protocol) *out_protocol = ld32(p + NV0073_OR_GI_PROTOCOL_OFF);
    if (out_index)    *out_index    = ld32(p + NV0073_OR_GI_INDEX_OFF);
    if (out_location) *out_location = ld32(p + NV0073_OR_GI_LOCATION_OFF);
    return NV_GSP_RM_OK;
}

int nv_gsp_disp_get_connect_state(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                                  uint32_t display_mask_in, uint32_t *out_connected_mask,
                                  uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV0073_CONNECT_STATE_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV0073_CONNECT_STATE_FLAGS_OFF, 0u);              /* METHOD_DEFAULT */
    st32(p + NV0073_CONNECT_STATE_MASK_OFF,  display_mask_in); /* IN: что проверить */

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hDispCommon, NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE,
                               p, sizeof(p), &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    if (out_connected_mask) *out_connected_mask = ld32(p + NV0073_CONNECT_STATE_MASK_OFF); /* OUT */
    return NV_GSP_RM_OK;
}

int nv_gsp_disp_get_edid(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                         uint32_t displayId, uint8_t *buf, uint32_t buf_cap,
                         uint32_t *out_size, uint32_t *status)
{
    if (!ch || !buf) return NV_GSP_RM_ERR_ARG;
    static uint8_t p[NV0073_EDID_V2_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV0073_EDID_DISPLAYID_OFF, displayId);
    st32(p + NV0073_EDID_FLAGS_OFF,     0u);   /* COPY_CACHE_NO + READ_MODE_COOKED → DDC-чтение */

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hDispCommon, NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2,
                               p, sizeof(p), &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    uint32_t sz = ld32(p + NV0073_EDID_BUFSIZE_OFF);
    if (sz > NV0073_EDID_MAX_BYTES) sz = NV0073_EDID_MAX_BYTES;
    uint32_t n = sz < buf_cap ? sz : buf_cap;
    for (uint32_t i = 0; i < n; i++) buf[i] = p[NV0073_EDID_BUFFER_OFF + i];
    if (out_size) *out_size = sz;
    return NV_GSP_RM_OK;
}

static void st64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

int nv_gsp_disp_write_inst_mem(nv_gsp_rpc_chan *ch, uint32_t hIntClient, uint32_t hIntSubdevice,
                               uint64_t inst_phys, uint64_t inst_size, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV_DISP_WRINST_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st64(p + NV_DISP_WRINST_PHYS_OFF,      inst_phys);
    st64(p + NV_DISP_WRINST_SIZE_OFF,      inst_size);
    st32(p + NV_DISP_WRINST_ADDRSPACE_OFF, NV_RM_ADDR_FBMEM);
    st32(p + NV_DISP_WRINST_CACHEATTR_OFF, NV_MEMORY_WRITECOMBINED);
    /* Контрол на внутреннем subdevice GSP (privileged internal client). */
    return nv_gsp_rm_control(ch, hIntClient, hIntSubdevice,
                             NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM,
                             p, sizeof(p), status);
}

int nv_gsp_disp_root_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                           uint32_t dispClass, uint32_t *out_root, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint32_t h = dispClass << 16;   /* напр. AD102_DISP(0xc770)<<16 = 0xc7700000 */
    int rc = nv_gsp_rm_alloc(ch, hClient, hDevice, h, dispClass, NULL, 0, status);
    if (rc == NV_GSP_RM_OK && out_root) *out_root = h;
    return rc;
}

int nv_gsp_disp_channel_pushbuffer(nv_gsp_rpc_chan *ch, uint32_t hIntClient,
                                   uint32_t hIntSubdevice, uint32_t hclass,
                                   uint32_t channelInstance, uint64_t pb_phys,
                                   uint64_t pb_limit, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV_DISP_PB_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV_DISP_PB_ADDRSPACE_OFF, NV_RM_ADDR_FBMEM);   /* пушбуфер во VRAM */
    st64(p + NV_DISP_PB_PHYS_OFF,      pb_phys);
    st64(p + NV_DISP_PB_LIMIT_OFF,     pb_limit);
    st32(p + NV_DISP_PB_CACHESNOOP_OFF, 0u);
    st32(p + NV_DISP_PB_HCLASS_OFF,    hclass);
    st32(p + NV_DISP_PB_CHANINST_OFF,  channelInstance);
    p[NV_DISP_PB_VALID_OFF] = 1u;                           /* valid=1 (не PIO) */
    return nv_gsp_rm_control(ch, hIntClient, hIntSubdevice,
                             NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER,
                             p, sizeof(p), status);
}

int nv_gsp_disp_core_channel_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispRoot,
                                   uint32_t coreClass, uint32_t channelInstance,
                                   uint32_t *out_chan, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint32_t h = (coreClass << 16) | channelInstance;
    uint8_t p[NV50VAIO_CHANDMA_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV50VAIO_CHANDMA_CHANINST_OFF, channelInstance);
    st32(p + NV50VAIO_CHANDMA_OFFSET_OFF,   0u);   /* offset put/get = 0 */
    int rc = nv_gsp_rm_alloc(ch, hClient, hDispRoot, h, coreClass, p, sizeof(p), status);
    if (rc == NV_GSP_RM_OK && out_chan) *out_chan = h;
    return rc;
}

int nv_gsp_disp_assign_sor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                           uint32_t displayId, uint32_t *out_sor, uint32_t *status)
{
    if (!ch) return NV_GSP_RM_ERR_ARG;
    uint8_t p[NV0073_ASSIGN_SOR_PARAMS_SIZE];
    for (unsigned i = 0; i < sizeof(p); i++) p[i] = 0;
    st32(p + NV0073_ASSIGN_SOR_DISPLAYID_OFF, displayId);   /* назначить SOR этому displayId */
    /* sorExcludeMask=0 (любой SOR), flags=0 (AUDIO_DEFAULT). */

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_control(ch, hClient, hDispCommon, NV0073_CTRL_CMD_DFP_ASSIGN_SOR,
                               p, sizeof(p), &st);
    if (status) *status = st;
    if (rc != NV_GSP_RM_OK) return rc;
    /* Найти SOR, чей displayMask содержит наш displayId (как r535_outp_acquire). */
    uint32_t sor = ~0u;
    for (uint32_t i = 0; i < NV0073_ASSIGN_SOR_MAX_SORS; i++) {
        uint32_t dm = ld32(p + NV0073_ASSIGN_SOR_LISTWITHTAG_OFF
                             + i * NV0073_ASSIGN_SOR_INFO_STRIDE);   /* displayMask@0 записи */
        if (dm & displayId) { sor = i; break; }
    }
    if (out_sor) *out_sor = sor;
    return NV_GSP_RM_OK;
}

void nv_gsp_disp_push_method(uint8_t *pb, uint32_t *poff, uint32_t method_addr, uint32_t data)
{
    if (!pb || !poff) return;
    uint32_t o = *poff;
    st32(pb + o,     nv_disp_method_hdr(method_addr, 1u));   /* DMA-заголовок: 1 слово данных */
    st32(pb + o + 4, data);                                  /* данные метода */
    *poff = o + 8;
}

int nv_edid_parse_dtd(const uint8_t *edid, uint32_t edid_len, nv_edid_timing *t)
{
    if (!edid || !t || edid_len < 128) return -1;
    const uint8_t *d = edid + 54;                 /* первый DTD */
    uint32_t pclk = (uint32_t)d[0] | ((uint32_t)d[1] << 8);
    if (pclk == 0) return -1;                     /* это не DTD (display descriptor) */
    t->pclk_khz  = pclk * 10u;                    /* поле в единицах 10 кГц */
    t->hact      = d[2] | ((uint32_t)(d[4] & 0xf0) << 4);
    t->hblank    = d[3] | ((uint32_t)(d[4] & 0x0f) << 8);
    t->vact      = d[5] | ((uint32_t)(d[7] & 0xf0) << 4);
    t->vblank    = d[6] | ((uint32_t)(d[7] & 0x0f) << 8);
    t->hsync_off = d[8]  | ((uint32_t)(d[11] & 0xc0) << 2);
    t->hsync_w   = d[9]  | ((uint32_t)(d[11] & 0x30) << 4);
    t->vsync_off = (d[10] >> 4) | ((uint32_t)(d[11] & 0x0c) << 2);
    t->vsync_w   = (d[10] & 0x0f) | ((uint32_t)(d[11] & 0x03) << 4);
    /* Полярность sync — из features bitmap d[17] (для digital sync биты 1..2). */
    t->hsync_pos = (d[17] & 0x02) ? 1 : 0;
    t->vsync_pos = (d[17] & 0x04) ? 1 : 0;
    return 0;
}

void nv_gsp_disp_build_core_modeset(uint8_t *pb, uint32_t *off, const nv_edid_timing *t,
                                    uint32_t head, uint32_t sor, uint32_t protocol)
{
    if (!pb || !off || !t) return;
    uint32_t htotal = t->hact + t->hblank;
    uint32_t vtotal = t->vact + t->vblank;
    /* Координаты raster (nv50_head_atomic_check_mode): active=total; sync_end=sync_w-1;
       blank_end = hblank - hsync_off - 1; blank_start = blank_end + active. */
    uint32_t hbe = t->hblank - t->hsync_off - 1u;
    uint32_t vbe = t->vblank - t->vsync_off - 1u;
    uint32_t hbs = hbe + t->hact;
    uint32_t vbs = vbe + t->vact;
    uint32_t hertz = t->pclk_khz * 1000u;   /* HERTZ[30:0] */

    (void)sor; (void)protocol;   /* SOR привязывается отдельной фазой (build_core_sor) */
    /* ПОРЯДОК как nv50_head_flush_set: view → mode → procamp → OR. OUTPUT_RESOURCE
       валидируется против режима головы, поэтому raster/viewport ДО него (иначе INVALID_ARG). */

    /* --- view: viewport = активная область (headc37d_view). --- */
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_VIEWPORT_POINT_IN(head), 0u);
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_VIEWPORT_SIZE_IN(head),  t->hact | (t->vact << 16));
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_VIEWPORT_SIZE_OUT(head), t->hact | (t->vact << 16));

    /* --- mode: raster + pixel clock + usage bounds (headc37d_mode). --- */
    nv_gsp_disp_push_method(pb, off, NVC77D_HEAD_SET_RASTER_SIZE(head), htotal | (vtotal << 16));
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_RASTER_SYNC_END(head),
                            (t->hsync_w - 1u) | ((t->vsync_w - 1u) << 16));
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_RASTER_BLANK_END(head),  hbe | (vbe << 16));
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_RASTER_BLANK_START(head), hbs | (vbs << 16));
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_RASTER_VERT_BLANK2(head), 0u | (0u << 16) | 1u);
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_CONTROL_METH(head), NVC37D_HEAD_CONTROL_PROGRESSIVE);
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(head),     hertz & 0x7fffffffu);
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(head), hertz & 0x7fffffffu);
    /* HEAD_USAGE_BOUNDS убран: отвергается INVALID_ARG при любом значении (прогоны #6/#7),
       nouveau сам помечает его "doesn't belong here". Требует output-LUT/cursor ctx-dma. */

    /* --- procamp: RGB (headc37d_procamp: BLACK_LEVEL GRAPHICS[31:30]=2). --- */
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_PROCAMP(head), (2u << 30));

    /* --- or: output resource — полярности sync (NEGATIVE_TRUE=1 если не positive) + 24bpp. ПОСЛЕДНИМ. --- */
    uint32_t ores = (NVC37D_ORESOURCE_PIXEL_DEPTH_BPP_24_444 << 4)
                  | (t->hsync_pos ? 0u : (1u << 2))
                  | (t->vsync_pos ? 0u : (1u << 3));
    nv_gsp_disp_push_method(pb, off, NVC37D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(head), ores);
}

void nv_gsp_disp_build_core_sor(uint8_t *pb, uint32_t *off, uint32_t sor,
                                uint32_t head, uint32_t protocol)
{
    if (!pb || !off) return;
    /* SOR_SET_CONTROL: OWNER_MASK[7:0]=1<<head, PROTOCOL[11:8]. Должно быть защёлкнуто
       (UPDATE) ДО head OUTPUT_RESOURCE, иначе OUTPUT_RESOURCE→INVALID_ARG (голова ещё
       не владеет OR). Порт sorc37d. */
    nv_gsp_disp_push_method(pb, off, NVC37D_SOR_SET_CONTROL(sor),
                            (1u << head) | (protocol << 8));
}

void nv_gsp_disp_build_core_init(uint8_t *pb, uint32_t *off, uint32_t notifier_handle)
{
    if (!pb || !off) return;
    /* SET_CONTEXT_DMA_NOTIFIER = handle sync-ctxdma. */
    nv_gsp_disp_push_method(pb, off, NVC37D_SET_CONTEXT_DMA_NOTIFIER, notifier_handle);
    /* Для всех 8 окон: разрешённые форматы/границы (corec37d_init). */
    for (uint32_t i = 0; i < 8u; i++) {
        nv_gsp_disp_push_method(pb, off, NVC37D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(i),
                                NVC37D_WIN_FORMAT_USAGE_DEFAULT);
        nv_gsp_disp_push_method(pb, off, NVC37D_WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(i), 0u);
        nv_gsp_disp_push_method(pb, off, NVC37D_WINDOW_SET_WINDOW_USAGE_BOUNDS(i),
                                NVC37D_WIN_USAGE_BOUNDS_DEFAULT);
    }
    /* Владелец окна i → head(i>>1) (corec37d_wndw_owner). */
    for (uint32_t i = 0; i < 8u; i++)
        nv_gsp_disp_push_method(pb, off, NVC37D_WINDOW_SET_CONTROL_OWNER(i), (i >> 1));
}

void nv_gsp_disp_build_core_update(uint8_t *pb, uint32_t *off, uint32_t wndw_interlock_mask)
{
    if (!pb || !off) return;
    nv_gsp_disp_push_method(pb, off, NVC37D_SET_INTERLOCK_FLAGS, 0u);              /* без cursor */
    nv_gsp_disp_push_method(pb, off, NVC37D_SET_WINDOW_INTERLOCK_FLAGS, wndw_interlock_mask);
    nv_gsp_disp_push_method(pb, off, NVC37D_UPDATE, NVC37D_UPDATE_DATA);
}

void nv_gsp_disp_build_window_image(uint8_t *pb, uint32_t *off, uint32_t format,
                                    uint32_t w, uint32_t h, uint32_t pitch,
                                    uint64_t fb_off, uint32_t iso_handle)
{
    if (!pb || !off) return;
    /* SET_PRESENT_CONTROL: MIN_PRESENT_INTERVAL=0, BEGIN_MODE NON_TEARING=0. */
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_PRESENT_CONTROL, 0u);
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_SIZE, (w & 0xffffu) | ((h & 0xffffu) << 16));
    /* линейный (pitch) layout, block_height=0. */
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_STORAGE, NVC37E_STORAGE_MEMORY_LAYOUT_PITCH);
    /* FORMAT | COLOR_SPACE RGB(0) | INPUT_RANGE BYPASS(0). */
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_PARAMS, (format & 0xffu));
    /* PLANAR_STORAGE pitch в единицах 64б (>>6). */
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_PLANAR_STORAGE(0), (pitch >> 6) & 0x1fffu);
    /* ISO ctx-dma (framebuffer) + offset (>>8). */
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_CONTEXT_DMA_ISO(0), iso_handle);
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_OFFSET(0), (uint32_t)(fb_off >> 8));
    /* вход = вся поверхность. */
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_POINT_IN(0), 0u);
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_SIZE_IN,  (w & 0x7fffu) | ((h & 0x7fffu) << 16));
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_SIZE_OUT, (w & 0x7fffu) | ((h & 0x7fffu) << 16));
}

void nv_gsp_disp_build_window_update(uint8_t *pb, uint32_t *off, int interlock_with_core)
{
    if (!pb || !off) return;
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_INTERLOCK_FLAGS,
                            interlock_with_core ? NVC37E_INTERLOCK_WITH_CORE_BIT : 0u);
    nv_gsp_disp_push_method(pb, off, NVC37E_SET_WINDOW_INTERLOCK_FLAGS, 0u);
    nv_gsp_disp_push_method(pb, off, NVC37E_UPDATE, NVC37D_UPDATE_DATA);
}

void nv_gsp_disp_build_ctxdma_desc(uint8_t *desc, uint64_t start, uint64_t limit)
{
    if (!desc) return;
    for (unsigned i = 0; i < NV_CTXDMA_DESC_SIZE; i++) desc[i] = 0;
    uint64_t s = start >> 8, l = limit >> 8;
    st32(desc + 0x00, NV_CTXDMA_FLAGS0_VRAM_RW);
    st32(desc + 0x04, (uint32_t)(s & 0xffffffffu));
    st32(desc + 0x08, (uint32_t)(s >> 32));
    st32(desc + 0x0c, (uint32_t)(l & 0xffffffffu));
    st32(desc + 0x10, (uint32_t)(l >> 32));
}

/* Хэш RAMHT (nvkm_ramht_hash): XOR handle по bits, затем ^ chid<<(bits-4). */
static uint32_t nv_disp_ramht_hash(uint32_t chid, uint32_t handle)
{
    uint32_t hash = 0;
    uint32_t mask = (1u << NV_DISP_RAMHT_BITS) - 1u;
    while (handle) { hash ^= (handle & mask); handle >>= NV_DISP_RAMHT_BITS; }
    hash ^= chid << (NV_DISP_RAMHT_BITS - 4u);
    return hash % NV_DISP_RAMHT_SIZE;
}

void nv_gsp_disp_ramht_entry(uint32_t chid, uint32_t handle, uint32_t client,
                             uint32_t inst_offset, uint32_t *out_slot, uint32_t *out_context)
{
    /* context = chid<<25 | (client & 0x3fff) | (inst_offset << 9) (r535_dmac_bind + addr=-9). */
    if (out_slot)    *out_slot    = nv_disp_ramht_hash(chid, handle);
    if (out_context) *out_context = (chid << 25) | (client & 0x3fffu) | (inst_offset << 9);
}
