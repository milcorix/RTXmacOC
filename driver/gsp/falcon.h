/*
 * falcon.h — примитивы микроконтроллера Falcon (GSP) для bring-up (слой 2).
 *
 * Реализует последовательности из nova-core поверх абстрактного MMIO + DMA,
 * чтобы один код работал в kext (IOPCIDevice + BAR0), dext (MemoryRead/Write) и
 * в тесте. Регистры — falcon_regs.h.
 *
 * Источники (пересказано для соответствия лицензии):
 *   falcon.rs            — load/boot/dma_wr/dma_load
 *   falcon/hal/ga102.rs  — reset_eng/select_core/program_brom (Ampere/Ada)
 *   regs.rs / gsp.rs     — регистры и база GSP
 *
 * DMA-источник: вызывающий выделяет coherent-буфер, копирует туда ucode
 * (imem||dmem) и передаёт сюда его DMA-адрес; модуль программирует движок.
 */
#ifndef RTXMACOC_FALCON_H
#define RTXMACOC_FALCON_H

#include <stdint.h>
#include "../../falcon_regs.h"

/* Абстракция доступа к BAR0 + задержка. */
typedef struct {
    void     *ctx;
    uint32_t (*rd)(void *ctx, uint32_t off);
    void     (*wr)(void *ctx, uint32_t off, uint32_t val);
    void     (*udelay)(void *ctx, uint32_t usec);
} nv_mmio_t;

/* Цель DMA-копирования в память Falcon (falcon.rs FalconMem). */
#define NV_FALCON_MEM_IMEM_SECURE    0
#define NV_FALCON_MEM_IMEM_NONSECURE 1
#define NV_FALCON_MEM_DMEM           2

/* Параметры одной DMA-загрузки (falcon.rs FalconDmaLoadTarget). */
typedef struct {
    uint32_t src_start; /* смещение в источнике (ucode-блобе) */
    uint32_t dst_start; /* смещение в памяти Falcon (IMEM/DMEM) */
    uint32_t len;       /* байт */
} nv_dma_load_target_t;

/* Коды возврата. */
#define NV_OK          0
#define NV_ERR_TIMEOUT (-1)
#define NV_ERR_STATE   (-2)

/* --- базовые примитивы (MMIO) --- */
void nv_falcon_reset_engine(const nv_mmio_t *io, uint32_t base);
int  nv_falcon_wait_mem_scrubbing(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us);
void nv_falcon_start(const nv_mmio_t *io, uint32_t base);
int  nv_falcon_wait_halted(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us);

void     nv_falcon_write_mailbox0(const nv_mmio_t *io, uint32_t base, uint32_t v);
void     nv_falcon_write_mailbox1(const nv_mmio_t *io, uint32_t base, uint32_t v);
uint32_t nv_falcon_read_mailbox0(const nv_mmio_t *io, uint32_t base);
uint32_t nv_falcon_read_mailbox1(const nv_mmio_t *io, uint32_t base);

int nv_falcon_boot(const nv_mmio_t *io, uint32_t base,
                   int set_mbox0, uint32_t mbox0,
                   int set_mbox1, uint32_t mbox1,
                   uint32_t timeout_us, uint32_t *out_mbox0);

/* --- GA102/Ada HAL (falcon/hal/ga102.rs) --- */

/* Выбрать ядро Falcon (а не RISC-V) на двухъядерном Peregrine. */
int  nv_falcon_select_core_ga102(const nv_mmio_t *io, uint32_t falcon2_base, uint32_t timeout_us);

/*
 * Полный reset для Ampere/Ada: ожидание reset_ready (необязательное), сброс
 * движка, ожидание скраба, выбор ядра Falcon, запись FALCON_RM = PMC_BOOT_0.
 * boot0 — значение PMC_BOOT_0 (offset 0x0 BAR0), как в falcon.rs reset().
 */
int  nv_falcon_reset_ga102(const nv_mmio_t *io, uint32_t base, uint32_t falcon2_base,
                           uint32_t boot0, uint32_t timeout_us);

/* Программирование Falcon Boot ROM под HS-подпись (RSA3K). */
void nv_falcon_program_brom_ga102(const nv_mmio_t *io, uint32_t falcon2_base,
                                  uint32_t pkc_data_offset, uint16_t engine_id_mask,
                                  uint8_t ucode_id);

/* --- DMA-загрузка ucode (falcon.rs dma_wr/dma_load) --- */

/* Сброс DMA-узла: FBIF_CTL.allow_phys_no_ctx=1, DMACTL=0. */
void nv_falcon_dma_reset(const nv_mmio_t *io, uint32_t base);

/*
 * Одна DMA-передача в IMEM/DMEM Falcon. dma_addr — DMA-адрес coherent-буфера с
 * ucode. target_mem — NV_FALCON_MEM_*. Возвращает NV_OK/NV_ERR_*.
 */
int nv_falcon_dma_wr(const nv_mmio_t *io, uint32_t base, uint64_t dma_addr,
                     int target_mem, const nv_dma_load_target_t *t, uint32_t timeout_us);

/*
 * Полная DMA-загрузка: dma_reset -> FBIF на coherent sysmem/physical ->
 * IMEM(secure) -> DMEM -> program_brom -> BOOTVEC=boot_addr.
 * imem/dmem — параметры из дескриптора FWSEC (V3: imem {0,imem_phys,imem_load},
 * dmem {imem_load,dmem_phys,dmem_load}).
 */
int nv_falcon_dma_load_ga102(const nv_mmio_t *io, uint32_t base, uint32_t falcon2_base,
                             uint64_t dma_addr,
                             const nv_dma_load_target_t *imem_sec,
                             const nv_dma_load_target_t *dmem,
                             uint32_t pkc_data_offset, uint16_t engine_id_mask,
                             uint8_t ucode_id, uint32_t boot_addr, uint32_t timeout_us);

/* --- проверки результата --- */
int nv_wpr2_is_set(const nv_mmio_t *io, uint64_t *out_lo, uint64_t *out_hi);
int nv_gfw_boot_completed(const nv_mmio_t *io);

#endif /* RTXMACOC_FALCON_H */
