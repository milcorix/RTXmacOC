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

#endif /* RTXMACOC_GSP_DISP_H */
