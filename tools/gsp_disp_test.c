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
    printf(failed ? "\n=== gsp_disp_test: ЕСТЬ ПРОВАЛЫ ===\n" : "\n=== gsp_disp_test: ВСЕ ТЕСТЫ ПРОШЛИ ===\n");
    return failed ? 1 : 0;
}
