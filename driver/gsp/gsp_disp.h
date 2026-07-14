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

#endif /* RTXMACOC_GSP_DISP_H */
