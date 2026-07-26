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
    uint64_t fb_phys;   /* физ-адрес scanout-FB (VRAM-offset либо ФИЗ. адрес хоста —
                           см. fb_sysmem: это разные адресные пространства!) */
    uint32_t width;     /* активные пиксели по горизонтали */
    uint32_t height;    /* активные пиксели по вертикали */
    uint32_t pitch;     /* байт на строку (width*4, X8R8G8B8) */
    uint32_t fb_target; /* апертура ctx-dma (NV_CTXDMA_TARGET_*), из которой
                           дисплей реально читает поверхность */
    int      ok;        /* 1 — modeset+scanout запрограммированы */
    uint8_t  edid[128]; /* EDID активного монитора (блок 0), если edid_ok */
    int      edid_ok;
} nv_gsp_scanout_t;

/*
 * Живой контекст исполнения на GPU, остающийся ПОСЛЕ bring-up'а.
 *
 * Слой 4 создаёт канал GPFIFO, привязывает его к движку копирования и исполняет
 * первую команду. Раньше всё это было одноразовой проверкой — состояние
 * создавалось и терялось. Здесь оно отдаётся наружу, чтобы платформа могла
 * отправлять СВОЮ работу, не переписывая путь, уже проверенный на железе.
 *
 * Раскладка замапленного региона [ring_va, ring_va+region_size):
 *     +0x0000  кольцо GPFIFO (ring_entries записей по 8 байт)
 *     +0x1000  пушбуфер
 *     +0x2000  семафор завершения
 *     +0x3000  свободно — отдано под данные (scratch_*)
 *
 * Регион имеет и GPU-VA, и физический адрес во VRAM: команды адресуются по VA,
 * а CPU дотягивается до тех же байт через окно PRAMIN по физическому адресу.
 */
typedef struct {
    int      ok;             /* 1 — канал создан, привязан, запланирован и исполнил команду */
    uint32_t h_client, h_device, h_vaspace, h_channel, h_ce;
    uint32_t chid, runlist;  /* token дверного звонка = (runlist<<16)|chid */
    uint32_t engine_type;    /* RM_ENGINE_TYPE движка канала */

    uint64_t ring_va,  ring_phys;    /* кольцо GPFIFO */
    uint32_t ring_entries;
    uint64_t pb_va,    pb_phys;      /* пушбуфер */
    uint64_t sem_va,   sem_phys;     /* семафор завершения */
    uint64_t userd_phys;             /* USERD (там GP_PUT) */

    uint64_t scratch_va, scratch_phys, scratch_size;  /* свободная область под данные */
} nv_gsp_gpu_ctx_t;

/*
 * Провайдер scanout-фреймбуфера. Платформа решает, ГДЕ живёт FB, потому что от
 * этого зависит, сможет ли ОС отдать его своему композитору:
 *
 *  - Linux-стенд (dbg-прогон) провайдера НЕ даёт (NULL) → FB берётся во VRAM по
 *    фиксированному адресу и заливается через PRAMIN. Так снят слой 5 на железе.
 *  - macOS-kext ОБЯЗАН дать провайдера с sysmem=1: WindowServer'у нужен обычный
 *    CPU-писабельный буфер, а VRAM за 1-МиБ окном PRAMIN для этого непригоден.
 *    Дисплей тогда читает поверхность прямо из системной памяти (ctx-dma
 *    TARGET=PCI, когерентно).
 *
 * alloc() вызывается ОДИН раз, уже после разбора EDID (то есть под конкретный
 * режим), до постройки ctx-dma. Возврат 0 — успех.
 *   out_gpu_addr — адрес для дескриптора ctx-dma: смещение во VRAM либо
 *                  физический адрес хоста — что именно, задаёт out_target;
 *   out_target   — апертура ctx-dma (NV_CTXDMA_TARGET_VRAM / _SYSMEM);
 *   out_cpu_va   — CPU-адрес того же буфера (для очистки экрана). Для VRAM это
 *                  указатель в окно BAR1, для sysmem — обычная память.
 *                  Может остаться NULL: тогда core зальёт FB через PRAMIN.
 *
 * ВАЖНО: адрес CPU-апертуры, которую платформа отдаёт своей ОС, core НЕ
 * возвращает и знать не должен — платформа хранит его сама. Это намеренно:
 * так VRAM-адрес физически не может утечь в апертуру ОС.
 */
typedef struct {
    void *ctx;
    int (*alloc)(void *ctx, uint32_t w, uint32_t h, uint32_t pitch,
                 uint64_t *out_gpu_addr, uint32_t *out_target, void **out_cpu_va);
    int skip_modeset;  /* 1 — НЕ трогать вывод: перечислить дисплеи и выйти.
                          Нужно, чтобы отделить «GSP поднялся» от «мы сломали
                          картинку»: на этой стадии экран остаётся за EFI-FB. */
} nv_gsp_fb_provider_t;

/*
 * Полный bring-up GSP-RM: FWSEC-FRTS → staging → Booter → RPC → слои 3-5.
 *   io   — доступ к BAR0 (+ .log для трассировки, .udelay для задержек);
 *   ar   — DMA-арена ≥ NV_DMA_ARENA_SIZE, физически адресуемая GPU;
 *   pci  — физ. BAR/PCI-id (может быть NULL);
 *   dbg  — дамп-колбэк И признак «диагностический прогон»: если !=NULL, bring-up
 *          дампит логи и держит длинные паузы «разглядеть монитор» (Linux-стенд).
 *          kext передаёт NULL — без дампов и без секундных задержек scanout;
 *   scan — (опц., может быть NULL) заполняется геометрией запрограммированного
 *          scanout-FB (см. nv_gsp_scanout_t);
 *   fbp  — (опц., может быть NULL) провайдер scanout-FB (см. nv_gsp_fb_provider_t).
 *          NULL → историческое поведение: FB во VRAM + заливка через PRAMIN;
 *   gpu  — (опц., может быть NULL) заполняется живым контекстом исполнения
 *          (см. nv_gsp_gpu_ctx_t), чтобы платформа могла слать свою работу.
 * Возврат: 0 — GSP-RM загружен и RISC-V active; <0 — провал (см. io->log).
 */
int nv_gsp_bringup(const nv_mmio_t *io, nv_dma_arena_t *ar,
                   const nv_gsp_pci_info_t *pci, const nv_gsp_debug_t *dbg,
                   nv_gsp_scanout_t *scan, const nv_gsp_fb_provider_t *fbp,
                   nv_gsp_gpu_ctx_t *gpu);

#endif /* RTXMACOC_GSP_BRINGUP_H */
