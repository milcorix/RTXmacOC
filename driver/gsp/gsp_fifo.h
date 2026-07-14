/*
 * gsp_fifo.h — слой 4: каналы GPFIFO + движки (CE/GR) через GSP-RM.
 *
 * После слоя 3 (RPC, RM-цепочка, VA-пространство, VRAM-маппинг) поднимаем канал
 * GPFIFO: клиент отдаёт GSP физ-адреса своих буферов (instance/RAMFC/USERD во VRAM)
 * и GPU-VA кольца GPFIFO, GSP планирует канал в runlist. Затем на канал вешается
 * объект движка (copy-engine/GR), собирается pushbuffer и отправляется на исполнение.
 *
 * Эталон (пересказано для лицензии): nouveau nvkm/engine/fifo/r535.c
 * (r535_chan_ramfc_write, r535_chan), fifo/ga102.c (ga102_fifo). Структуры/классы —
 * nvrm 535.113.01: alloc/alloc_channel.h (NV_CHANNEL_ALLOC_PARAMS), nvif/class.h
 * (номера классов), ctrl/ctrla06f/ctrla06fgpfifo.h (BIND/SCHEDULE). Полная тех-запись:
 * docs/gsp-layer4-fifo.md.
 */
#ifndef RTXMACOC_GSP_FIFO_H
#define RTXMACOC_GSP_FIFO_H

#include <stdint.h>
#include "gsp_rm.h"

/* --- Классы объектов (nvif/class.h, сверено v6.11) --- */
#define KEPLER_CHANNEL_GROUP_A     0x0000a06cu   /* TSG (в r535_chan не используется) */
#define AMPERE_CHANNEL_GPFIFO_A    0x0000c56fu   /* канал GPFIFO (Ada = ga102-путь) */
#define AMPERE_USERMODE_A          0x0000c561u   /* usermode/doorbell (submit) */
#define AMPERE_DMA_COPY_B          0x0000c7b5u   /* copy-engine (Ada) */
#define ADA_A                      0x0000c997u   /* 3D/graphics */
#define ADA_COMPUTE_A              0x0000c9c0u   /* compute */

/* Базовые хэндлы наших объектов слоя 4. */
#define NV_GSP_RM_CHANNEL_HANDLE   0xf1f00000u   /* | chid (как nouveau) */
#define NV_GSP_RM_CE_OBJ_HANDLE    0x0000ce00u   /* | inst — объект copy-engine */

/* --- NV_CHANNEL_ALLOC_PARAMS (alloc_channel.h, sizeof=360, compile-probe) ---
   NV_DECLARE_ALIGNED(..,8), NV_MAX_SUBDEVICES=8. */
#define NV_CHANNEL_ALLOC_PARAMS_SIZE   360u
#define NV_CHAN_HOBJECTERROR_OFF         0u
#define NV_CHAN_HOBJECTBUFFER_OFF        4u
#define NV_CHAN_GPFIFOOFFSET_OFF         8u    /* u64 */
#define NV_CHAN_GPFIFOENTRIES_OFF       16u
#define NV_CHAN_FLAGS_OFF               20u
#define NV_CHAN_HCONTEXTSHARE_OFF       24u
#define NV_CHAN_HVASPACE_OFF            28u
#define NV_CHAN_HUSERDMEMORY_OFF        32u    /* NvHandle[8] */
#define NV_CHAN_USERDOFFSET_OFF         64u    /* u64[8] */
#define NV_CHAN_ENGINETYPE_OFF         128u
#define NV_CHAN_CID_OFF                132u
#define NV_CHAN_SUBDEVICEID_OFF        136u
#define NV_CHAN_HOBJECTECCERROR_OFF    140u
#define NV_CHAN_INSTANCEMEM_OFF        144u    /* NV_MEMORY_DESC_PARAMS */
#define NV_CHAN_USERDMEM_OFF           168u
#define NV_CHAN_RAMFCMEM_OFF           192u
#define NV_CHAN_MTHDBUFMEM_OFF         216u
#define NV_CHAN_HPHYSCHANNELGROUP_OFF  240u
#define NV_CHAN_INTERNALFLAGS_OFF      244u
#define NV_CHAN_ERRORNOTIFIERMEM_OFF   248u
#define NV_CHAN_ECCERRORNOTIFIERMEM_OFF 272u
#define NV_CHAN_PROCESSID_OFF          296u
#define NV_CHAN_SUBPROCESSID_OFF       300u

/* NV_MEMORY_DESC_PARAMS (24б): base@0(u64) size@8(u64) addressSpace@16 cacheAttrib@20. */
#define NV_MEMDESC_SIZE                24u
#define NV_MEMDESC_BASE_OFF             0u
#define NV_MEMDESC_SIZE_OFF             8u
#define NV_MEMDESC_ADDRSPACE_OFF       16u
#define NV_MEMDESC_CACHEATTRIB_OFF     20u
#define NV_MEMDESC_ADDRSPACE_SYSMEM     1u
#define NV_MEMDESC_ADDRSPACE_VIDMEM     2u

/* NVOS04 flags (alloc_channel.h). Для kernel-канала (nouveau .priv=true). */
#define NVOS04_FLAGS_PRIVILEGED_CHANNEL_BIT        (1u << 5)
#define NVOS04_FLAGS_USERD_INDEX_VALUE_SHIFT        8u    /* [10:8]  */
#define NVOS04_FLAGS_USERD_INDEX_PAGE_VALUE_SHIFT  12u    /* [20:12] */
#define NVOS04_FLAGS_USERD_INDEX_PAGE_FIXED_BIT    (1u << 21)

/* --- Контролы канала (ctrla06fgpfifo.h) --- */
#define NVA06F_CTRL_CMD_GPFIFO_SCHEDULE  0xa06f0103u  /* {NvBool bEnable; NvBool bSkipSubmit} */
#define NVA06F_CTRL_CMD_BIND             0xa06f0104u  /* {NvU32 engineType} */
#define NVA06F_CTRL_BIND_PARAMS_SIZE          4u   /* {NvU32 engineType} */
#define NVA06F_CTRL_SCHEDULE_PARAMS_SIZE      2u   /* {NvBool bEnable; NvBool bSkipSubmit} — RM сверяет точный размер */

/* Конфигурация канала: физ-адреса буферов во VRAM + GPU-VA кольца GPFIFO. */
typedef struct {
    uint32_t hClient;
    uint32_t hDevice;       /* родитель канала */
    uint32_t hVASpace;      /* наш FERMI_VASPACE_A (0x90f10000) */
    uint32_t chid;          /* индекс канала (хэндл = 0xf1f00000|chid) */
    uint32_t engineType;    /* NV2080_ENGINE_TYPE_* (из device-info) */
    uint64_t gpfifo_va;     /* GPU-VA кольца GPFIFO (замаплено прямым GMMU) */
    uint32_t gpfifo_entries;/* число записей GPFIFO (кольцо: 8 байт/запись) */
    uint64_t inst_phys;     /* instance block во VRAM (RAMFC внутри, off 0) */
    uint64_t inst_size;
    uint64_t userd_phys;    /* USERD во VRAM */
    uint64_t userd_size;
    uint64_t ramfc_size;    /* обычно 0x200 */
    uint64_t mthdbuf_phys;  /* method-buffer */
    uint64_t mthdbuf_size;
    uint32_t mthdbuf_sysmem;/* 1 → mthdbuf в sysmem (addressSpace=1), иначе VRAM(2) */
    int      priv;          /* привилегированный канал (kernel) */
} nv_gsp_chan_cfg;

/* --- Проход A0: таблица движков (ctrl2080fifo.h) ---
 * NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE на subdevice → список движков GPU с
 * их RM_ENGINE_TYPE/runlist. Из него берём engineType для канала (как r535_fifo_runl_ctor). */
#define NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE  0x20801112u
#define NV_FIFO_DEVINFO_MAX_ENTRIES        32u   /* MAX_ENTRIES за один вызов */
#define NV_FIFO_DEVINFO_DATA_TYPES         16u   /* engineData[16] */
#define NV_FIFO_DEVINFO_ENTRY_SIZE        100u   /* engineData[16]+pbdma+name */
/* NV2080_CTRL_FIFO_GET_DEVICE_INFO_TABLE_PARAMS: baseIndex@0 numEntries@4 bMore@8
   entries[32]@12 (по 100б). */
#define NV_FIFO_DEVINFO_BASEINDEX_OFF       0u
#define NV_FIFO_DEVINFO_NUMENTRIES_OFF      4u
#define NV_FIFO_DEVINFO_BMORE_OFF           8u
#define NV_FIFO_DEVINFO_ENTRIES_OFF        12u
#define NV_FIFO_DEVINFO_PARAMS_SIZE  (NV_FIFO_DEVINFO_ENTRIES_OFF + \
                                      NV_FIFO_DEVINFO_MAX_ENTRIES * NV_FIFO_DEVINFO_ENTRY_SIZE) /* 3212 */

/* Индексы в engineData[16] (g_kernel_fifo_nvoc.h ENGINE_INFO_TYPE). */
#define ENGINE_INFO_TYPE_ENG_DESC          0u
#define ENGINE_INFO_TYPE_RM_ENGINE_TYPE    2u
#define ENGINE_INFO_TYPE_RUNLIST           3u
#define ENGINE_INFO_TYPE_RUNLIST_PRI_BASE 11u

/* RM_ENGINE_TYPE (gpu_engine_type.h); совпадает с NV2080_ENGINE_TYPE (унифицированы). */
#define RM_ENGINE_TYPE_NULL    0x00u
#define RM_ENGINE_TYPE_GR0     0x01u
#define RM_ENGINE_TYPE_COPY0   0x09u
#define RM_ENGINE_TYPE_COPY9   0x12u
#define RM_ENGINE_TYPE_SW      0x2cu

/* Один распарсенный движок. */
typedef struct {
    uint32_t eng_desc;         /* ENG_DESC */
    uint32_t rm_engine_type;   /* RM_ENGINE_TYPE (== engineType для канала) */
    uint32_t runlist;          /* RUNLIST id */
    uint32_t runlist_pri_base; /* RUNLIST_PRI_BASE */
} nv_gsp_fifo_engn;

typedef struct {
    uint32_t count;
    nv_gsp_fifo_engn engines[NV_FIFO_DEVINFO_MAX_ENTRIES];
} nv_gsp_fifo_devinfo;

/*
 * Проход A0: прочитать таблицу движков (baseIndex=0) на hSubdevice, разобрать
 * engineData каждого движка (ENG_DESC/RM_ENGINE_TYPE/RUNLIST/PRI_BASE) в *out.
 * *status ← статус RM. Порт r535_fifo_runl_ctor (FIFO_GET_DEVICE_INFO_TABLE).
 */
int nv_gsp_fifo_get_device_info(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hSubdevice,
                                nv_gsp_fifo_devinfo *out, uint32_t *status);

/* Найти первый движок с заданным RM_ENGINE_TYPE (напр. COPY0). Возврат индекс или -1. */
int nv_gsp_fifo_find_engine(const nv_gsp_fifo_devinfo *di, uint32_t rm_engine_type);

/*
 * Проход A2: аллокация канала GPFIFO (AMPERE_CHANNEL_GPFIFO_A) под device.
 * Строит NV_CHANNEL_ALLOC_PARAMS из cfg, шлёт GSP_RM_ALLOC. hObject=0xf1f00000|chid.
 * *out_channel ← хэндл, *status ← статус RM (NV_OK==0). Порт r535_chan_ramfc_write.
 */
int nv_gsp_rm_channel_alloc(nv_gsp_rpc_chan *ch, const nv_gsp_chan_cfg *cfg,
                            uint32_t *out_channel, uint32_t *status);

/* BIND: привязать канал к движку engineType (NVA06F_CTRL_CMD_BIND). */
int nv_gsp_rm_channel_bind(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hChannel,
                           uint32_t engineType, uint32_t *status);

/* SCHEDULE: включить/выключить канал в runlist (NVA06F_CTRL_CMD_GPFIFO_SCHEDULE). */
int nv_gsp_rm_channel_schedule(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hChannel,
                               int enable, uint32_t *status);

/*
 * Проход B: аллокация объекта движка на канале (hParent=канал). Напр. copy-engine
 * AMPERE_DMA_COPY_B (0xC7B5). params обычно пустые (0б). *out_obj ← хэндл.
 */
int nv_gsp_rm_engine_obj_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hChannel,
                               uint32_t hObject, uint32_t engineClass, uint32_t *status);

#endif /* RTXMACOC_GSP_FIFO_H */
