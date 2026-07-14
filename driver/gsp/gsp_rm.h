/*
 * gsp_rm.h — слой 3: двусторонний RPC к живому GSP-RM, RM-объекты и память.
 *
 * После GSP_INIT_DONE (слой 2) канал «драйвер↔GSP» уже поднят: cmdq (CPU→GSP) и
 * msgq (GSP→CPU) в общей памяти. Здесь — ПОЛНОЦЕННЫЙ двусторонний RPC: послать
 * команду пост-бут и дождаться типизированного ответа со статусом, плюс первые
 * RPC слоя 3:
 *   - GET_GSP_STATIC_INFO (65) — карта FB-регионов VRAM + внутренние RM-хэндлы GSP;
 *   - GSP_RM_ALLOC (103)       — цепочка RM-объектов client→device→subdevice.
 *
 * Источники (пересказано для лицензии): nouveau r535.c (r535_gsp_cmdq_push,
 * r535_gsp_msg_recv, r535_gsp_rpc_rm_alloc_*, r535_gsp_client_ctor/device_ctor/
 * subdevice_ctor, r535_gsp_rpc_get_gsp_static_info); nvrm 535.113.01:
 * g_rpc-structures.h, rpc_global_enums.h, cl0000/cl0080/cl2080.h, gsp_static_config.h.
 */
#ifndef RTXMACOC_GSP_RM_H
#define RTXMACOC_GSP_RM_H

#include <stdint.h>
#include <stddef.h>
#include "gsp_rpc.h"

/* --- Номера RPC-функций (rpc_global_enums.h 535.113.01) --- */
#define NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO  65u
#define NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL       76u
#define NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC        103u

/* --- Классы RM-объектов (cl0000/cl0080/cl2080.h/cl90f1.h/cl84a0.h) --- */
#define NV01_ROOT               0x00000000u
#define NV01_DEVICE_0           0x00000080u
#define NV20_SUBDEVICE_0        0x00002080u
#define FERMI_VASPACE_A         0x000090f1u
#define NV01_MEMORY_LIST_FBMEM  0x00000082u   /* VRAM memlist (гость даёт физ. страницы) */
#define NV01_MEMORY_LIST_SYSTEM 0x00000081u
#define NV01_MEMORY_VIRTUAL     0x00000070u   /* объект VirtualMemory (ссылается на VA-space) */

/* Канонические хэндлы цепочки (ровно как nouveau r535_gsp_*_ctor). */
#define NV_GSP_RM_CLIENT_HANDLE  0xc1d00000u  /* | id (id=0) */
#define NV_GSP_RM_DEVICE_HANDLE  0xde1d0000u
#define NV_GSP_RM_SUBDEV_HANDLE  0x5d1d0000u
#define NV_GSP_RM_VASPACE_HANDLE 0x90f10000u
#define NV_GSP_RM_VRAM_HANDLE    0x00ca0001u  /* наш хэндл VRAM-объекта */
#define NV_GSP_RM_VMEM_HANDLE    0x00700001u  /* наш хэндл VirtualMemory-объекта */

/* rpc_gsp_rm_control_v03_00 — шапка 24 байта (g_rpc-structures.h):
   hClient@0, hObject@4, cmd@8, status@12, paramsSize@16, flags@20, params@24. */
#define NV_RM_CTRL_HDR_SIZE       24u
#define NV_RM_CTRL_HCLIENT_OFF     0u
#define NV_RM_CTRL_HOBJECT_OFF     4u
#define NV_RM_CTRL_CMD_OFF         8u
#define NV_RM_CTRL_STATUS_OFF     12u
#define NV_RM_CTRL_PARAMSIZE_OFF  16u
#define NV_RM_CTRL_FLAGS_OFF      20u

/* NV2080_CTRL_CMD_FB_GET_INFO_V2 (ctrl2080fb.h): читать конфиг VRAM/heap.
   params = NV2080_CTRL_FB_GET_INFO_V2_PARAMS: fbInfoListSize@0 (u32),
   fbInfoList[54]@4 — каждый NV2080_CTRL_FB_INFO = {index(u32), data(u32)} (8б). */
#define NV2080_CTRL_CMD_FB_GET_INFO_V2       0x20801303u
#define NV2080_CTRL_FB_INFO_MAX_LIST         54u          /* 0x36 */
#define NV2080_CTRL_FB_GET_INFO_V2_SIZE      (4u + NV2080_CTRL_FB_INFO_MAX_LIST * 8u)  /* 436 */
/* индексы (в KiB, если не сказано иначе) */
#define NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE        0x05u
#define NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE         0x07u
#define NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE   0x08u
#define NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE        0x09u
#define NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE        0x16u
#define NV2080_CTRL_FB_INFO_INDEX_USABLE_RAM_SIZE  0x20u

/* NV_VASPACE_ALLOCATION_PARAMETERS (nvos.h, 48б): index@0 flags@4 vaSize@8
   vaStartInternal@16 vaLimitInternal@24 bigPageSize@32 vaBase@40. */
#define NV_VASPACE_ALLOC_PARAMS_SIZE          48u
#define NV_VASPACE_ALLOCATION_INDEX_GPU_NEW   0u

/* --- VRAM-аллокация через ALLOC_MEMORY (memlist, как nouveau fbsr_memlist) ---
 * В GSP-модели VRAM'ом владеет гость: он выбирает физический диапазон и регистрирует
 * его как memlist. rpc_alloc_memory_v13_01 (g_rpc-structures.h) + pte_desc
 * (sdk-structures.h). Функция ALLOC_MEMORY=4 (rpc_global_enums.h). */
#define NV_VGPU_MSG_FUNCTION_ALLOC_MEMORY  4u
/* rpc_alloc_memory_v13_01: hClient@0 hDevice@4 hMemory@8 hClass@12 flags@16
   pteAdjust@20 format@24 length@32(u64) pageCount@40 pteDesc@44. */
#define NV_ALLOCMEM_HCLIENT_OFF    0u
#define NV_ALLOCMEM_HDEVICE_OFF    4u
#define NV_ALLOCMEM_HMEMORY_OFF    8u
#define NV_ALLOCMEM_HCLASS_OFF    12u
#define NV_ALLOCMEM_FLAGS_OFF     16u
#define NV_ALLOCMEM_PTEADJUST_OFF 20u
#define NV_ALLOCMEM_FORMAT_OFF    24u
#define NV_ALLOCMEM_LENGTH_OFF    32u
#define NV_ALLOCMEM_PAGECOUNT_OFF 40u
/* struct pte_desc: u32 bitfield (idr:2, reserved1:14, length:16) @44, затем
   pte_pde[] (u64, 8-выровнен) @48. length кладётся в старшие 16 бит u32. */
#define NV_ALLOCMEM_PTEDESC_OFF   44u
#define NV_ALLOCMEM_PTEARR_OFF    48u
/* NVOS02 flags (nvos.h): PHYSICALITY_CONTIGUOUS(0)@7:4 | LOCATION_VIDMEM(2)@11:8 |
   MAPPING_NO_MAP(1)@31:30 = (2<<8)|(1<<30) = 0x40000200. */
#define NVOS02_FLAGS_FBMEM_CONTIG_NOMAP  0x40000200u
#define NV_MMU_PTE_KIND_GENERIC_MEMORY   6u   /* format для VIDMEM */
#define NV_GSP_PAGE_SHIFT                12u

/* --- Маппинг памяти в GPU VA через MAP_MEMORY_DMA ---
 * rpc_map_memory_dma_v03_00 = NVOS46_PARAMETERS_v03_00 (56б).
 * Функция MAP_MEMORY_DMA=14 (rpc_global_enums.h 535.113.01). */
#define NV_VGPU_MSG_FUNCTION_MAP_MEMORY_DMA  14u
#define NV_MAP_MEMORY_DMA_PARAMS_SIZE         56u
#define NV_MAP_MEMORY_DMA_HCLIENT_OFF          0u
#define NV_MAP_MEMORY_DMA_HDEVICE_OFF          4u
#define NV_MAP_MEMORY_DMA_HDMA_OFF             8u
#define NV_MAP_MEMORY_DMA_HMEMORY_OFF          12u
#define NV_MAP_MEMORY_DMA_OFFSET_OFF           16u
#define NV_MAP_MEMORY_DMA_LENGTH_OFF           24u
#define NV_MAP_MEMORY_DMA_FLAGS_OFF            32u
#define NV_MAP_MEMORY_DMA_DMAOFFSET_OFF        40u
#define NV_MAP_MEMORY_DMA_STATUS_OFF           48u
/* flags=0: read/write, physical page kind, default page size, динамический GPU VA. */
#define NVOS46_FLAGS_DEFAULT                   0u

/* NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS (cl0070.h, 24б): объект VirtualMemory,
   через который MAP_MEMORY_DMA кладёт физ. память в наше VA-пространство.
   offset@0 (u64,IN) limit@8 (u64,IN/OUT) hVASpace@16 (u32,IN). */
#define NV_VMEM_ALLOC_PARAMS_SIZE   24u
#define NV_VMEM_OFFSET_OFF           0u
#define NV_VMEM_LIMIT_OFF            8u
#define NV_VMEM_HVASPACE_OFF        16u

/* rpc_gsp_rm_alloc_v03_00 — шапка 32 байта (g_rpc-structures.h):
   hClient@0, hParent@4, hObject@8, hClass@12, status@16, paramsSize@20,
   flags@24, reserved[4]@28, params@32. */
#define NV_RM_ALLOC_HDR_SIZE       32u
#define NV_RM_ALLOC_HCLIENT_OFF     0u
#define NV_RM_ALLOC_HPARENT_OFF     4u
#define NV_RM_ALLOC_HOBJECT_OFF     8u
#define NV_RM_ALLOC_HCLASS_OFF     12u
#define NV_RM_ALLOC_STATUS_OFF     16u
#define NV_RM_ALLOC_PARAMSIZE_OFF  20u
#define NV_RM_ALLOC_FLAGS_OFF      24u

/* --- GspStaticConfigInfo (gsp_static_config.h 535.113.01) ---
 * Смещения получены compile-probe против SDK-заголовков 535.113.01 (см. gsp_rm.c,
 * комментарий «PROBE»), на железе дополнительно проверяются самопроверкой
 * (numFBRegions∈[1..16], region[0].limit ≈ размер VRAM). */
#define NV_GSP_SCI_SIZE            2168u  /* sizeof(GspStaticConfigInfo) */
#define NV_GSP_SCI_FBREGION_OFF     840u  /* offsetof(.., fbRegionInfoParams) */
#define NV_GSP_SCI_BAR1PDE_OFF     2088u  /* bar1PdeBase (u64) */
#define NV_GSP_SCI_BAR2PDE_OFF     2096u  /* bar2PdeBase (u64) */
#define NV_GSP_SCI_HINTCLIENT_OFF  2152u  /* hInternalClient */
#define NV_GSP_SCI_HINTDEVICE_OFF  2156u  /* hInternalDevice */
#define NV_GSP_SCI_HINTSUBDEV_OFF  2160u  /* hInternalSubdevice */
/* NV2080_CTRL_CMD_FB_GET_FB_REGION_INFO_PARAMS @FBREGION_OFF (ctrl2080fb.h):
   numFBRegions@0 (u32), fbRegion[16]@8; запись региона = 48 байт:
   base@0 limit@8 reserved@16 performance@24 supportCompressed@28
   supportISO@29 bProtected@30. */
#define NV_GSP_FBREGION_NUM_OFF      0u
#define NV_GSP_FBREGION_ARR_OFF      8u
#define NV_GSP_FBREGION_MAX         16u
#define NV_GSP_FBREGION_STRIDE      48u

/* ===================== Канал двустороннего RPC ===================== */

/* Платформенные колбэки (как nv_mmio_t в falcon.c — модуль остаётся переносимым):
   ring — дверной звонок GSP (falcon+0xc00=0); udelay — задержка опроса. */
typedef struct {
    uint8_t            *shm;        /* начало shared-региона очередей */
    nv_gsp_shm_layout_t lay;        /* раскладка очередей (cmdq/msgq/ptes) */
    uint32_t            cmdq_wptr;  /* текущий слот записи cmdq (продолжаем с пре-бута) */
    uint32_t            seq;        /* следующий sequence для cmdq-элемента */
    uint32_t            msgq_rptr;  /* текущий слот чтения msgq (после INIT_DONE) */
    void               *io_ctx;     /* контекст для колбэков (наш nv_mmio_t*) */
    void              (*ring)(void *io_ctx);                /* звонок 0xc00=0 */
    void              (*udelay)(void *io_ctx, uint32_t us); /* задержка */
} nv_gsp_rpc_chan;

/* Один FB-регион VRAM (распарсенный). */
typedef struct {
    uint64_t base, limit, reserved;
    uint32_t performance;
    uint8_t  compressed, iso, prot;
} nv_gsp_fb_region;

/* Результат GET_GSP_STATIC_INFO. */
typedef struct {
    uint32_t         num_regions;
    nv_gsp_fb_region regions[NV_GSP_FBREGION_MAX];
    uint32_t         h_client, h_device, h_subdevice;  /* внутренние хэндлы GSP */
    uint64_t         bar1_pde, bar2_pde;
} nv_gsp_static_info;

/* Коды возврата уровня RPC. */
#define NV_GSP_RM_OK            0
#define NV_GSP_RM_ERR_ARG     (-1)
#define NV_GSP_RM_ERR_TIMEOUT (-2)
#define NV_GSP_RM_ERR_BOUNDS  (-3)

/*
 * Двусторонний RPC: послать функцию fn с payload req[req_len] в cmdq, позвонить
 * в звонок, опросить msgq до ответа той же функции (async-сообщения по пути —
 * пропускаются с consume). При совпадении: payload ответа → reply_out[reply_cap]
 * (усечение допускается), *reply_len ← полная длина payload ответа,
 * *rpc_result ← поле rpc_result заголовка ответа (0 = ок). timeout_us — лимит.
 * Возврат: 0 / NV_GSP_RM_ERR_*. Порт r535_gsp_cmdq_push + r535_gsp_msg_recv.
 */
int nv_gsp_rpc_call(nv_gsp_rpc_chan *ch, uint32_t fn,
                    const uint8_t *req, uint32_t req_len,
                    uint8_t *reply_out, uint32_t reply_cap, uint32_t *reply_len,
                    uint32_t *rpc_result, uint32_t timeout_us);

/* GET_GSP_STATIC_INFO (65): запросить и разобрать FB-карту + хэндлы в *out.
   Возврат 0 при rpc_result==0 и прошедшей самопроверке FB-карты. */
int nv_gsp_get_static_info(nv_gsp_rpc_chan *ch, nv_gsp_static_info *out);

/*
 * GSP_RM_ALLOC (103): аллоцировать RM-объект (hObject класса hClass под hParent).
 * params[params_len] копируются после 32-байтной шапки. *status ← поле status
 * ответа (NV_OK==0). Возврат 0, если RPC прошёл (статус смотреть отдельно).
 * Порт r535_gsp_rpc_rm_alloc_get/push.
 */
int nv_gsp_rm_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hParent,
                    uint32_t hObject, uint32_t hClass,
                    uint8_t *params, uint32_t params_len, uint32_t *status);

/* Конструкторы цепочки (root→device→subdevice). *out_* ← хэндл, *status ← статус. */
int nv_gsp_rm_client_ctor(nv_gsp_rpc_chan *ch, uint32_t *out_client, uint32_t *status);
int nv_gsp_rm_device_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient,
                          uint32_t *out_device, uint32_t *status);
int nv_gsp_rm_subdevice_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                             uint32_t *out_subdev, uint32_t *status);

/* ===================== Проход B: RM_CONTROL + VA-пространство ===================== */

/*
 * GSP_RM_CONTROL (76): управляющий вызов cmd на объекте hObject. params — IN/OUT:
 * запрос копируется из params, ответ GSP копируется обратно в params (params_len байт).
 * *status ← поле status ответа (NV_OK==0). Порт r535_gsp_rpc_rm_ctrl_get/push.
 */
int nv_gsp_rm_control(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hObject, uint32_t cmd,
                      uint8_t *params, uint32_t params_len, uint32_t *status);

/*
 * FB_GET_INFO_V2: прочитать n значений конфигурации VRAM/heap по индексам indices[]
 * на субустройстве hSubdevice; результат → out_data[]. *status ← статус RM.
 */
int nv_gsp_fb_get_info(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hSubdevice,
                       const uint32_t *indices, uint32_t *out_data, uint32_t n,
                       uint32_t *status);

/* Аллокация GPU VA-пространства (FERMI_VASPACE_A) под hDevice — корень GMMU.
   index=GPU_NEW, прочие поля 0 (дефолтное новое полное пространство). */
int nv_gsp_rm_vaspace_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                           uint32_t *out_vaspace, uint32_t *status);

/*
 * Регистрация VRAM-диапазона как memlist (NV01_MEMORY_LIST_FBMEM) через ALLOC_MEMORY.
 * Гость выбирает физ. смещение phys (из usable FB-региона) и размер size (кратно 4К,
 * contiguous). GSP регистрирует объект hMemory. Возврат: *out_handle — хэндл,
 * *rpc_result — поле rpc_result ответа (0 = успех). Порт nouveau fbsr_memlist.
 * Ограничение реализации: size ≤ ~3.9 МиБ (один страничный элемент cmdq).
 */
int nv_gsp_rm_vram_memlist(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                           uint64_t phys, uint64_t size,
                           uint32_t *out_handle, uint32_t *rpc_result);

/*
 * Аллокация объекта VirtualMemory (NV01_MEMORY_VIRTUAL, 0x70) под hDevice,
 * ссылающегося на VA-пространство hVASpace (наш FERMI_VASPACE_A). Именно этот
 * объект (а не сам vaspace) служит hDma в MAP_MEMORY_DMA — RM резолвит hDma в
 * VirtualMemory (context-dma в едином объектном виде, cl0070.h).
 * params NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS (24б): offset=0, limit=0 (→ max), hVASpace.
 * *out_vmem ← хэндл, *status ← статус RM. Порт: virtmemConstruct + cl0070.h.
 */
int nv_gsp_rm_vmem_ctor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                        uint32_t hVASpace, uint32_t *out_vmem, uint32_t *status);

/*
 * MAP_MEMORY_DMA: смаппить [offset, offset+length) объекта hMemory в hDma
 * (объект NV01_MEMORY_VIRTUAL, ссылающийся на FERMI_VASPACE_A) и вернуть GPU VA
 * в *dma_offset. На входе *dma_offset=0 и flags=NVOS46_FLAGS_DEFAULT просят RM
 * выбрать адрес; поле является IN/OUT.
 * *status — NVOS46.status, *rpc_result — статус RPC-заголовка.
 */
int nv_gsp_rm_map_memory_dma(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                             uint32_t hDma, uint32_t hMemory,
                             uint64_t offset, uint64_t length, uint32_t flags,
                             uint64_t *dma_offset, uint32_t *status,
                             uint32_t *rpc_result);

#endif /* RTXMACOC_GSP_RM_H */
