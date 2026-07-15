/*
 * gsp_disp.h — слой 5: дисплей через GSP-RM (энумерация, modeset).
 *
 * После слоёв 2–4 поднимаем дисплейный движок: объект NV04_DISPLAY_COMMON (0x0073)
 * для контролов NV0073_*, читаем число heads и маску поддерживаемых дисплеев, затем
 * (5B) коннекторы/EDID, (5C) display root класс + modeset + scanout.
 *
 * Эталон (пересказано для лицензии): nouveau nvkm/engine/disp/r535.c
 * (r535_disp_oneinit). Контролы — nvrm 535.113.01 ctrl0073system.h. Классы —
 * nvif/class.h (NV04_DISPLAY_COMMON=0x0073, AD102_DISP=0xC770). Тех-запись:
 * docs/gsp-layer5-display.md.
 */
#ifndef RTXMACOC_GSP_DISP_H
#define RTXMACOC_GSP_DISP_H

#include <stdint.h>
#include "gsp_rm.h"

/* --- Классы дисплея (nvif/class.h) --- */
#define NV04_DISPLAY_COMMON        0x00000073u   /* objcom для NV0073_* контролов */
#define AD102_DISP                 0x0000c770u   /* display root класс (Ada, 5C) */

#define NV_GSP_RM_DISPCOMMON_HANDLE 0x00730000u  /* хэндл NV04_DISPLAY_COMMON (как nouveau) */
#define NV_GSP_RM_DISPROOT_HANDLE   0xc7700000u  /* AD102_DISP << 16 (display root, 5C) */

/* --- 5C.1: instance-mem дисплея + display root --- */
/* NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM (ctrl2080internal.h) на ВНУТРЕННИЙ
   subdevice GSP (hInternalSubdevice из GET_STATIC_INFO). Прописывает RAMIN дисплея. */
#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM  0x20800a49u
/* params (24б): instMemPhysAddr@0(u64) instMemSize@8(u64) instMemAddrSpace@16 cacheAttr@20. */
#define NV_DISP_WRINST_PARAMS_SIZE   24u
#define NV_DISP_WRINST_PHYS_OFF       0u
#define NV_DISP_WRINST_SIZE_OFF       8u
#define NV_DISP_WRINST_ADDRSPACE_OFF 16u
#define NV_DISP_WRINST_CACHEATTR_OFF 20u
#define NV_RM_ADDR_FBMEM             2u   /* ADDR_FBMEM (VRAM), как memdesc VIDMEM */
#define NV_MEMORY_WRITECOMBINED      2u   /* nv_memory_type.h */
#define NV_DISP_INST_SIZE            0x10000u  /* RAMIN дисплея = 64 КиБ */

/* --- 5C.2: каналы дисплея --- */
#define AD102_DISP_CORE_CHANNEL_DMA  0x0000c77du  /* core channel (Ada) */
#define GA102_DISP_WINDOW_CHANNEL_DMA 0x0000c67eu /* window channel (Ada reuse GA102) */
#define GA102_DISP_CURSOR            0x0000c67au  /* cursor (Ada reuse GA102) */
#define GA102_DISP_WINDOW_IMM_CHANNEL_DMA 0x0000c67bu /* window IMM (Ada reuse GA102) */
#define NV_GSP_RM_DISPCORE_HANDLE    0xc77d0000u  /* AD102_DISP_CORE_CHANNEL_DMA<<16 | head0 */
#define NV_GSP_RM_DISPWIN_HANDLE     0xc67e0000u  /* GA102_DISP_WINDOW_CHANNEL_DMA<<16 | win0 */
/* nv_gsp_disp_core_channel_alloc/_channel_pushbuffer — generic (годятся и для window
   channel GA102_DISP_WINDOW_CHANNEL_DMA: тот же r535_dmac_init путь). */

/* NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER (ctrl2080internal.h, на внутр.
   subdevice GSP). params (40б): addressSpace@0 physicalAddr@8(u64) limit@16(u64)
   cacheSnoop@24 hclass@28 channelInstance@32 valid@36(NvBool). */
#define NV2080_CTRL_CMD_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER  0x20800a58u
#define NV_DISP_PB_PARAMS_SIZE       40u
#define NV_DISP_PB_ADDRSPACE_OFF      0u
#define NV_DISP_PB_PHYS_OFF           8u
#define NV_DISP_PB_LIMIT_OFF         16u
#define NV_DISP_PB_CACHESNOOP_OFF    24u
#define NV_DISP_PB_HCLASS_OFF        28u
#define NV_DISP_PB_CHANINST_OFF      32u
#define NV_DISP_PB_VALID_OFF         36u
#define NV_DISP_PB_SIZE          0x1000u  /* пушбуфер core-channel ≤ 4 КиБ */

/* NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS (nvos.h, 32б): channelInstance@0
   hObjectBuffer@4 hObjectNotify@8 offset@12 pControl@16(u64) flags@24. */
#define NV50VAIO_CHANDMA_PARAMS_SIZE  32u
#define NV50VAIO_CHANDMA_CHANINST_OFF  0u
#define NV50VAIO_CHANDMA_OFFSET_OFF   12u

/* --- 5C.4: методы core-channel (NVC37D/NVC77D DMA) --- */
/* DMA-слово метода: opcode[31:29] (METHOD=0), count[27:18], methodOffset[13:2]=addr>>2.
   За заголовком идёт count слов данных. PUT@0/GET@4 — регистры user-региона канала. */
#define NVC37D_DMA_OPCODE_METHOD   0u
#define NVC37D_CORE_PUT_OFF        0x0u   /* user-регион core-channel: PUT */
#define NVC37D_CORE_GET_OFF        0x4u   /* GET */
#define NVC37D_UPDATE              0x0200u   /* данные: 0x1 | SPECIAL_HANDLING NONE | INHIBIT_INTERRUPTS FALSE */
#define NVC37D_UPDATE_DATA         0x00000001u
#define NVC77D_HEAD_SET_RASTER_SIZE(a)  (0x00002064u + (a)*0x400u)  /* WIDTH[14:0]|HEIGHT[30:16] */
/* User-регион core-channel в BAR0 (r535_chan_user: core → 0x680000, size 0x10000).
   PUT@offset0 регистр (r535_core_fini читает suspend_put из 0x680000), в DWORD-единицах.
   Window user-регион: 0x690000 + head*0x1000 (r535_chan_user case 0x7e). */
#define NVC77D_CORE_USER_BASE      0x00680000u
#define NVC37E_WINDOW_USER_BASE    0x00690000u   /* + head*0x1000 */
#define NVC37D_CHAN_PUT_OFF        0x0u          /* PUT (DWORD offset потока), MMIO в user-регион */

/* --- 5C.4c: доп. методы core-channel head-modeset (clc37d.h, сверено) --- */
#define NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY(a)      (0x0000200Cu + (a)*0x400u)  /* HERTZ[30:0]=kHz*1000 */
#define NVC37D_HEAD_SET_PIXEL_CLOCK_FREQUENCY_MAX(a)  (0x00002028u + (a)*0x400u)
#define NVC37D_HEAD_SET_HEAD_USAGE_BOUNDS(a)          (0x00002030u + (a)*0x400u)
#define NVC37D_HEAD_SET_VIEWPORT_POINT_IN(a)          (0x00002048u + (a)*0x400u)
#define NVC37D_HEAD_SET_RASTER_VERT_BLANK2(a)         (0x00002074u + (a)*0x400u)  /* blank2e<<16|blank2s (progressive: 0<<16|1) */
/* HEAD_USAGE_BOUNDS: CURSOR W256_H256=4[2:0], OUTPUT_LUT 1025=2[5:4], UPSCALING_ALLOWED=1[8:8]. */
#define NVC37D_HEAD_USAGE_BOUNDS_DEFAULT   (4u | (2u << 4) | (1u << 8))  /* 0x124 */
/* HEAD_SET_CONTROL(0x2008): STRUCTURE[1:0] PROGRESSIVE=0. */
#define NVC37D_HEAD_CONTROL_PROGRESSIVE    0u

/* --- 5C.4d: core-level методы (channel-scope, clc37d.h, сверено) --- */
#define NVC37D_SET_CONTEXT_DMA_NOTIFIER    0x00000208u  /* HANDLE[31:0] */
#define NVC37D_SET_INTERLOCK_FLAGS         0x00000218u  /* INTERLOCK_WITH_CURSOR(i) bit i; WITH_CORE bit16 */
#define NVC37D_SET_WINDOW_INTERLOCK_FLAGS  0x0000021Cu  /* INTERLOCK_WITH_WINDOW(i) bit i */
#define NVC37D_WINDOW_SET_WINDOW_FORMAT_USAGE_BOUNDS(a)         (0x00001004u + (a)*0x80u)
#define NVC37D_WINDOW_SET_WINDOW_ROTATED_FORMAT_USAGE_BOUNDS(a) (0x00001008u + (a)*0x80u)
#define NVC37D_WINDOW_SET_WINDOW_USAGE_BOUNDS(a)               (0x00001010u + (a)*0x80u)
#define NVC37D_WINDOW_SET_CONTROL_OWNER(a)                     (0x00001000u + (a)*0x80u)  /* OWNER[3:0]=HEAD(i) */
/* FORMAT_USAGE_BOUNDS: RGB_PACKED 1/2/4/8BPP[0..3] + YUV_PACKED422[4] = 0x1F (как corec37d_init). */
#define NVC37D_WIN_FORMAT_USAGE_DEFAULT    0x0000001Fu
/* USAGE_BOUNDS: MAX_PIXELS_FETCHED_PER_LINE[14:0]=0x7fff | INPUT_LUT 1025=2<<16 | INPUT_SCALER_TAPS TAPS_2=1<<20. */
#define NVC37D_WIN_USAGE_BOUNDS_DEFAULT    (0x7fffu | (2u << 16) | (1u << 20))  /* 0x127fff */
#define NVC37D_INTERLOCK_WITH_CORE_BIT     (1u << 16)   /* в SET_INTERLOCK_FLAGS core */

/* --- 5C.4d: window-channel класс NVC37E (GA102_DISP_WINDOW_CHANNEL_DMA, clc37e.h) --- */
#define NVC37E_UPDATE                  0x00000200u   /* данные: 0x1 (| INTERLOCK_WITH_WIN_IMM) */
#define NVC37E_SET_PRESENT_CONTROL     0x00000308u   /* MIN_PRESENT_INTERVAL[3:0]|BEGIN_MODE[6:4] NON_TEARING=0 */
#define NVC37E_SET_SIZE                0x00000224u   /* WIDTH[15:0]|HEIGHT[31:16] */
#define NVC37E_SET_STORAGE             0x00000228u   /* BLOCK_HEIGHT[3:0]|MEMORY_LAYOUT[4:4] (PITCH=1) */
#define NVC37E_SET_PARAMS              0x0000022Cu   /* FORMAT[7:0]|COLOR_SPACE[9:8]|INPUT_RANGE[13:12] */
#define NVC37E_SET_PLANAR_STORAGE(b)   (0x00000230u + (b)*0x4u)  /* PITCH[12:0]=pitch>>6 */
#define NVC37E_SET_CONTEXT_DMA_ISO(b)  (0x00000240u + (b)*0x4u)  /* HANDLE[31:0] */
#define NVC37E_SET_OFFSET(b)           (0x00000260u + (b)*0x4u)  /* ORIGIN[31:0]=offset>>8 */
#define NVC37E_SET_POINT_IN(b)         (0x00000290u + (b)*0x4u)  /* X[15:0]|Y[31:16] */
#define NVC37E_SET_SIZE_IN             0x00000298u   /* WIDTH[14:0]|HEIGHT[30:16] */
#define NVC37E_SET_SIZE_OUT            0x000002A4u   /* WIDTH[14:0]|HEIGHT[30:16] */
#define NVC37E_SET_INTERLOCK_FLAGS     0x00000370u   /* WITH_CORE[0:0]|WITH_CURSOR(i)[i+1] */
#define NVC37E_SET_WINDOW_INTERLOCK_FLAGS 0x00000374u
#define NVC37E_STORAGE_MEMORY_LAYOUT_PITCH  (1u << 4)
#define NVC37E_PARAMS_FORMAT_X8R8G8B8   0xE6u   /* dword A/X,R,G,B (наш FB: 0x00RRGGBB) */
#define NVC37E_PARAMS_FORMAT_A8R8G8B8   0xCFu
#define NVC37E_INTERLOCK_WITH_CORE_BIT  (1u << 0)   /* в NVC37E SET_INTERLOCK_FLAGS */

/* --- 5C.4d: ctx-dma (NV_DMA_IN_MEMORY) для SET_CONTEXT_DMA_ISO/NOTIFIER --- */
#define NV_DMA_IN_MEMORY               0x0000003du   /* nvif/class.h */
/* Дескриптор ctx-dma Volta+ (gv100_dmaobj_bind, 24б, 16-align, в inst-mem дисплея):
   @0x00 flags0; @0x04 start>>8 lo; @0x08 start>>8 hi; @0x0c limit>>8 lo; @0x10 limit>>8 hi. */
#define NV_CTXDMA_DESC_SIZE            24u
#define NV_CTXDMA_FLAGS0_VRAM          0x00000001u
#define NV_CTXDMA_FLAGS0_RW            0x00000004u
#define NV_CTXDMA_FLAGS0_PAGE_SP       0x00000040u   /* GF119_DMA_V0_PAGE_SP */
#define NV_CTXDMA_FLAGS0_VRAM_RW       (NV_CTXDMA_FLAGS0_VRAM | NV_CTXDMA_FLAGS0_RW | NV_CTXDMA_FLAGS0_PAGE_SP)  /* 0x45 */
/* RAMHT дисплея (nvkm_ramht): в первых 0x1000 inst-mem, запись 8б {handle@0, context@4};
   size=512 (bits=9). hash: XOR(handle по bits) ^ chid<<(bits-4). context = chid<<25 |
   (client&0x3fff) | (inst_offset << 9), где inst_offset — смещение дескриптора в inst-mem. */
#define NV_DISP_RAMHT_BITS             9u
#define NV_DISP_RAMHT_SIZE             512u
#define NV_DISP_RAMHT_ENTRY            8u
/* chid.user (disp_chan.c: user->user + id): core=0, window=1+head. */
#define NV_DISP_CHID_CORE              0u
#define NV_DISP_CHID_WINDOW(head)      (1u + (head))
/* Хэндлы ctx-dma (как nv50: HANDLE_SYNCBUF/HANDLE_VRAM), произвольные, ищутся в RAMHT. */
#define NV_DISP_HANDLE_SYNCBUF         0xcaf00001u
#define NV_DISP_HANDLE_VRAM            0xcaf00002u

/* Собрать DMA-заголовок метода (opcode METHOD, count слов данных). */
static inline uint32_t nv_disp_method_hdr(uint32_t method_addr, uint32_t count)
{ return ((NVC37D_DMA_OPCODE_METHOD) << 29) | ((count & 0x3ffu) << 18) | (method_addr & 0x3ffcu); }

/* Смещения методов core-channel (NVC37D, база Volta; head a, or a). */
#define NVC37D_HEAD_SET_PROCAMP(a)                 (0x00002000u + (a)*0x400u)
#define NVC37D_HEAD_SET_CONTROL_OUTPUT_RESOURCE(a) (0x00002004u + (a)*0x400u)
#define NVC37D_HEAD_SET_CONTROL_METH(a)            (0x00002008u + (a)*0x400u)
#define NVC37D_HEAD_SET_VIEWPORT_SIZE_IN(a)        (0x0000204Cu + (a)*0x400u)
#define NVC37D_HEAD_SET_VIEWPORT_SIZE_OUT(a)       (0x00002058u + (a)*0x400u)
#define NVC37D_HEAD_SET_RASTER_SYNC_END(a)         (0x00002068u + (a)*0x400u)
#define NVC37D_HEAD_SET_RASTER_BLANK_END(a)        (0x0000206Cu + (a)*0x400u)
#define NVC37D_HEAD_SET_RASTER_BLANK_START(a)      (0x00002070u + (a)*0x400u)
#define NVC37D_SOR_SET_CONTROL(a)                  (0x00000300u + (a)*0x20u)
#define NVC37D_WINDOW_SET_CONTROL(a)               (0x00001000u + (a)*0x80u)
/* поля HEAD_SET_CONTROL_OUTPUT_RESOURCE: HSYNC@2, VSYNC@3, PIXEL_DEPTH[7:4] (24_444=4). */
#define NVC37D_ORESOURCE_PIXEL_DEPTH_BPP_24_444    4u
/* SOR_SET_CONTROL: OWNER_MASK[7:0]=1<<head, PROTOCOL[11:8]. */
#define NVC37D_SOR_PROTOCOL_SINGLE_TMDS_A          1u
#define NVC37D_SOR_PROTOCOL_DP_A                   8u
#define NVC37D_SOR_PROTOCOL_DP_B                   9u

/* Детальный тайминг из EDID (DTD @54). */
typedef struct {
    uint32_t pclk_khz;                  /* пиксельклок, кГц */
    uint32_t hact, hblank, hsync_off, hsync_w;
    uint32_t vact, vblank, vsync_off, vsync_w;
    uint8_t  hsync_pos, vsync_pos;      /* полярность (1=positive) */
} nv_edid_timing;

/*
 * Распарсить первый Detailed Timing Descriptor из EDID (128+ байт) в *t.
 * Возврат 0 при валидном DTD (pclk!=0), -1 иначе. Стандартный декод EDID DTD.
 */
int nv_edid_parse_dtd(const uint8_t *edid, uint32_t edid_len, nv_edid_timing *t);

/*
 * 5C.4c: собрать поток методов core-channel head/OR-modeset в pb[*off] по таймингу t
 * для head/sor/protocol (БЕЗ финального UPDATE — его добавляет _build_core_update):
 * SOR_SET_CONTROL, HEAD_SET_PROCAMP, OUTPUT_RESOURCE, RASTER_SIZE/SYNC_END/BLANK_END/
 * BLANK_START/VERT_BLANK2, HEAD_SET_CONTROL(progressive), PIXEL_CLOCK_FREQUENCY(+MAX),
 * HEAD_USAGE_BOUNDS, VIEWPORT_POINT_IN/SIZE_IN/SIZE_OUT. Порт headc37d_mode/_or/_view.
 * Возврат: *off продвинут (15 методов).
 */
void nv_gsp_disp_build_core_modeset(uint8_t *pb, uint32_t *off, const nv_edid_timing *t,
                                    uint32_t head, uint32_t sor, uint32_t protocol);

/*
 * 5C.4d: core-channel init (порт corec37d_init + corec37d_wndw_owner). В pb[*off]:
 * SET_CONTEXT_DMA_NOTIFIER=notifier_handle; для 8 окон FORMAT_USAGE_BOUNDS+ROTATED(0)+
 * USAGE_BOUNDS; для 8 окон WINDOW_SET_CONTROL OWNER=HEAD(i>>1). БЕЗ UPDATE.
 */
void nv_gsp_disp_build_core_init(uint8_t *pb, uint32_t *off, uint32_t notifier_handle);

/*
 * 5C.4d: финальный UPDATE core-channel (порт corec37d_update, ntfy=false). В pb[*off]:
 * SET_INTERLOCK_FLAGS=0, SET_WINDOW_INTERLOCK_FLAGS=wndw_interlock_mask, UPDATE=0x1.
 * wndw_interlock_mask — битовая маска окон, с которыми core интерлочится (0=без интерлока).
 */
void nv_gsp_disp_build_core_update(uint8_t *pb, uint32_t *off, uint32_t wndw_interlock_mask);

/*
 * 5C.4d: поток методов window-channel (NVC37E) для surface-scanout (порт wndwc37e_image_set).
 * В pb[*off]: SET_PRESENT_CONTROL, SET_SIZE, SET_STORAGE(PITCH), SET_PARAMS(format,RGB),
 * SET_PLANAR_STORAGE(pitch>>6), SET_CONTEXT_DMA_ISO=iso_handle, SET_OFFSET(fb_off>>8),
 * SET_POINT_IN(0), SET_SIZE_IN, SET_SIZE_OUT. БЕЗ UPDATE. format — NVC37E_PARAMS_FORMAT_*.
 */
void nv_gsp_disp_build_window_image(uint8_t *pb, uint32_t *off, uint32_t format,
                                    uint32_t w, uint32_t h, uint32_t pitch,
                                    uint64_t fb_off, uint32_t iso_handle);

/*
 * 5C.4d: финальный UPDATE window-channel (порт wndwc37e_update). В pb[*off]:
 * SET_INTERLOCK_FLAGS=(interlock_with_core?WITH_CORE:0), SET_WINDOW_INTERLOCK_FLAGS=0,
 * UPDATE=0x1.
 */
void nv_gsp_disp_build_window_update(uint8_t *pb, uint32_t *off, int interlock_with_core);

/*
 * 5C.4d: закодировать 24-байтный дескриптор ctx-dma Volta+ (gv100_dmaobj_bind) в desc[24]
 * для диапазона VRAM [start..limit] (RDWR). Затем пишется в inst-mem дисплея через PRAMIN.
 */
void nv_gsp_disp_build_ctxdma_desc(uint8_t *desc, uint64_t start, uint64_t limit);

/*
 * 5C.4d: вычислить индекс RAMHT-слота (co) для (chid, handle) и context-слово.
 * inst_offset — смещение дескриптора ctx-dma в inst-mem (байты). client — RM client handle.
 * Возврат: *out_slot ← индекс записи (co), *out_context ← context-слово. Порт nvkm_ramht.
 */
void nv_gsp_disp_ramht_entry(uint32_t chid, uint32_t handle, uint32_t client,
                             uint32_t inst_offset, uint32_t *out_slot, uint32_t *out_context);

/* --- 5C.3: SOR acquire (ctrl0073dfp.h) --- */
#define NV0073_CTRL_CMD_DFP_ASSIGN_SOR   0x731152u
#define NV0073_ASSIGN_SOR_MAX_SORS       4u
/* NV0073_CTRL_DFP_ASSIGN_SOR_PARAMS (80б): subDeviceInstance@0 displayId@4
   sorExcludeMask@8(u8) slaveDisplayId@12 forceSublinkConfig@16 bIs2Head1Or@20(u8)
   sorAssignList[4]@24 sorAssignListWithTag[4]@40 ({displayMask,sorType} по 8б)
   reservedSorMask@72(u8) flags@76. */
#define NV0073_ASSIGN_SOR_PARAMS_SIZE      80u
#define NV0073_ASSIGN_SOR_DISPLAYID_OFF     4u
#define NV0073_ASSIGN_SOR_EXCLUDEMASK_OFF   8u
#define NV0073_ASSIGN_SOR_LISTWITHTAG_OFF  40u
#define NV0073_ASSIGN_SOR_INFO_STRIDE       8u   /* {displayMask@0, sorType@4} */
#define NV0073_ASSIGN_SOR_FLAGS_OFF        76u

/* --- Контролы NV0073 SYSTEM (ctrl0073system.h) --- */
#define NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS  0x730102u
#define NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED  0x730120u
/* NV0073_CTRL_SYSTEM_GET_NUM_HEADS_PARAMS (12б): subDeviceInstance@0 flags@4 numHeads@8. */
#define NV0073_NUM_HEADS_PARAMS_SIZE   12u
#define NV0073_NUM_HEADS_SUBDEV_OFF     0u
#define NV0073_NUM_HEADS_FLAGS_OFF      4u
#define NV0073_NUM_HEADS_NUMHEADS_OFF   8u
/* NV0073_CTRL_SYSTEM_GET_SUPPORTED_PARAMS (12б): subDeviceInstance@0 displayMask@4
   displayMaskDDC@8. */
#define NV0073_SUPPORTED_PARAMS_SIZE   12u
#define NV0073_SUPPORTED_SUBDEV_OFF     0u
#define NV0073_SUPPORTED_MASK_OFF       4u
#define NV0073_SUPPORTED_MASKDDC_OFF    8u

/* --- Контролы NV0073 5B: коннекторы / connect-state / EDID --- */
#define NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO       0x73028bu
#define NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE   0x730122u
#define NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2       0x730245u
/* OR_GET_INFO params (56б): subDeviceInstance@0 displayId@4 index@8 type@12 protocol@16
   ditherType@20 ditherAlgo@24 location@28 rootPortId@32 dcbIndex@36 vbiosAddress@40(u64)
   bIsLitByVbios@48 bIsDispDynamic@49. */
#define NV0073_OR_GET_INFO_PARAMS_SIZE   56u
#define NV0073_OR_GI_DISPLAYID_OFF        4u
#define NV0073_OR_GI_INDEX_OFF            8u
#define NV0073_OR_GI_TYPE_OFF            12u
#define NV0073_OR_GI_PROTOCOL_OFF       16u
#define NV0073_OR_GI_LOCATION_OFF       28u
/* OR type/protocol значения (ctrl0073specific.h). */
#define NV0073_OR_TYPE_NONE   0u
#define NV0073_OR_TYPE_DAC    1u
#define NV0073_OR_TYPE_SOR    2u
#define NV0073_OR_PROTOCOL_SOR_SINGLE_TMDS_A  1u
#define NV0073_OR_PROTOCOL_SOR_SINGLE_TMDS_B  2u
#define NV0073_OR_PROTOCOL_SOR_DUAL_TMDS      5u
#define NV0073_OR_PROTOCOL_SOR_DP_A           8u
#define NV0073_OR_PROTOCOL_SOR_DP_B           9u
/* CONNECT_STATE params (16б): subDeviceInstance@0 flags@4 displayMask@8(IN/OUT) retryTimeMs@12. */
#define NV0073_CONNECT_STATE_PARAMS_SIZE  16u
#define NV0073_CONNECT_STATE_FLAGS_OFF     4u
#define NV0073_CONNECT_STATE_MASK_OFF      8u
/* GET_EDID_V2 params (2064б): subDeviceInstance@0 displayId@4 bufferSize@8 flags@12
   edidBuffer[2048]@16. */
#define NV0073_EDID_MAX_BYTES         2048u
#define NV0073_EDID_V2_PARAMS_SIZE    (16u + NV0073_EDID_MAX_BYTES)  /* 2064 */
#define NV0073_EDID_DISPLAYID_OFF      4u
#define NV0073_EDID_BUFSIZE_OFF        8u
#define NV0073_EDID_FLAGS_OFF         12u
#define NV0073_EDID_BUFFER_OFF        16u

/*
 * 5A шаг 1: аллоцировать NV04_DISPLAY_COMMON (0x0073) под hDevice — объект для всех
 * контролов NV0073_*. hObject=0x00730000, params нет. *out_disp ← хэндл, *status ← статус.
 * Порт r535_disp_oneinit (nvkm_gsp_rm_alloc NV04_DISPLAY_COMMON).
 */
int nv_gsp_disp_common_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                             uint32_t *out_disp, uint32_t *status);

/* 5A шаг 2: число heads (SYSTEM_GET_NUM_HEADS) на объекте hDispCommon. */
int nv_gsp_disp_get_num_heads(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                              uint32_t *out_num_heads, uint32_t *status);

/* 5A шаг 3: маска поддерживаемых дисплеев (SYSTEM_GET_SUPPORTED). */
int nv_gsp_disp_get_supported(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                              uint32_t *out_display_mask, uint32_t *out_display_mask_ddc,
                              uint32_t *status);

/* 5B: инфо о выходном ресурсе (OR) для одного displayId — тип/протокол/индекс/локация. */
int nv_gsp_disp_or_get_info(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                            uint32_t displayId, uint32_t *out_type, uint32_t *out_protocol,
                            uint32_t *out_index, uint32_t *out_location, uint32_t *status);

/* 5B: connect-state для маски дисплеев (IN=что проверить, OUT=подключённые). */
int nv_gsp_disp_get_connect_state(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                                  uint32_t display_mask_in, uint32_t *out_connected_mask,
                                  uint32_t *status);

/* 5B: прочитать EDID одного displayId (DDC). *out_size ← число байт в buf (≤ buf_cap). */
int nv_gsp_disp_get_edid(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                         uint32_t displayId, uint8_t *buf, uint32_t buf_cap,
                         uint32_t *out_size, uint32_t *status);

/*
 * 5C.1 шаг 1: прописать RAMIN дисплея (WRITE_INST_MEM) на ВНУТРЕННЕМ subdevice GSP.
 * hIntClient/hIntSubdevice — hInternalClient/hInternalSubdevice из GET_STATIC_INFO.
 * inst_phys — физ. VRAM-адрес обнулённого 64 КиБ блока (addrSpace=FBMEM, WC).
 * Порт r535_disp_oneinit (INTERNAL_DISPLAY_WRITE_INST_MEM).
 */
int nv_gsp_disp_write_inst_mem(nv_gsp_rpc_chan *ch, uint32_t hIntClient, uint32_t hIntSubdevice,
                               uint64_t inst_phys, uint64_t inst_size, uint32_t *status);

/*
 * 5C.1 шаг 2: аллоцировать display root класс (напр. AD102_DISP=0xC770) под hDevice.
 * hObject = dispClass<<16, params нет. Порт r535_disp_init. *out_root ← хэндл.
 */
int nv_gsp_disp_root_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDevice,
                           uint32_t dispClass, uint32_t *out_root, uint32_t *status);

/*
 * 5C.2 шаг 1: зарегистрировать пушбуфер дисплей-канала (DISPLAY_CHANNEL_PUSHBUFFER)
 * на ВНУТРЕННЕМ subdevice GSP. pb_phys — физ. VRAM-адрес обнулённого пушбуфера
 * (≤4КиБ), hclass — класс канала (напр. AD102_DISP_CORE_CHANNEL_DMA), channelInstance
 * — инстанс (core=0). Порт r535_chan_push.
 */
int nv_gsp_disp_channel_pushbuffer(nv_gsp_rpc_chan *ch, uint32_t hIntClient,
                                   uint32_t hIntSubdevice, uint32_t hclass,
                                   uint32_t channelInstance, uint64_t pb_phys,
                                   uint64_t pb_limit, uint32_t *status);

/*
 * 5C.2 шаг 2: аллоцировать core-channel дисплея (AD102_DISP_CORE_CHANNEL_DMA) под
 * display root. hObject=(coreClass<<16)|channelInstance, params
 * NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS (channelInstance, offset=0). Порт
 * r535_dmac_init. *out_chan ← хэндл.
 */
int nv_gsp_disp_core_channel_alloc(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispRoot,
                                   uint32_t coreClass, uint32_t channelInstance,
                                   uint32_t *out_chan, uint32_t *status);

/*
 * 5C.3: назначить SOR подключённому displayId (DFP_ASSIGN_SOR). Нужно до modeset и
 * до DP link training. Читает sorAssignListWithTag[] и находит индекс SOR, чей
 * displayMask содержит наш displayId. *out_sor ← индекс SOR (0..3) или ~0. Порт
 * r535_outp_acquire.
 */
int nv_gsp_disp_assign_sor(nv_gsp_rpc_chan *ch, uint32_t hClient, uint32_t hDispCommon,
                           uint32_t displayId, uint32_t *out_sor, uint32_t *status);

/*
 * 5C.4 (фундамент): дописать один метод core-channel (заголовок+данные) в пушбуфер pb
 * по смещению *poff (в байтах, LE). Возврат: смещение продвинуто на 8. Кодирует
 * DMA-слово (NVC37D_DMA opcode METHOD) + слово данных. Для сборки потока modeset.
 */
void nv_gsp_disp_push_method(uint8_t *pb, uint32_t *poff, uint32_t method_addr, uint32_t data);

#endif /* RTXMACOC_GSP_DISP_H */
