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
