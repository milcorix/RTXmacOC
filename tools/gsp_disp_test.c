/*
 * gsp_disp_test.c — офлайн-тест слоя 5 (дисплей) без GPU.
 * Framing: NV04_DISPLAY_COMMON alloc + GET_NUM_HEADS + GET_SUPPORTED на синтетическом канале.
 *   make gsp-disp-test && ./tools/gsp_disp_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../driver/gsp/gsp_disp.h"

static int failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); failed = 1; } \
                              else printf("  ok: %s\n", msg); } while (0)

static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

static void noop_ring(void *c){ (void)c; }
static void noop_udelay(void *c, uint32_t us){ (void)c; (void)us; }
static uint8_t g_shm[1u << 20];

static void put_msg(uint8_t *shm, const nv_gsp_shm_layout_t *lay, uint32_t slot,
                    uint32_t fn, uint32_t rpc_result, const uint8_t *payload,
                    uint32_t plen, uint32_t ec)
{
    uint8_t *e = shm + lay->msgq_off + NV_GSP_QUEUE_ENTRYOFF + (size_t)slot * NV_GSP_QUEUE_MSGSIZE;
    memset(e, 0, (size_t)ec * 0x1000u);
    st32(e + NV_GSP_MSG_ELEMCOUNT_OFF, ec);
    uint8_t *r = e + NV_GSP_RPC_HDR_OFF;
    st32(r + 0,  NV_GSP_RPC_HEADER_VERSION);
    st32(r + 4,  NV_GSP_RPC_SIGNATURE);
    st32(r + NV_GSP_RPC_F_LENGTH,   NV_GSP_RPC_HDR_SIZE + plen);
    st32(r + NV_GSP_RPC_F_FUNCTION, fn);
    st32(r + 16, rpc_result);
    if (payload && plen) memcpy(e + NV_GSP_RPC_PAYLOAD_OFF, payload, plen);
}
static void set_msgq_wptr(uint8_t *shm, const nv_gsp_shm_layout_t *lay, uint32_t w)
{ st32(shm + lay->msgq_off + NV_MSGQ_TX_WRITEPTR_OFF, w); }

static void chan_init(nv_gsp_rpc_chan *ch)
{
    nv_gsp_shm_layout_t lay;
    int rc = nv_gsp_shm_init(g_shm, sizeof(g_shm), 0x10000000ull, &lay);
    if (rc != NV_GSP_RPC_OK) { printf("  FAIL: shm_init rc=%d\n", rc); failed = 1; }
    memset(ch, 0, sizeof(*ch));
    ch->shm = g_shm; ch->lay = lay;
    ch->io_ctx = NULL; ch->ring = noop_ring; ch->udelay = noop_udelay;
}

static void test_disp_common_alloc(void)
{
    printf("[test_disp_common_alloc]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_ALLOC_HDR_SIZE]; memset(rep, 0, sizeof(rep));  /* status=0 */
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t hdisp = 0, st = 0xffffffffu;
    int rc = nv_gsp_disp_common_alloc(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DEVICE_HANDLE,
                                      &hdisp, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "disp_common_alloc OK");
    CHECK(hdisp == NV_GSP_RM_DISPCOMMON_HANDLE, "хэндл == 0x00730000");
    const uint8_t *al = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(al + NV_RM_ALLOC_HCLASS_OFF) == NV04_DISPLAY_COMMON, "hClass == NV04_DISPLAY_COMMON");
    CHECK(ld32(al + NV_RM_ALLOC_HPARENT_OFF) == NV_GSP_RM_DEVICE_HANDLE, "hParent == device");
    CHECK(ld32(al + NV_RM_ALLOC_PARAMSIZE_OFF) == 0, "paramsSize == 0");
}

static void test_num_heads(void)
{
    printf("[test_num_heads]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV0073_NUM_HEADS_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    st32(rep + NV_RM_CTRL_HDR_SIZE + NV0073_NUM_HEADS_NUMHEADS_OFF, 4);   /* numHeads=4 */
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t nh = 0, st = 0xffffffffu;
    int rc = nv_gsp_disp_get_num_heads(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DISPCOMMON_HANDLE,
                                       &nh, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "get_num_heads OK");
    CHECK(nh == 4, "numHeads == 4");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS, "cmd == GET_NUM_HEADS");
    CHECK(ld32(cp + NV_RM_CTRL_HOBJECT_OFF) == NV_GSP_RM_DISPCOMMON_HANDLE, "hObject == disp-common");
    CHECK(ld32(cp + NV_RM_CTRL_PARAMSIZE_OFF) == NV0073_NUM_HEADS_PARAMS_SIZE, "paramsSize == 12");
}

static void test_supported(void)
{
    printf("[test_supported]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV0073_SUPPORTED_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    st32(rep + NV_RM_CTRL_HDR_SIZE + NV0073_SUPPORTED_MASK_OFF, 0x300);      /* displayMask */
    st32(rep + NV_RM_CTRL_HDR_SIZE + NV0073_SUPPORTED_MASKDDC_OFF, 0x100);   /* DDC subset */
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t mask = 0, ddc = 0, st = 0xffffffffu;
    int rc = nv_gsp_disp_get_supported(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DISPCOMMON_HANDLE,
                                       &mask, &ddc, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "get_supported OK");
    CHECK(mask == 0x300, "displayMask == 0x300");
    CHECK(ddc == 0x100, "displayMaskDDC == 0x100");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED, "cmd == GET_SUPPORTED");
}

static void test_or_get_info(void)
{
    printf("[test_or_get_info]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV0073_OR_GET_INFO_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    uint8_t *pp = rep + NV_RM_CTRL_HDR_SIZE;
    st32(pp + NV0073_OR_GI_TYPE_OFF, NV0073_OR_TYPE_SOR);
    st32(pp + NV0073_OR_GI_PROTOCOL_OFF, NV0073_OR_PROTOCOL_SOR_DP_A);
    st32(pp + NV0073_OR_GI_INDEX_OFF, 2);
    st32(pp + NV0073_OR_GI_LOCATION_OFF, 0);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t type=0, proto=0, idx=0, loc=9, st=0xffffffffu;
    int rc = nv_gsp_disp_or_get_info(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DISPCOMMON_HANDLE,
                                     0x100, &type, &proto, &idx, &loc, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "or_get_info OK");
    CHECK(type == NV0073_OR_TYPE_SOR, "type == SOR");
    CHECK(proto == NV0073_OR_PROTOCOL_SOR_DP_A, "protocol == DP_A");
    CHECK(idx == 2, "index == 2");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO, "cmd == OR_GET_INFO");
    CHECK(ld32(cp + NV_RM_CTRL_HDR_SIZE + NV0073_OR_GI_DISPLAYID_OFF) == 0x100, "displayId в запросе");
    CHECK(ld32(cp + NV_RM_CTRL_PARAMSIZE_OFF) == NV0073_OR_GET_INFO_PARAMS_SIZE, "paramsSize == 56");
}

static void test_connect_state(void)
{
    printf("[test_connect_state]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV0073_CONNECT_STATE_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    st32(rep + NV_RM_CTRL_HDR_SIZE + NV0073_CONNECT_STATE_MASK_OFF, 0x100);  /* подключён 0x100 */
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t conn = 0xffff, st = 0xffffffffu;
    int rc = nv_gsp_disp_get_connect_state(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DISPCOMMON_HANDLE,
                                           0x7f00, &conn, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "connect_state OK");
    CHECK(conn == 0x100, "connected mask == 0x100");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE, "cmd == CONNECT_STATE");
    CHECK(ld32(cp + NV_RM_CTRL_HDR_SIZE + NV0073_CONNECT_STATE_MASK_OFF) == 0x7f00, "IN displayMask == 0x7f00");
}

static void test_edid(void)
{
    printf("[test_edid]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    static uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV0073_EDID_V2_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    uint8_t *pp = rep + NV_RM_CTRL_HDR_SIZE;
    st32(pp + NV0073_EDID_BUFSIZE_OFF, 128);
    /* EDID magic 00 FF FF FF FF FF FF 00 в начале буфера */
    static const uint8_t magic[8] = {0x00,0xff,0xff,0xff,0xff,0xff,0xff,0x00};
    memcpy(pp + NV0073_EDID_BUFFER_OFF, magic, 8);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    static uint8_t edid[256]; uint32_t sz = 0, st = 0xffffffffu;
    int rc = nv_gsp_disp_get_edid(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DISPCOMMON_HANDLE,
                                  0x100, edid, sizeof(edid), &sz, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "get_edid OK");
    CHECK(sz == 128, "bufferSize == 128");
    CHECK(memcmp(edid, magic, 8) == 0, "EDID magic скопирован");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2, "cmd == GET_EDID_V2");
    CHECK(ld32(cp + NV_RM_CTRL_PARAMSIZE_OFF) == NV0073_EDID_V2_PARAMS_SIZE, "paramsSize == 2064");
}

static uint64_t ld64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

static void test_write_inst_mem(void)
{
    printf("[test_write_inst_mem]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV_DISP_WRINST_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_disp_write_inst_mem(&ch, 0xc2000005u, 0xabcd2080u, 0x13400000ull,
                                        NV_DISP_INST_SIZE, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "write_inst_mem OK");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM, "cmd == WRITE_INST_MEM");
    CHECK(ld32(cp + NV_RM_CTRL_HCLIENT_OFF) == 0xc2000005u, "hClient == internal");
    CHECK(ld32(cp + NV_RM_CTRL_HOBJECT_OFF) == 0xabcd2080u, "hObject == internal subdevice");
    const uint8_t *pp = cp + NV_RM_CTRL_HDR_SIZE;
    CHECK(ld64(pp + NV_DISP_WRINST_PHYS_OFF) == 0x13400000ull, "instMemPhysAddr");
    CHECK(ld64(pp + NV_DISP_WRINST_SIZE_OFF) == NV_DISP_INST_SIZE, "instMemSize == 64K");
    CHECK(ld32(pp + NV_DISP_WRINST_ADDRSPACE_OFF) == NV_RM_ADDR_FBMEM, "addrSpace == FBMEM");
    CHECK(ld32(pp + NV_DISP_WRINST_CACHEATTR_OFF) == NV_MEMORY_WRITECOMBINED, "cacheAttr == WC");
}

static void test_disp_root_alloc(void)
{
    printf("[test_disp_root_alloc]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_ALLOC_HDR_SIZE]; memset(rep, 0, sizeof(rep));
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t hroot = 0, st = 0xffffffffu;
    int rc = nv_gsp_disp_root_alloc(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DEVICE_HANDLE,
                                    AD102_DISP, &hroot, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "disp_root_alloc OK");
    CHECK(hroot == NV_GSP_RM_DISPROOT_HANDLE, "хэндл == 0xc7700000 (AD102_DISP<<16)");
    const uint8_t *al = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(al + NV_RM_ALLOC_HCLASS_OFF) == AD102_DISP, "hClass == AD102_DISP");
    CHECK(ld32(al + NV_RM_ALLOC_HOBJECT_OFF) == NV_GSP_RM_DISPROOT_HANDLE, "hObject == class<<16");
    CHECK(ld32(al + NV_RM_ALLOC_PARAMSIZE_OFF) == 0, "paramsSize == 0");
}

static void test_channel_pushbuffer(void)
{
    printf("[test_channel_pushbuffer]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV_DISP_PB_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_disp_channel_pushbuffer(&ch, 0xc2000005u, 0xabcd2080u,
                                            AD102_DISP_CORE_CHANNEL_DMA, 0,
                                            0x13410000ull, NV_DISP_PB_SIZE - 1, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "channel_pushbuffer OK");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, "cmd == CHANNEL_PUSHBUFFER");
    CHECK(ld32(cp + NV_RM_CTRL_HOBJECT_OFF) == 0xabcd2080u, "hObject == internal subdevice");
    const uint8_t *pp = cp + NV_RM_CTRL_HDR_SIZE;
    CHECK(ld32(pp + NV_DISP_PB_ADDRSPACE_OFF) == NV_RM_ADDR_FBMEM, "addressSpace == FBMEM");
    CHECK(ld64(pp + NV_DISP_PB_PHYS_OFF) == 0x13410000ull, "physicalAddr");
    CHECK(ld64(pp + NV_DISP_PB_LIMIT_OFF) == NV_DISP_PB_SIZE - 1, "limit == 0xfff");
    CHECK(ld32(pp + NV_DISP_PB_HCLASS_OFF) == AD102_DISP_CORE_CHANNEL_DMA, "hclass == CORE_CHANNEL");
    CHECK(pp[NV_DISP_PB_VALID_OFF] == 1, "valid == 1");
}

static void test_core_channel_alloc(void)
{
    printf("[test_core_channel_alloc]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_ALLOC_HDR_SIZE]; memset(rep, 0, sizeof(rep));
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t hcore = 0, st = 0xffffffffu;
    int rc = nv_gsp_disp_core_channel_alloc(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DISPROOT_HANDLE,
                                            AD102_DISP_CORE_CHANNEL_DMA, 0, &hcore, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "core_channel_alloc OK");
    CHECK(hcore == NV_GSP_RM_DISPCORE_HANDLE, "хэндл == 0xc77d0000");
    const uint8_t *al = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(al + NV_RM_ALLOC_HCLASS_OFF) == AD102_DISP_CORE_CHANNEL_DMA, "hClass == CORE_CHANNEL");
    CHECK(ld32(al + NV_RM_ALLOC_HPARENT_OFF) == NV_GSP_RM_DISPROOT_HANDLE, "hParent == display root");
    CHECK(ld32(al + NV_RM_ALLOC_PARAMSIZE_OFF) == NV50VAIO_CHANDMA_PARAMS_SIZE, "paramsSize == 32");
}

static void test_assign_sor(void)
{
    printf("[test_assign_sor]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV0073_ASSIGN_SOR_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    uint8_t *pp = rep + NV_RM_CTRL_HDR_SIZE;
    /* sorAssignListWithTag: [0]={0x200,SINGLE} [1]={0,NONE} [2]={0x100,SINGLE} [3]={0,NONE}
       → для displayId 0x100 ожидаем SOR 2. */
    st32(pp + NV0073_ASSIGN_SOR_LISTWITHTAG_OFF + 0*NV0073_ASSIGN_SOR_INFO_STRIDE, 0x200);
    st32(pp + NV0073_ASSIGN_SOR_LISTWITHTAG_OFF + 2*NV0073_ASSIGN_SOR_INFO_STRIDE, 0x100);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t sor = 0xffffffffu, st = 0xffffffffu;
    int rc = nv_gsp_disp_assign_sor(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DISPCOMMON_HANDLE,
                                    0x100, &sor, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "assign_sor OK");
    CHECK(sor == 2, "SOR для 0x100 == 2 (из sorAssignListWithTag)");
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV0073_CTRL_CMD_DFP_ASSIGN_SOR, "cmd == DFP_ASSIGN_SOR");
    CHECK(ld32(cp + NV_RM_CTRL_HDR_SIZE + NV0073_ASSIGN_SOR_DISPLAYID_OFF) == 0x100, "displayId == 0x100");
    CHECK(ld32(cp + NV_RM_CTRL_PARAMSIZE_OFF) == NV0073_ASSIGN_SOR_PARAMS_SIZE, "paramsSize == 80");
}

static void test_disp_push_method(void)
{
    printf("[test_disp_push_method]\n");
    uint8_t pb[64]; memset(pb, 0, sizeof(pb));
    uint32_t off = 0;
    /* HEAD_SET_RASTER_SIZE(0) = 0x2064, data = 1920 | (1080<<16) */
    uint32_t raster = 0x2064u;
    uint32_t data = 1920u | (1080u << 16);
    nv_gsp_disp_push_method(pb, &off, raster, data);
    nv_gsp_disp_push_method(pb, &off, NVC37D_UPDATE, 0u);
    CHECK(off == 16, "две записи → offset 16");
    /* заголовок[0]: count=1<<18 | (addr&0x3ffc) */
    uint32_t hdr0 = ld32(pb + 0);
    CHECK(((hdr0 >> 29) & 0x7) == NVC37D_DMA_OPCODE_METHOD, "opcode == METHOD");
    CHECK(((hdr0 >> 18) & 0x3ff) == 1, "count == 1");
    CHECK((hdr0 & 0x3ffc) == 0x2064, "methodOffset == 0x2064 (RASTER_SIZE)");
    CHECK(ld32(pb + 4) == data, "data == 1920|(1080<<16)");
    uint32_t hdr1 = ld32(pb + 8);
    CHECK((hdr1 & 0x3ffc) == NVC37D_UPDATE, "второй метод == UPDATE (0x200)");
    CHECK(NVC77D_HEAD_SET_RASTER_SIZE(1) == 0x2464, "RASTER_SIZE(1) == 0x2464 (+0x400)");
}

static void test_edid_dtd_and_modeset(void)
{
    printf("[test_edid_dtd_and_modeset]\n");
    /* Синтетический EDID со стандартным DTD 1920x1080@60 (CVT-подобные тайминги):
       pclk=148500кГц→14850 (10кГц), hact=1920 hblank=280, vact=1080 vblank=45,
       hsync_off=88 hsync_w=44, vsync_off=4 vsync_w=5. */
    uint8_t e[128]; memset(e, 0, sizeof(e));
    uint8_t *d = e + 54;
    uint32_t pclk10 = 14850;
    d[0] = pclk10 & 0xff; d[1] = pclk10 >> 8;
    d[2] = 1920 & 0xff;  d[3] = 280 & 0xff;  d[4] = ((1920>>8)<<4) | ((280>>8)&0xf);
    d[5] = 1080 & 0xff;  d[6] = 45  & 0xff;  d[7] = ((1080>>8)<<4) | ((45>>8)&0xf);
    d[8] = 88 & 0xff;    d[9] = 44 & 0xff;
    d[10] = ((4 & 0xf) << 4) | (5 & 0xf);
    d[11] = (((88>>8)&0x3)<<6) | (((44>>8)&0x3)<<4) | (((4>>4)&0x3)<<2) | ((5>>4)&0x3);
    d[17] = 0x1e;   /* digital sync, h+/v+ positive (биты 1,2 set) */

    nv_edid_timing t; memset(&t, 0, sizeof(t));
    int rc = nv_edid_parse_dtd(e, sizeof(e), &t);
    CHECK(rc == 0, "parse_dtd OK");
    CHECK(t.pclk_khz == 148500, "pclk == 148500 кГц");
    CHECK(t.hact == 1920 && t.vact == 1080, "hact/vact == 1920/1080");
    CHECK(t.hblank == 280 && t.vblank == 45, "hblank/vblank == 280/45");
    CHECK(t.hsync_off == 88 && t.hsync_w == 44, "hsync off/w == 88/44");
    CHECK(t.vsync_off == 4 && t.vsync_w == 5, "vsync off/w == 4/5");
    CHECK(t.hsync_pos == 1 && t.vsync_pos == 1, "sync polarity positive");

    uint8_t pb[256]; memset(pb, 0, sizeof(pb)); uint32_t off = 0;
    uint32_t soff = 0;
    nv_gsp_disp_build_core_sor(pb, &soff, /*sor*/0, /*head*/0, NVC37D_SOR_PROTOCOL_SINGLE_TMDS_A);
    CHECK(soff == 1*8, "build_core_sor: 1 метод");
    CHECK((ld32(pb+0) & 0x3ffc) == NVC37D_SOR_SET_CONTROL(0), "SOR метод == SOR_SET_CONTROL(0)");
    CHECK(ld32(pb+4) == (1u | (NVC37D_SOR_PROTOCOL_SINGLE_TMDS_A << 8)), "SOR: OWNER head0 | TMDS_A");
    memset(pb, 0, sizeof(pb)); off = 0;
    nv_gsp_disp_build_core_modeset(pb, &off, &t, /*head*/0, /*sor*/0,
                                   NVC37D_SOR_PROTOCOL_SINGLE_TMDS_A);
    CHECK(off == 14*8, "поток: 14 методов (3×viewport/5×raster/CTRL/2×pclk/usage/PROCAMP/ORES)");
    /* порядок nv50_head_flush_set: view→mode→procamp→or. Первый — VIEWPORT_POINT_IN. */
    CHECK((ld32(pb+0) & 0x3ffc) == NVC37D_HEAD_SET_VIEWPORT_POINT_IN(0), "метод[0] == VIEWPORT_POINT_IN(0)");
    /* последний — OUTPUT_RESOURCE (валидируется против режима, поэтому в конце). */
    CHECK((ld32(pb+off-8) & 0x3ffc) == NVC37D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(0), "последний == OUTPUT_RESOURCE(0)");
    /* найти RASTER_SIZE, PIXEL_CLOCK, HEAD_USAGE_BOUNDS в потоке */
    int found_raster = 0, found_pclk = 0, found_usage = 0;
    for (uint32_t o = 0; o < off; o += 8) {
        uint32_t m = ld32(pb+o) & 0x3ffc;
        if (m == NVC77D_HEAD_SET_RASTER_SIZE(0)) {
            CHECK(ld32(pb+o+4) == ((1920u+280u) | ((1080u+45u) << 16)), "RASTER_SIZE = htotal|vtotal");
            found_raster = 1;
        }
        if (m == NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(0)) {
            CHECK(ld32(pb+o+4) == 148500u*1000u, "PIXEL_CLOCK HERTZ = 148500000");
            found_pclk = 1;
        }
        if (m == NVC37D_HEAD_SET_HEAD_USAGE_BOUNDS(0)) {
            CHECK(ld32(pb+o+4) == NVC37D_HEAD_USAGE_BOUNDS_DEFAULT, "HEAD_USAGE_BOUNDS = минимум (0)");
            found_usage = 1;
        }
    }
    CHECK(found_raster && found_pclk && found_usage, "RASTER_SIZE+PIXEL_CLOCK+USAGE_BOUNDS в потоке");
    CHECK(NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(0) == 0x200c, "PIXEL_CLOCK(0) == 0x200c");
    CHECK(NVC37D_HEAD_SET_HEAD_USAGE_BOUNDS(0) == 0x2030, "HEAD_USAGE_BOUNDS(0) == 0x2030");
}

static void test_core_init_and_update(void)
{
    printf("[test_core_init_and_update]\n");
    uint8_t pb[512]; memset(pb, 0, sizeof(pb)); uint32_t off = 0;
    nv_gsp_disp_build_core_init(pb, &off, NV_DISP_HANDLE_SYNCBUF);
    /* 1 (notifier) + 8*3 (window bounds) + 8 (owner) = 33 метода */
    CHECK(off == 33*8, "core_init: 33 метода");
    CHECK((ld32(pb+0) & 0x3ffc) == NVC37D_SET_CONTEXT_DMA_NOTIFIER, "метод[0] == SET_CONTEXT_DMA_NOTIFIER");
    CHECK(ld32(pb+4) == NV_DISP_HANDLE_SYNCBUF, "notifier = SYNCBUF handle");
    /* последний метод — WINDOW_SET_CONTROL(7) OWNER=head3 */
    CHECK((ld32(pb+off-8) & 0x3ffc) == NVC37D_WINDOW_SET_CONTROL_OWNER(7), "последний = WINDOW_SET_CONTROL(7)");
    CHECK(ld32(pb+off-4) == 3u, "OWNER окна7 = head3 (7>>1)");

    off = 0;
    nv_gsp_disp_build_core_update(pb, &off, 0x1u /*window0*/);
    CHECK(off == 3*8, "core_update: 3 метода");
    CHECK((ld32(pb+0) & 0x3ffc) == NVC37D_SET_INTERLOCK_FLAGS, "метод[0] == SET_INTERLOCK_FLAGS");
    CHECK((ld32(pb+8) & 0x3ffc) == NVC37D_SET_WINDOW_INTERLOCK_FLAGS, "метод[1] == SET_WINDOW_INTERLOCK_FLAGS");
    CHECK(ld32(pb+12) == 0x1u, "WINDOW_INTERLOCK = window0");
    CHECK((ld32(pb+16) & 0x3ffc) == NVC37D_UPDATE && ld32(pb+20) == 0x1u, "UPDATE = 0x1");
}

static void test_window_image_and_update(void)
{
    printf("[test_window_image_and_update]\n");
    uint8_t pb[256]; memset(pb, 0, sizeof(pb)); uint32_t off = 0;
    nv_gsp_disp_build_window_image(pb, &off, NVC37E_PARAMS_FORMAT_X8R8G8B8,
                                   1920, 1080, 7680, 0x14000000ull, NV_DISP_HANDLE_VRAM);
    CHECK(off == 10*8, "window_image: 10 методов");
    CHECK((ld32(pb+0) & 0x3ffc) == NVC37E_SET_PRESENT_CONTROL, "метод[0] == SET_PRESENT_CONTROL");
    int f_iso=0, f_off=0, f_pitch=0, f_params=0;
    for (uint32_t o = 0; o < off; o += 8) {
        uint32_t m = ld32(pb+o) & 0x3ffc;
        if (m == NVC37E_SET_CONTEXT_DMA_ISO(0)) { CHECK(ld32(pb+o+4)==NV_DISP_HANDLE_VRAM, "ISO=VRAM handle"); f_iso=1; }
        if (m == NVC37E_SET_OFFSET(0))          { CHECK(ld32(pb+o+4)==(0x14000000u>>8), "OFFSET=fb>>8"); f_off=1; }
        if (m == NVC37E_SET_PLANAR_STORAGE(0))  { CHECK(ld32(pb+o+4)==(7680u>>6), "PITCH=pitch>>6=120"); f_pitch=1; }
        if (m == NVC37E_SET_PARAMS)             { CHECK(ld32(pb+o+4)==0xE6u, "PARAMS FORMAT=X8R8G8B8"); f_params=1; }
    }
    CHECK(f_iso && f_off && f_pitch && f_params, "ISO/OFFSET/PITCH/PARAMS в потоке");

    off = 0;
    nv_gsp_disp_build_window_update(pb, &off, 1 /*interlock core*/);
    CHECK(off == 3*8, "window_update: 3 метода");
    CHECK((ld32(pb+0) & 0x3ffc) == NVC37E_SET_INTERLOCK_FLAGS, "метод[0] == SET_INTERLOCK_FLAGS");
    CHECK(ld32(pb+4) == NVC37E_INTERLOCK_WITH_CORE_BIT, "interlock WITH_CORE");
    CHECK((ld32(pb+16) & 0x3ffc) == NVC37E_UPDATE && ld32(pb+20) == 0x1u, "UPDATE = 0x1");
}

static void test_ctxdma_and_ramht(void)
{
    printf("[test_ctxdma_and_ramht]\n");
    uint8_t desc[NV_CTXDMA_DESC_SIZE];
    /* ctx-dma на весь VRAM: start=0, limit=0x2ffffffff (12 ГиБ-1). */
    nv_gsp_disp_build_ctxdma_desc(desc, 0, 0x2ffffffffull);
    CHECK(ld32(desc+0x00) == 0x45u, "flags0 = VRAM|RW|PAGE_SP = 0x45");
    CHECK(ld32(desc+0x04) == 0u && ld32(desc+0x08) == 0u, "start>>8 = 0");
    CHECK(ld32(desc+0x0c) == (uint32_t)((0x2ffffffffull>>8)&0xffffffffu), "limit>>8 lo");
    CHECK(ld32(desc+0x10) == (uint32_t)((0x2ffffffffull>>8)>>32), "limit>>8 hi");

    uint32_t slot, ctx;
    nv_gsp_disp_ramht_entry(NV_DISP_CHID_CORE, NV_DISP_HANDLE_SYNCBUF, 0xc1d00001u, 0x1000u, &slot, &ctx);
    CHECK(slot < NV_DISP_RAMHT_SIZE, "slot в пределах RAMHT");
    /* context: chid(0)<<25 | (client&0x3fff) | (inst_off<<9) */
    CHECK(ctx == ((0xc1d00001u & 0x3fffu) | (0x1000u << 9)), "context core = client|inst<<9");
    uint32_t slot_w, ctx_w;
    nv_gsp_disp_ramht_entry(NV_DISP_CHID_WINDOW(0), NV_DISP_HANDLE_VRAM, 0xc1d00001u, 0x1020u, &slot_w, &ctx_w);
    CHECK(ctx_w == ((1u<<25) | (0xc1d00001u & 0x3fffu) | (0x1020u << 9)), "context window0 = chid1<<25|...");
}

int main(void)
{
    test_disp_common_alloc();
    test_num_heads();
    test_supported();
    test_or_get_info();
    test_connect_state();
    test_edid();
    test_write_inst_mem();
    test_disp_root_alloc();
    test_channel_pushbuffer();
    test_core_channel_alloc();
    test_assign_sor();
    test_disp_push_method();
    test_edid_dtd_and_modeset();
    test_core_init_and_update();
    test_window_image_and_update();
    test_ctxdma_and_ramht();
    printf(failed ? "\n=== gsp_disp_test: ЕСТЬ ПРОВАЛЫ ===\n" : "\n=== gsp_disp_test: ВСЕ ТЕСТЫ ПРОШЛИ ===\n");
    return failed ? 1 : 0;
}
