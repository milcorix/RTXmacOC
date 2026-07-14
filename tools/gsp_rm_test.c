/*
 * gsp_rm_test.c — офлайн-тест двустороннего RPC слоя 3 (без GPU).
 *
 * Строит синтетический shared-регион очередей, заранее кладёт в msgq «ответ GSP»
 * и проверяет, что nv_gsp_rpc_call / get_static_info / rm_alloc / map_memory_dma:
 *   - корректно собирают cmdq-элемент (checksum, продвижение writePtr/seq);
 *   - распознают ответ по function, собирают multi-page payload;
 *   - пропускают async-сообщение перед ответом;
 *   - правильно парсят FB-карту и хэндлы (на синтетических данных).
 * Плюс юнит на размеры alloc-params (NV0000/NV0080/NV2080/NVOS46) и смещения.
 *
 * Собрать: make gsp-rm-test ; запустить: ./tools/gsp_rm_test
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include "../driver/gsp/gsp_rm.h"

static int failed = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL: %s\n", msg); failed = 1; } \
                              else printf("  ok: %s\n", msg); } while (0)

static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void st64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t ld32(const uint8_t *p)
{ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

/* Колбэки канала: no-op (ответ заранее в очереди). */
static void noop_ring(void *c){ (void)c; }
static void noop_udelay(void *c, uint32_t us){ (void)c; (void)us; }

/* Положить msgq-элемент так, как это сделал бы GSP. */
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

/* Пересчитать checksum cmdq-элемента (XOR u64, верх^низ) — должна сойтись с записанной. */
static uint32_t recompute_checksum(const uint8_t *e, uint32_t ec)
{
    uint32_t total = ec * 0x1000u;
    /* копия с занулённым полем checksum */
    static uint8_t tmp[16 * 0x1000];
    memcpy(tmp, e, total);
    st32(tmp + NV_GSP_MSG_CHECKSUM_OFF, 0);
    uint64_t csum = 0;
    for (uint32_t o = 0; o < total; o += 8) {
        uint64_t v = 0; for (int i=0;i<8;i++) v |= (uint64_t)tmp[o+i] << (8*i);
        csum ^= v;
    }
    return (uint32_t)(csum >> 32) ^ (uint32_t)csum;
}

static uint8_t g_shm[1u << 20];   /* 1 МиБ хватает на ptes+cmdq+msgq (фактически >0.5 МиБ) */

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

/* --- тест 1: базовый rpc_call (echo) + framing cmdq --- */
static void test_basic_call(void)
{
    printf("[test_basic_call]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t payload[64];
    for (int i=0;i<64;i++) payload[i]=(uint8_t)(0xA0+i);
    put_msg(g_shm, &ch.lay, /*slot*/0, /*fn*/0x1234, /*rpc_result*/0, payload, 64, 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint8_t reqbuf[16]; for (int i=0;i<16;i++) reqbuf[i]=(uint8_t)i;
    uint8_t out[64]; uint32_t olen=0, rres=0xff;
    int rc = nv_gsp_rpc_call(&ch, 0x1234, reqbuf, 16, out, sizeof(out), &olen, &rres, 1000000);
    CHECK(rc == NV_GSP_RM_OK, "rpc_call вернул OK");
    CHECK(rres == 0, "rpc_result == 0");
    CHECK(olen == 64, "reply_len == 64");
    CHECK(memcmp(out, payload, 64) == 0, "payload ответа совпал");
    CHECK(ch.msgq_rptr == 1, "msgq_rptr продвинулся до 1");
    CHECK(ch.cmdq_wptr == 1, "cmdq_wptr продвинулся до 1");
    CHECK(ch.seq == 1, "seq инкрементирован");

    /* framing отправленного cmdq-элемента (слот 0) */
    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    CHECK(ld32(ce + NV_GSP_RPC_HDR_OFF + NV_GSP_RPC_F_FUNCTION) == 0x1234, "cmdq function == 0x1234");
    CHECK(ld32(ce + NV_GSP_RPC_HDR_OFF + 4) == NV_GSP_RPC_SIGNATURE, "cmdq signature == CPRV");
    CHECK(ld32(ce + NV_GSP_RPC_HDR_OFF + NV_GSP_RPC_F_LENGTH) == NV_GSP_RPC_HDR_SIZE + 16, "cmdq length == 32+16");
    CHECK(ld32(ce + NV_GSP_MSG_CHECKSUM_OFF) == recompute_checksum(ce, 1), "cmdq checksum валиден");
    CHECK(ld32(g_shm + ch.lay.cmdq_off + NV_MSGQ_TX_WRITEPTR_OFF) == 1, "cmdq.tx.writePtr == 1");
}

/* --- тест 2: пропуск async-сообщения перед ответом --- */
static void test_async_skip(void)
{
    printf("[test_async_skip]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    put_msg(g_shm, &ch.lay, 0, 0x101b, 0, NULL, 16, 1);   /* async-событие */
    uint8_t pl[8] = {1,2,3,4,5,6,7,8};
    put_msg(g_shm, &ch.lay, 1, 0x4321, 0, pl, 8, 1);       /* наш ответ */
    set_msgq_wptr(g_shm, &ch.lay, 2);

    uint8_t out[8]; uint32_t olen=0, rres=0xff;
    int rc = nv_gsp_rpc_call(&ch, 0x4321, NULL, 0, out, sizeof(out), &olen, &rres, 1000000);
    CHECK(rc == NV_GSP_RM_OK, "rpc_call OK (после пропуска async)");
    CHECK(olen == 8 && memcmp(out, pl, 8) == 0, "получен правильный ответ, не async");
    CHECK(ch.msgq_rptr == 2, "msgq_rptr продвинулся через оба сообщения");
}

/* --- тест 3: multi-page ответ --- */
static void test_multipage(void)
{
    printf("[test_multipage]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    static uint8_t big[5000];
    for (int i=0;i<5000;i++) big[i]=(uint8_t)(i*7+1);
    /* элемент = 48+32+5000 = 5080 → 2 страницы */
    put_msg(g_shm, &ch.lay, 0, 0x55, 0, big, 5000, 2);
    set_msgq_wptr(g_shm, &ch.lay, 2);

    static uint8_t out[5000]; uint32_t olen=0, rres=0xff;
    int rc = nv_gsp_rpc_call(&ch, 0x55, NULL, 0, out, sizeof(out), &olen, &rres, 1000000);
    CHECK(rc == NV_GSP_RM_OK, "multi-page rpc_call OK");
    CHECK(olen == 5000, "reply_len == 5000 (2 страницы)");
    CHECK(memcmp(out, big, 5000) == 0, "multi-page payload собран целиком");
    CHECK(ch.msgq_rptr == 2, "msgq_rptr продвинулся на 2 страницы");
}

/* --- тест 4: get_static_info на синтетической FB-карте --- */
static void test_static_info(void)
{
    printf("[test_static_info]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    static uint8_t sci[NV_GSP_SCI_SIZE];
    memset(sci, 0, sizeof(sci));
    uint8_t *fb = sci + NV_GSP_SCI_FBREGION_OFF;
    st32(fb + NV_GSP_FBREGION_NUM_OFF, 2);                 /* 2 региона */
    uint8_t *r0 = fb + NV_GSP_FBREGION_ARR_OFF;
    st64(r0 + 0, 0x0);                 st64(r0 + 8, 0x2ff9fffffull);   /* base/limit региона 0 */
    st64(r0 + 16, 0);                  st32(r0 + 24, 0x11);
    r0[28]=1; r0[29]=1; r0[30]=0;
    uint8_t *r1 = fb + NV_GSP_FBREGION_ARR_OFF + NV_GSP_FBREGION_STRIDE;
    st64(r1 + 0, 0x2ffa00000ull);      st64(r1 + 8, 0x2ffafffffull);
    st32(sci + NV_GSP_SCI_HINTCLIENT_OFF, 0xc1d00abc);
    st32(sci + NV_GSP_SCI_HINTDEVICE_OFF, 0xde1d0abc);
    st32(sci + NV_GSP_SCI_HINTSUBDEV_OFF, 0x5d1d0abc);
    st64(sci + NV_GSP_SCI_BAR1PDE_OFF, 0xdead0000ull);

    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, 0,
            sci, NV_GSP_SCI_SIZE, 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    nv_gsp_static_info si;
    int rc = nv_gsp_get_static_info(&ch, &si);
    CHECK(rc == NV_GSP_RM_OK, "get_static_info OK");
    CHECK(si.num_regions == 2, "num_regions == 2");
    CHECK(si.regions[0].limit == 0x2ff9fffffull, "region[0].limit распарсен");
    CHECK(si.regions[0].compressed == 1 && si.regions[0].iso == 1, "флаги региона 0");
    CHECK(si.regions[1].base == 0x2ffa00000ull, "region[1].base распарсен");
    CHECK(si.h_client == 0xc1d00abc && si.h_subdevice == 0x5d1d0abc, "внутренние хэндлы GSP");
    CHECK(si.bar1_pde == 0xdead0000ull, "bar1PdeBase распарсен");
}

/* --- тест 5: rm_client_ctor (root) --- */
static void test_client_ctor(void)
{
    printf("[test_client_ctor]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    /* ответ GSP: rpc_gsp_rm_alloc_v03_00 echo со status=0 */
    uint8_t rep[NV_RM_ALLOC_HDR_SIZE]; memset(rep, 0, sizeof(rep));
    st32(rep + NV_RM_ALLOC_STATUS_OFF, 0);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t hcli=0, st=0xff;
    int rc = nv_gsp_rm_client_ctor(&ch, &hcli, &st);
    CHECK(rc == NV_GSP_RM_OK, "client_ctor OK");
    CHECK(st == 0, "status == NV_OK");
    CHECK(hcli == NV_GSP_RM_CLIENT_HANDLE, "хэндл клиента == 0xc1d00000");

    /* проверить отправленный cmdq-элемент: класс NV01_ROOT, hClient в payload */
    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *al = ce + NV_GSP_RPC_PAYLOAD_OFF;       /* rpc_gsp_rm_alloc */
    CHECK(ld32(ce + NV_GSP_RPC_HDR_OFF + NV_GSP_RPC_F_FUNCTION) == NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, "cmdq function == GSP_RM_ALLOC");
    CHECK(ld32(al + NV_RM_ALLOC_HCLASS_OFF) == NV01_ROOT, "hClass == NV01_ROOT");
    CHECK(ld32(al + NV_RM_ALLOC_HCLIENT_OFF) == NV_GSP_RM_CLIENT_HANDLE, "hClient в payload");
    CHECK(ld32(al + NV_RM_ALLOC_PARAMSIZE_OFF) == 108, "paramsSize == sizeof(NV0000)");
    /* NV0000: processID = ~0 @ params+4 */
    CHECK(ld32(al + NV_RM_ALLOC_HDR_SIZE + 4) == 0xffffffffu, "NV0000.processID == ~0");
}

/* --- тест 6: RM_CONTROL (params IN/OUT) --- */
static void test_rm_control(void)
{
    printf("[test_rm_control]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    /* ответ GSP: rpc_gsp_rm_control echo (24б шапка, status=0) + params(16) с новыми значениями */
    uint8_t rep[NV_RM_CTRL_HDR_SIZE + 16]; memset(rep, 0, sizeof(rep));
    st32(rep + NV_RM_CTRL_STATUS_OFF, 0);
    for (int i=0;i<16;i++) rep[NV_RM_CTRL_HDR_SIZE + i] = (uint8_t)(0xB0 + i);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint8_t params[16]; for (int i=0;i<16;i++) params[i]=(uint8_t)i;   /* IN */
    uint32_t st=0xff;
    int rc = nv_gsp_rm_control(&ch, 0x111, 0x222, 0x333, params, 16, &st);
    CHECK(rc == NV_GSP_RM_OK, "rm_control OK");
    CHECK(st == 0, "status == NV_OK");
    CHECK((uint8_t)params[0]==0xB0 && (uint8_t)params[15]==0xBF, "params перезаписаны ответом (IN/OUT)");
    /* framing cmdq: fn=76, hObject/cmd в payload */
    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *cp = ce + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(ce + NV_GSP_RPC_HDR_OFF + NV_GSP_RPC_F_FUNCTION) == NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, "cmdq function == GSP_RM_CONTROL");
    CHECK(ld32(cp + NV_RM_CTRL_HOBJECT_OFF) == 0x222, "hObject в payload");
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == 0x333, "cmd в payload");
    CHECK(ld32(cp + NV_RM_CTRL_PARAMSIZE_OFF) == 16, "paramsSize == 16");
}

/* --- тест 7: FB_GET_INFO_V2 (парс значений по индексам) --- */
static void test_fb_get_info(void)
{
    printf("[test_fb_get_info]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint32_t idx[3] = { NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE,
                        NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE,
                        NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE };
    /* ответ: ctrl-шапка(24, status=0) + FB_GET_INFO_V2_PARAMS(436): data заполнены GSP */
    static uint8_t rep[NV_RM_CTRL_HDR_SIZE + NV2080_CTRL_FB_GET_INFO_V2_SIZE];
    memset(rep, 0, sizeof(rep));
    uint8_t *pp = rep + NV_RM_CTRL_HDR_SIZE;
    st32(pp + 0, 3);                                        /* fbInfoListSize */
    uint32_t vals[3] = { 0x00c00000, 0x00c00000, 0x00b00000 };
    for (int i=0;i<3;i++){ st32(pp + 4 + i*8, idx[i]); st32(pp + 4 + i*8 + 4, vals[i]); }
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t out[3] = {0,0,0}, st=0xff;
    int rc = nv_gsp_fb_get_info(&ch, 0xc1d00000, NV_GSP_RM_SUBDEV_HANDLE, idx, out, 3, &st);
    CHECK(rc == NV_GSP_RM_OK, "fb_get_info OK");
    CHECK(st == 0, "status == NV_OK");
    CHECK(out[0]==vals[0] && out[1]==vals[1] && out[2]==vals[2], "значения по индексам распарсены");
    /* cmdq: cmd == FB_GET_INFO_V2, hObject == subdevice */
    const uint8_t *cp = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0 + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(cp + NV_RM_CTRL_CMD_OFF) == NV2080_CTRL_CMD_FB_GET_INFO_V2, "cmd == FB_GET_INFO_V2");
    CHECK(ld32(cp + NV_RM_CTRL_HOBJECT_OFF) == NV_GSP_RM_SUBDEV_HANDLE, "hObject == subdevice");
    CHECK(ld32(cp + NV_RM_CTRL_HDR_SIZE + 0) == 3, "fbInfoListSize == 3 в запросе");
}

/* --- тест 8: vaspace_ctor (FERMI_VASPACE_A) --- */
static void test_vaspace_ctor(void)
{
    printf("[test_vaspace_ctor]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[NV_RM_ALLOC_HDR_SIZE]; memset(rep, 0, sizeof(rep));
    st32(rep + NV_RM_ALLOC_STATUS_OFF, 0);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t hva=0, st=0xff;
    int rc = nv_gsp_rm_vaspace_ctor(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DEVICE_HANDLE, &hva, &st);
    CHECK(rc == NV_GSP_RM_OK, "vaspace_ctor OK");
    CHECK(st == 0, "status == NV_OK");
    CHECK(hva == NV_GSP_RM_VASPACE_HANDLE, "хэндл vaspace == 0x90f10000");
    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *al = ce + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(al + NV_RM_ALLOC_HCLASS_OFF) == FERMI_VASPACE_A, "hClass == FERMI_VASPACE_A");
    CHECK(ld32(al + NV_RM_ALLOC_HPARENT_OFF) == NV_GSP_RM_DEVICE_HANDLE, "hParent == device");
    CHECK(ld32(al + NV_RM_ALLOC_PARAMSIZE_OFF) == 48, "paramsSize == 48 (NV_VASPACE_ALLOCATION_PARAMETERS)");
}

/* --- тест 9: VRAM memlist (NV01_MEMORY_LIST_FBMEM через ALLOC_MEMORY) --- */
static uint64_t ld64t(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }
static void test_vram_memlist(void)
{
    printf("[test_vram_memlist]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    /* ответ GSP на ALLOC_MEMORY: тот же function=4, rpc_result=0 (echo, payload не важен) */
    uint8_t rep[16]; memset(rep, 0, sizeof(rep));
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_ALLOC_MEMORY, /*rpc_result*/0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint64_t phys = 0x10000000ull, size = 0x100000ull;  /* 256 страниц */
    uint32_t hmem=0, rres=0xff;
    int rc = nv_gsp_rm_vram_memlist(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DEVICE_HANDLE,
                                    phys, size, &hmem, &rres);
    CHECK(rc == NV_GSP_RM_OK, "vram_memlist OK (rpc_result=0)");
    CHECK(rres == 0, "rpc_result == 0");
    CHECK(hmem == NV_GSP_RM_VRAM_HANDLE, "хэндл VRAM == 0x00ca0001");
    /* framing запроса: функция, класс, flags, format, pageCount, pte-массив */
    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *rp = ce + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(ce + NV_GSP_RPC_HDR_OFF + NV_GSP_RPC_F_FUNCTION) == NV_VGPU_MSG_FUNCTION_ALLOC_MEMORY, "function == ALLOC_MEMORY(4)");
    CHECK(ld32(rp + NV_ALLOCMEM_HCLASS_OFF) == NV01_MEMORY_LIST_FBMEM, "hClass == NV01_MEMORY_LIST_FBMEM");
    CHECK(ld32(rp + NV_ALLOCMEM_FLAGS_OFF) == NVOS02_FLAGS_FBMEM_CONTIG_NOMAP, "flags = VIDMEM|CONTIG|NO_MAP");
    CHECK(ld32(rp + NV_ALLOCMEM_FORMAT_OFF) == NV_MMU_PTE_KIND_GENERIC_MEMORY, "format == GENERIC_MEMORY(6)");
    CHECK(ld32(rp + NV_ALLOCMEM_PAGECOUNT_OFF) == 256, "pageCount == 256");
    CHECK((ld32(rp + NV_ALLOCMEM_PTEDESC_OFF) >> 16) == 256, "pte_desc.length == 256 (старшие 16 бит)");
    CHECK(ld64t(rp + NV_ALLOCMEM_PTEARR_OFF) == (phys >> 12), "pte[0] == phys>>12");
    CHECK(ld64t(rp + NV_ALLOCMEM_PTEARR_OFF + 8) == (phys >> 12) + 1, "pte[1] == phys>>12 + 1 (contiguous)");
}

/* --- тест 10a: VirtualMemory ctor (NV01_MEMORY_VIRTUAL → ссылка на vaspace) --- */
static void test_vmem_ctor(void)
{
    printf("[test_vmem_ctor]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);
    uint8_t rep[64]; memset(rep, 0, sizeof(rep));   /* status@16 = NV_OK (0) */
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint32_t hvmem=0, st=0xff;
    int rc = nv_gsp_rm_vmem_ctor(&ch, NV_GSP_RM_CLIENT_HANDLE, NV_GSP_RM_DEVICE_HANDLE,
                                 NV_GSP_RM_VASPACE_HANDLE, &hvmem, &st);
    CHECK(rc == NV_GSP_RM_OK, "vmem_ctor OK");
    CHECK(st == 0, "status == NV_OK");
    CHECK(hvmem == NV_GSP_RM_VMEM_HANDLE, "хэндл VMEM == 0x00700001");
    /* framing: RM_ALLOC с классом NV01_MEMORY_VIRTUAL и params.hVASpace */
    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *hp = ce + NV_GSP_RPC_PAYLOAD_OFF;              /* rpc_gsp_rm_alloc header */
    const uint8_t *pp = hp + NV_RM_ALLOC_HDR_SIZE;               /* params */
    CHECK(ld32(hp + NV_RM_ALLOC_HCLASS_OFF) == NV01_MEMORY_VIRTUAL, "hClass == NV01_MEMORY_VIRTUAL(0x70)");
    CHECK(ld32(pp + NV_VMEM_HVASPACE_OFF) == NV_GSP_RM_VASPACE_HANDLE, "params.hVASpace == vaspace handle");
    CHECK(ld64t(pp + NV_VMEM_OFFSET_OFF) == 0, "params.offset == 0");
    CHECK(ld64t(pp + NV_VMEM_LIMIT_OFF) == 0, "params.limit == 0 (→ max)");
}

/* --- тест 10: MAP_MEMORY_DMA (VRAM memlist → VirtualMemory → GPU VA) --- */
static void test_map_memory_dma(void)
{
    printf("[test_map_memory_dma]\n");
    nv_gsp_rpc_chan ch; chan_init(&ch);

    uint8_t rep[NV_MAP_MEMORY_DMA_PARAMS_SIZE]; memset(rep, 0, sizeof(rep));
    uint64_t expected_va = 0x0000000201000000ull;
    st64(rep + NV_MAP_MEMORY_DMA_DMAOFFSET_OFF, expected_va);
    st32(rep + NV_MAP_MEMORY_DMA_STATUS_OFF, 0);
    put_msg(g_shm, &ch.lay, 0, NV_VGPU_MSG_FUNCTION_MAP_MEMORY_DMA,
            /*rpc_result*/0, rep, sizeof(rep), 1);
    set_msgq_wptr(g_shm, &ch.lay, 1);

    uint64_t gpu_va = 0;
    uint32_t st = 0xffffffffu, rres = 0xffffffffu;
    int rc = nv_gsp_rm_map_memory_dma(&ch, NV_GSP_RM_CLIENT_HANDLE,
                                      NV_GSP_RM_DEVICE_HANDLE,
                                      NV_GSP_RM_VMEM_HANDLE,
                                      NV_GSP_RM_VRAM_HANDLE,
                                      0, 0x100000ull, NVOS46_FLAGS_DEFAULT,
                                      &gpu_va, &st, &rres);
    CHECK(rc == NV_GSP_RM_OK, "map_memory_dma OK");
    CHECK(rres == 0, "rpc_result == 0");
    CHECK(st == 0, "NVOS46.status == NV_OK");
    CHECK(gpu_va == expected_va, "dmaOffset OUT == GPU VA");

    const uint8_t *ce = g_shm + ch.lay.cmdq_off + NV_GSP_QUEUE_ENTRYOFF + 0;
    const uint8_t *mp = ce + NV_GSP_RPC_PAYLOAD_OFF;
    CHECK(ld32(ce + NV_GSP_RPC_HDR_OFF + NV_GSP_RPC_F_FUNCTION) ==
          NV_VGPU_MSG_FUNCTION_MAP_MEMORY_DMA, "function == MAP_MEMORY_DMA(14)");
    CHECK(ld32(mp + NV_MAP_MEMORY_DMA_HDMA_OFF) == NV_GSP_RM_VMEM_HANDLE,
          "hDma == NV01_MEMORY_VIRTUAL handle");
    CHECK(ld32(mp + NV_MAP_MEMORY_DMA_HMEMORY_OFF) == NV_GSP_RM_VRAM_HANDLE,
          "hMemory == VRAM memlist handle");
    CHECK(ld64t(mp + NV_MAP_MEMORY_DMA_OFFSET_OFF) == 0, "offset == 0");
    CHECK(ld64t(mp + NV_MAP_MEMORY_DMA_LENGTH_OFF) == 0x100000ull, "length == 1 MiB");
    CHECK(ld32(mp + NV_MAP_MEMORY_DMA_FLAGS_OFF) == 0, "flags == dynamic VA defaults");
    CHECK(ld64t(mp + NV_MAP_MEMORY_DMA_DMAOFFSET_OFF) == 0, "dmaOffset IN == 0");
}

/* --- тест 11: размеры структур и константы --- */
struct nv0000_mirror { uint32_t hClient; uint32_t processID; char processName[100]; };
struct nv0080_mirror { uint32_t deviceId, hClientShare, hTargetClient, hTargetDevice;
                       int32_t flags; uint64_t vaSpaceSize, vaStartInternal, vaLimitInternal;
                       int32_t vaMode; };
struct nv2080_mirror { uint32_t subDeviceId; };
struct nvos46_mirror { uint32_t hClient, hDevice, hDma, hMemory;
                       uint64_t offset, length; uint32_t flags;
                       uint64_t dmaOffset; uint32_t status; };

static void test_layout(void)
{
    printf("[test_layout]\n");
    CHECK(sizeof(struct nv0000_mirror) == 108, "sizeof(NV0000_ALLOC_PARAMETERS) == 108");
    CHECK(sizeof(struct nv0080_mirror) == 56,  "sizeof(NV0080_ALLOC_PARAMETERS) == 56");
    CHECK(sizeof(struct nv2080_mirror) == 4,   "sizeof(NV2080_ALLOC_PARAMETERS) == 4");
    CHECK(NV_RM_ALLOC_HDR_SIZE == 32, "rpc_gsp_rm_alloc header == 32");
    CHECK(NV_GSP_SCI_SIZE == 2168, "sizeof(GspStaticConfigInfo) == 2168 (probe)");
    CHECK(NV_GSP_SCI_FBREGION_OFF == 840, "off fbRegionInfoParams == 840 (probe)");
    CHECK(NV_GSP_SCI_HINTSUBDEV_OFF == 2160, "off hInternalSubdevice == 2160 (probe)");
    CHECK(NV_RM_CTRL_HDR_SIZE == 24, "rpc_gsp_rm_control header == 24");
    CHECK(NV2080_CTRL_FB_GET_INFO_V2_SIZE == 436, "FB_GET_INFO_V2_PARAMS == 436 (4+54*8)");
    CHECK(NV_VASPACE_ALLOC_PARAMS_SIZE == 48, "NV_VASPACE_ALLOCATION_PARAMETERS == 48");
    CHECK(NV_ALLOCMEM_PTEARR_OFF == 48, "pte_pde[] @48 в rpc_alloc_memory_v13_01");
    CHECK(NVOS02_FLAGS_FBMEM_CONTIG_NOMAP == 0x40000200u, "NVOS02 flags VIDMEM|CONTIG|NO_MAP == 0x40000200");
    CHECK(sizeof(struct nvos46_mirror) == NV_MAP_MEMORY_DMA_PARAMS_SIZE,
          "sizeof(NVOS46_PARAMETERS_v03_00) == 56");
    CHECK(offsetof(struct nvos46_mirror, dmaOffset) == NV_MAP_MEMORY_DMA_DMAOFFSET_OFF,
          "NVOS46.dmaOffset @40");
    CHECK(offsetof(struct nvos46_mirror, status) == NV_MAP_MEMORY_DMA_STATUS_OFF,
          "NVOS46.status @48");
    CHECK(NV_VMEM_ALLOC_PARAMS_SIZE == 24, "NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS == 24 (probe)");
    CHECK(NV_VMEM_HVASPACE_OFF == 16, "NV_MEMORY_VIRTUAL.hVASpace @16 (probe)");
}

int main(void)
{
    test_basic_call();
    test_async_skip();
    test_multipage();
    test_static_info();
    test_client_ctor();
    test_rm_control();
    test_fb_get_info();
    test_vaspace_ctor();
    test_vram_memlist();
    test_vmem_ctor();
    test_map_memory_dma();
    test_layout();
    printf(failed ? "\n=== gsp_rm_test: ЕСТЬ ПРОВАЛЫ ===\n" : "\n=== gsp_rm_test: ВСЕ ТЕСТЫ ПРОШЛИ ===\n");
    return failed ? 1 : 0;
}
