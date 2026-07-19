/*
 * gsp_bringup.h — вход в переносимый оркестратор загрузки GSP-RM (слои 2–5).
 *
 * Реализация — gsp_bringup.c. Платформа (Linux-стенд / macOS-kext) готовит
 * MMIO-абстракцию, DMA-арену, PCI-инфо и (опц.) дамп-колбэк, затем зовёт
 * nv_gsp_bringup(). Прошивку оркестратор берёт через nv_fw_blob_get().
 */
#ifndef RTXMACOC_GSP_BRINGUP_H
#define RTXMACOC_GSP_BRINGUP_H

#include <stdint.h>
#include "falcon.h"
#include "nv_dma.h"

/* Физические адреса BAR-регионов и PCI-id устройства — для GspSystemInfo.
   Linux читает из sysfs, kext — из IOPCIDevice. Если NULL передан в bringup,
   sysinfo заполняется нулями (GSP-RM это переживает на нашем железе). */
typedef struct {
    uint64_t bar0;   /* физ. база BAR0 (регистры) */
    uint64_t bar1;   /* физ. база BAR1 */
    uint64_t bar3;   /* физ. база BAR3 */
    uint64_t devid;  /* (bus<<8)|(dev<<3)|fn — как в build_sysinfo */
} nv_gsp_pci_info_t;

/* Опциональный дамп libos-логов и shm для диагностики. Linux пишет .bin-файлы,
   kext может слить в системный лог или проигнорировать. dump==NULL → не дампить.
   name — "LOGINIT"/"LOGINTR"/"LOGRM"/"SHM". */
typedef struct {
    void *ctx;
    void (*dump)(void *ctx, const char *name, const uint8_t *data, uint32_t len);
} nv_gsp_debug_t;

/* Итог слоя 5, который bring-up реально запрограммировал в железо: адрес и
   геометрия scanout-FB (нативный режим из EDID). Kext берёт это для апертуры
   IOFramebuffer (WindowServer), чтобы не хардкодить размеры. ok=0 → modeset не
   состоялся (нет монитора/EDID) — поля не валидны. */
typedef struct {
    uint64_t fb_phys;   /* физ-адрес scanout-FB во VRAM */
    uint32_t width;     /* активные пиксели по горизонтали */
    uint32_t height;    /* активные пиксели по вертикали */
    uint32_t pitch;     /* байт на строку (width*4, X8R8G8B8) */
    int      ok;        /* 1 — modeset+scanout запрограммированы */
} nv_gsp_scanout_t;

/*
 * Полный bring-up GSP-RM: FWSEC-FRTS → staging → Booter → RPC → слои 3-5.
 *   io   — доступ к BAR0 (+ .log для трассировки, .udelay для задержек);
 *   ar   — DMA-арена ≥ NV_DMA_ARENA_SIZE, физически адресуемая GPU;
 *   pci  — физ. BAR/PCI-id (может быть NULL);
 *   dbg  — дамп-колбэк И признак «диагностический прогон»: если !=NULL, bring-up
 *          дампит логи и держит длинные паузы «разглядеть монитор» (Linux-стенд).
 *          kext передаёт NULL — без дампов и без секундных задержек scanout;
 *   scan — (опц., может быть NULL) заполняется геометрией запрограммированного
 *          scanout-FB (см. nv_gsp_scanout_t).
 * Возврат: 0 — GSP-RM загружен и RISC-V active; <0 — провал (см. io->log).
 */
int nv_gsp_bringup(const nv_mmio_t *io, nv_dma_arena_t *ar,
                   const nv_gsp_pci_info_t *pci, const nv_gsp_debug_t *dbg,
                   nv_gsp_scanout_t *scan);

#endif /* RTXMACOC_GSP_BRINGUP_H */
