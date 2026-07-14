/*
 * gsp_fifo_test.c — офлайн-тест слоя 4 (каналы GPFIFO) без GPU.
 *
 * 1) compile-probe раскладки NV_CHANNEL_ALLOC_PARAMS/NV_MEMORY_DESC_PARAMS
 *    (sizeof + offsetof, NV_DECLARE_ALIGNED) против зеркала структуры;
 * 2) framing: channel_alloc (класс AMPERE_CHANNEL_GPFIFO_A, поля params),
 *    bind (cmd 0xa06f0104), schedule (cmd 0xa06f0103) на синтетическом канале.
 *
 * Собрать: make gsp-fifo-test ; запустить: ./tools/gsp_fifo_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../driver/gsp/gsp_fifo.h"

static int failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); failed = 1; } \
                              else printf("  ok: %s\n", msg); } while (0)

static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t ld64(const uint8_t *p)
{ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

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
    ch->cmdq_wptr = 0; ch->seq = 0; ch->msgq_rptr = 0;
    ch->io_ctx = NULL; ch->ring = noop_ring; ch->udelay = noop_udelay;
}

/* --- зеркало для compile-probe раскладки --- */
struct memdesc_mirror { uint64_t base; uint64_t size; uint32_t addressSpace; uint32_t cacheAttrib; };
struct chan_alloc_mirror {
    uint32_t hObjectError;
    uint32_t hObjectBuffer;
    uint64_t gpFifoOffset;
    uint32_t gpFifoEntries;
    uint32_t flags;
    uint32_t hContextShare;
    uint32_t hVASpace;
    uint32_t hUserdMemory[8];
    uint64_t userdOffset[8];
    uint32_t engineType;
    uint32_t cid;
    uint32_t subDeviceId;
    uint32_t hObjectEccError;
    struct memdesc_mirror instanceMem;
    struct memdesc_mirror userdMem;
    struct memdesc_mirror ramfcMem;
    struct memdesc_mirror mthdbufMem;
    uint32_t hPhysChannelGroup;
    uint32_t internalFlags;
    struct memdesc_mirror errorNotifierMem;
    struct memdesc_mirror eccErrorNotifierMem;
    uint32_t ProcessID;
    uint32_t SubProcessID;
    uint32_t encryptIv[3];
    uint32_t decryptIv[3];
    uint32_t hmacNonce[8];
};

static void test_layout(void)
{
    printf("[test_layout]\n");
    CHECK(sizeof(struct memdesc_mirror) == NV_MEMDESC_SIZE, "sizeof(NV_MEMORY_DESC_PARAMS) == 24");
    CHECK(sizeof(struct chan_alloc_mirror) == NV_CHANNEL_ALLOC_PARAMS_SIZE,
          "sizeof(NV_CHANNEL_ALLOC_PARAMS) == 360");
    CHECK(offsetof(struct chan_alloc_mirror, gpFifoOffset) == NV_CHAN_GPFIFOOFFSET_OFF, "gpFifoOffset @8");
    CHECK(offsetof(struct chan_alloc_mirror, gpFifoEntries) == NV_CHAN_GPFIFOENTRIES_OFF, "gpFifoEntries @16");
    CHECK(offsetof(struct chan_alloc_mirror, flags) == NV_CHAN_FLAGS_OFF, "flags @20");
    CHECK(offsetof(struct chan_alloc_mirror, hVASpace) == NV_CHAN_HVASPACE_OFF, "hVASpace @28");
    CHECK(offsetof(struct chan_alloc_mirror, hUserdMemory) == NV_CHAN_HUSERDMEMORY_OFF, "hUserdMemory @32");
    CHECK(offsetof(struct chan_alloc_mirror, userdOffset) == NV_CHAN_USERDOFFSET_OFF, "userdOffset @64");
    CHECK(offsetof(struct chan_alloc_mirror, engineType) == NV_CHAN_ENGINETYPE_OFF, "engineType @128");
    CHECK(offsetof(struct chan_alloc_mirror, instanceMem) == NV_CHAN_INSTANCEMEM_OFF, "instanceMem @144");
    CHECK(offsetof(struct chan_alloc_mirror, userdMem) == NV_CHAN_USERDMEM_OFF, "userdMem @168");
    CHECK(offsetof(struct chan_alloc_mirror, ramfcMem) == NV_CHAN_RAMFCMEM_OFF, "ramfcMem @192");
    CHECK(offsetof(struct chan_alloc_mirror, mthdbufMem) == NV_CHAN_MTHDBUFMEM_OFF, "mthdbufMem @216");
    CHECK(offsetof(struct chan_alloc_mirror, internalFlags) == NV_CHAN_INTERNALFLAGS_OFF, "internalFlags @244");
    CHECK(offsetof(struct memdesc_mirror, addressSpace) == NV_MEMDESC_ADDRSPACE_OFF, "memdesc.addressSpace @16");
    CHECK(offsetof(struct memdesc_mirror, cacheAttrib) == NV_MEMDESC_CACHEATTRIB_OFF, "memdesc.cacheAttrib @20");
}

static nv_gsp_chan_cfg mk_cfg(void)
{
    nv_gsp_chan_cfg c; memset(&c, 0, sizeof(c));
    c.hClient = NV_GSP_RM_CLIENT_HANDLE;
    c.hDevice = NV_GSP_RM_DEVICE_HANDLE;
    c.hVASpace = NV_GSP_RM_VASPACE_HANDLE;
    c.chid = 0;
    c.engineType = 9;                 /* синтетический NV2080_ENGINE_TYPE_COPY0 */
    c.gpfifo_va = 0x20000000ull;      /* GPU-VA кольца (из прямого GMMU) */
    c.gpfifo_entries = 0x400;         /* 1024 записей */
    c.inst_phys = 0x13300000ull; c.inst_size = 0x1000;
    c.userd_phys = 0x13301000ull; c.userd_size = 0x1000;
    c.ramfc_size = 0x200;
    c.mthdbuf_phys = 0x13302000ull; c.mthdbuf_size = 0x5000; c.mthdbuf_sysmem = 0;
    c.priv = 1;
    return c;
}

static void test_channel_alloc(void)
{
    printf("[test_channel_alloc]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_ALLOC_HDR_SIZE]; memset(rep, 0, sizeof(rep));  /* status=0 */
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    nv_gsp_chan_cfg cfg = mk_cfg();
    uint32_t hchan = 0, st = 0xffffffffu;
    int rc = nv_gsp_rm_channel_alloc(&ch, &cfg, &hchan, &st);
    CHECK(rc == NV_GSP_RM_OK, "channel_alloc OK");
    CHECK(st == 0, "status == NV_OK");
    CHECK(hchan == (NV_GSP_RM_CHANNEL_HANDLE | 0), "хэндл канала == 0xf1f00000|chid");

    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *al = ce + NV_GSP_RPC_PAYLOAD_OFF;   /* rpc_gsp_rm_alloc header */
    const uint8_t *pp = al + NV_RM_ALLOC_HDR_SIZE;     /* NV_CHANNEL_ALLOC_PARAMS */
    CHECK(ld32(al + NV_RM_ALLOC_HCLASS_OFF) == AMPERE_CHANNEL_GPFIFO_A, "hClass == AMPERE_CHANNEL_GPFIFO_A");
    CHECK(ld32(al + NV_RM_ALLOC_HPARENT_OFF) == NV_GSP_RM_DEVICE_HANDLE, "hParent == device");
    CHECK(ld32(al + NV_RM_ALLOC_PARAMSIZE_OFF) == NV_CHANNEL_ALLOC_PARAMS_SIZE, "paramsSize == 360");
    CHECK(ld64(pp + NV_CHAN_GPFIFOOFFSET_OFF) == cfg.gpfifo_va, "gpFifoOffset == GPU-VA");
    CHECK(ld32(pp + NV_CHAN_GPFIFOENTRIES_OFF) == cfg.gpfifo_entries, "gpFifoEntries");
    CHECK(ld32(pp + NV_CHAN_HVASPACE_OFF) == NV_GSP_RM_VASPACE_HANDLE, "hVASpace == vaspace");
    CHECK(ld32(pp + NV_CHAN_ENGINETYPE_OFF) == cfg.engineType, "engineType");
    /* flags: priv(bit5) + page_fixed(bit21); chid=0 → индексы 0 */
    CHECK(ld32(pp + NV_CHAN_FLAGS_OFF) == (NVOS04_FLAGS_PRIVILEGED_CHANNEL_BIT
          | NVOS04_FLAGS_USERD_INDEX_PAGE_FIXED_BIT), "flags == PRIV|PAGE_FIXED (0x200020)");
    /* instanceMem: base=inst_phys, addressSpace=VIDMEM(2) */
    CHECK(ld64(pp + NV_CHAN_INSTANCEMEM_OFF + NV_MEMDESC_BASE_OFF) == cfg.inst_phys, "instanceMem.base");
    CHECK(ld32(pp + NV_CHAN_INSTANCEMEM_OFF + NV_MEMDESC_ADDRSPACE_OFF) == NV_MEMDESC_ADDRSPACE_VIDMEM,
          "instanceMem.addressSpace == VIDMEM");
    CHECK(ld64(pp + NV_CHAN_USERDMEM_OFF + NV_MEMDESC_BASE_OFF) == cfg.userd_phys, "userdMem.base");
    CHECK(ld64(pp + NV_CHAN_RAMFCMEM_OFF + NV_MEMDESC_BASE_OFF) == cfg.inst_phys, "ramfcMem.base == inst_phys");
    CHECK(ld64(pp + NV_CHAN_RAMFCMEM_OFF + NV_MEMDESC_SIZE_OFF) == 0x200, "ramfcMem.size == 0x200");
    CHECK(ld32(pp + NV_CHAN_MTHDBUFMEM_OFF + NV_MEMDESC_ADDRSPACE_OFF) == NV_MEMDESC_ADDRSPACE_VIDMEM,
          "mthdbufMem.addressSpace == VIDMEM (sysmem=0)");
}

static void test_bind_schedule(void)
{
    printf("[test_bind_schedule]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    /* два ответа RM_CONTROL подряд (bind, schedule) */
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + 8]; memset(rep, 0, sizeof(rep));
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    put_msg(g_shm, &ch.lay, 1, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 2);

    uint32_t hchan = NV_GSP_RM_CHANNEL_HANDLE | 0;
    uint32_t st = 0xffffffffu;
    int rc = nv_gsp_rm_channel_bind(&ch, NV_GSP_RM_CLIENT_HANDLE, hchan, 9, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "channel_bind OK");
    const uint8_t *cp0 = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp0 + NV_RM_CTRL_CMD_OFF) == NVA06F_CTRL_CMD_BIND, "cmd == BIND (0xa06f0104)");
    CHECK(ld32(cp0 + NV_RM_CTRL_HOBJECT_OFF) == hchan, "hObject == канал");
    CHECK(ld32(cp0 + NV_RM_CTRL_HDR_SIZE + 0) == 9, "BIND.engineType == 9");

    st = 0xffffffffu;
    rc = nv_gsp_rm_channel_schedule(&ch, NV_GSP_RM_CLIENT_HANDLE, hchan, 1, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "channel_schedule OK");
    const uint8_t *cp1 = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF
                       + (size_t)1 * NV_GSP_QUEUE_MSGSIZE + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp1 + NV_RM_CTRL_CMD_OFF) == NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, "cmd == GPFIFO_SCHEDULE (0xa06f0103)");
    CHECK((cp1 + NV_RM_CTRL_HDR_SIZE)[0] == 1, "SCHEDULE.bEnable == 1");
}

/* --- тест device-info-table: framing + парс синтетической таблицы (2 движка) --- */
static void test_device_info(void)
{
    printf("[test_device_info]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);

    /* ответ RM_CONTROL: шапка 24б (status=0) + PARAMS (3212б) с 2 движками. */
    static uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV_FIFO_DEVINFO_PARAMS_SIZE];
    memset(rep, 0, sizeof(rep));
    uint8_t *pp = rep + NV_RM_CTRL_HDR_SIZE;
    st32(pp + NV_FIFO_DEVINFO_NUMENTRIES_OFF, 2);
    /* entry0 = GR0: rm_engine_type=1, runlist=0, pri_base=0x90000, eng_desc=0x11 */
    uint8_t *e0 = pp + NV_FIFO_DEVINFO_ENTRIES_OFF;
    st32(e0 + ENGINE_INFO_TYPE_ENG_DESC * 4u, 0x11);
    st32(e0 + ENGINE_INFO_TYPE_RM_ENGINE_TYPE * 4u, RM_ENGINE_TYPE_GR0);
    st32(e0 + ENGINE_INFO_TYPE_RUNLIST * 4u, 0);
    st32(e0 + ENGINE_INFO_TYPE_RUNLIST_PRI_BASE * 4u, 0x90000);
    /* entry1 = COPY0: rm_engine_type=9, runlist=2, pri_base=0x92000 */
    uint8_t *e1 = pp + NV_FIFO_DEVINFO_ENTRIES_OFF + NV_FIFO_DEVINFO_ENTRY_SIZE;
    st32(e1 + ENGINE_INFO_TYPE_ENG_DESC * 4u, 0x22);
    st32(e1 + ENGINE_INFO_TYPE_RM_ENGINE_TYPE * 4u, RM_ENGINE_TYPE_COPY0);
    st32(e1 + ENGINE_INFO_TYPE_RUNLIST * 4u, 2);
    st32(e1 + ENGINE_INFO_TYPE_RUNLIST_PRI_BASE * 4u, 0x92000);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    nv_gsp_fifo_devinfo di; uint32_t st = 0xffffffffu;
    int rc = nv_gsp_fifo_get_device_info(&ch, NV_GSP_RM_CLIENT_HANDLE,
                                         NV_GSP_RM_SUBDEV_HANDLE, &di, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "device_info OK");
    CHECK(di.count == 2, "count == 2");
    CHECK(di.engines[0].rm_engine_type == RM_ENGINE_TYPE_GR0, "engine[0] == GR0");
    CHECK(di.engines[1].rm_engine_type == RM_ENGINE_TYPE_COPY0, "engine[1] == COPY0");
    CHECK(di.engines[1].runlist == 2, "engine[1].runlist == 2");
    CHECK(di.engines[1].runlist_pri_base == 0x92000, "engine[1].pri_base == 0x92000");
    CHECK(nv_gsp_fifo_find_engine(&di, RM_ENGINE_TYPE_COPY0) == 1, "find COPY0 → индекс 1");
    CHECK(nv_gsp_fifo_find_engine(&di, RM_ENGINE_TYPE_NULL) == -1, "find отсутствующего → -1");
    /* framing: cmd == FIFO_GET_DEVICE_INFO_TABLE, hObject == subdevice */
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE,
          "cmd == FIFO_GET_DEVICE_INFO_TABLE (0x20801112)");
    CHECK(ld32(cp + NV_RM_CTRL_HOBJECT_OFF) == NV_GSP_RM_SUBDEV_HANDLE, "hObject == subdevice");
    CHECK(ld32(cp + NV_RM_CTRL_PARAMSIZE_OFF) == NV_FIFO_DEVINFO_PARAMS_SIZE, "paramsSize == 3212");
}

/* --- тест B: CE-объект (NVB0B5_ALLOCATION_PARAMETERS) --- */
static void test_ce_obj(void)
{
    printf("[test_ce_obj]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_ALLOC_HDR_SIZE]; memset(rep, 0, sizeof(rep));  /* status=0 */
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t hce = NV_GSP_RM_CE_OBJ_HANDLE | 0u, st = 0xffffffffu;
    int rc = nv_gsp_rm_ce_obj_alloc(&ch, NV_GSP_RM_CLIENT_HANDLE,
                                    NV_GSP_RM_CHANNEL_HANDLE | 0u, hce,
                                    AMPERE_DMA_COPY_B, 0x9, &st);
    CHECK(rc == NV_GSP_RM_OK && st == 0, "ce_obj_alloc OK");
    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *al = ce + NV_GSP_RPC_PAYLOAD_OFF;
    const uint8_t *pp = al + NV_RM_ALLOC_HDR_SIZE;
    CHECK(ld32(al + NV_RM_ALLOC_HCLASS_OFF) == AMPERE_DMA_COPY_B, "hClass == AMPERE_DMA_COPY_B (0xC7B5)");
    CHECK(ld32(al + NV_RM_ALLOC_HPARENT_OFF) == (NV_GSP_RM_CHANNEL_HANDLE | 0u), "hParent == канал");
    CHECK(ld32(al + NV_RM_ALLOC_PARAMSIZE_OFF) == NVB0B5_ALLOC_PARAMS_SIZE, "paramsSize == 8");
    CHECK(ld32(pp + NVB0B5_ALLOC_VERSION_OFF) == NVB0B5_ALLOCATION_PARAMETERS_VERSION_1, "version == 1");
    CHECK(ld32(pp + NVB0B5_ALLOC_ENGINETYPE_OFF) == 0x9, "engineType == 0x9 (CE0)");
}

/* --- тест C: pushbuffer host-семафор + GPFIFO-запись --- */
static void test_pushbuffer(void)
{
    printf("[test_pushbuffer]\n");
    uint32_t pb[8];
    uint64_t sem_va = 0x0000000020002000ull;
    uint32_t payload = 0xcafe0001u;
    uint32_t nd = nv_gsp_fifo_build_sem_release(pb, sem_va, payload);
    CHECK(nd == 6, "pushbuffer = 6 dword");
    /* header: OPCODE=1(31:29), COUNT=5(28:16), SUBCH=0, ADDR=0x17 → 0x20050017 */
    CHECK(pb[0] == 0x20050017u, "INC_METHOD header (count=5, addr=SEM_ADDR_LO>>2)");
    CHECK(pb[1] == (uint32_t)(sem_va & 0xfffffffcu), "SEM_ADDR_LO");
    CHECK(pb[2] == (uint32_t)((sem_va >> 32) & 0xffu), "SEM_ADDR_HI");
    CHECK(pb[3] == payload, "SEM_PAYLOAD_LO");
    CHECK(pb[4] == 0, "SEM_PAYLOAD_HI");
    CHECK(pb[5] == (NVC56F_SEM_EXECUTE_OPERATION_RELEASE | NVC56F_SEM_EXECUTE_RELEASE_WFI_EN),
          "SEM_EXECUTE = RELEASE|WFI_EN");

    uint32_t e0 = 0, e1 = 0;
    uint64_t pb_va = 0x0000000020001000ull;
    nv_gsp_fifo_gpfifo_entry(pb_va, nd, &e0, &e1);
    CHECK(e0 == (uint32_t)(pb_va & 0xfffffffcu), "GP_ENTRY0 = pb_va[31:2]");
    CHECK(e1 == (((uint32_t)((pb_va >> 32) & 0xffu)) | (nd << 10)), "GP_ENTRY1 = va_hi | (len<<10)");
    CHECK(((e1 >> 10) & 0x1fffffu) == 6, "GP_ENTRY1 LENGTH == 6 dword");
}

int main(void)
{
    test_layout();
    test_device_info();
    test_channel_alloc();
    test_bind_schedule();
    test_pushbuffer();
    test_ce_obj();
    printf(failed ? "\n=== gsp_fifo_test: ЕСТЬ ПРОВАЛЫ ===\n" : "\n=== gsp_fifo_test: ВСЕ ТЕСТЫ ПРОШЛИ ===\n");
    return failed ? 1 : 0;
}
