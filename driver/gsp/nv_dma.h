/*
 * nv_dma.h — переносимая DMA-арена для bring-up GSP (слои 2–5).
 *
 * Весь bring-up (staging GSP-RM, radix3, WprMeta, libos-логи, RPC shm, дисплейные
 * пушбуферы) требует физически адресуемой хостом памяти, которую видит и GPU.
 * На Linux/VFIO её давала одна DMA-арена (VA↔IOVA линейно, IOVA выбирали мы).
 * В macOS-kext IOMMU выключен (`DisableIoMapper=true` в OpenCore) → IOVA = физадрес,
 * значит нужен ОДИН физически непрерывный буфер, и его физадрес = адрес для GPU.
 *
 * Абстракция: платформа выделяет буфер и заполняет nv_dma_arena_t тройкой
 * (va, dma_addr, size). Дальше весь код бампает арену через nv_dma_alloc() —
 * это чистая арифметика, одинаковая на всех платформах. Так оркестратор bring-up
 * перестаёт зависеть от VFIO/IOKit и переносится 1:1.
 *
 * Платформенные реализации выделения (НЕ здесь):
 *   Linux (стенд): mmap + VFIO_IOMMU_MAP_DMA (tools/gsp_boot_linux.c);
 *   macOS (kext):  IOBufferMemoryDescriptor (contiguous, physmask) — driver/macos.
 */
#ifndef RTXMACOC_NV_DMA_H
#define RTXMACOC_NV_DMA_H

#include <stdint.h>
#include <stddef.h>

/* Размер DMA-арены под весь bring-up: fwimage GSP-RM ~36 МиБ + radix3 +
   bootloader + libos-логи + RPC shm + дисплейные структуры. 64 МиБ с запасом
   (= ARENA_SIZE в tools/gsp_boot_linux.c). */
#define NV_DMA_ARENA_SIZE  (64u * 1024u * 1024u)

/*
 * DMA-арена: непрерывный буфер, доступный CPU по va и GPU по dma_addr.
 * off — текущее смещение бампа, cap — ёмкость (= size). При IOMMU=off
 * dma_addr — физический адрес; при включённом IOMMU — IOVA.
 */
typedef struct {
    uint8_t  *va;        /* CPU-адрес начала буфера */
    uint64_t  dma_addr;  /* адрес того же начала для GPU (phys или IOVA) */
    uint64_t  off;       /* текущее смещение бамп-аллокатора */
    uint64_t  cap;       /* ёмкость буфера (байт) */
} nv_dma_arena_t;

/* Инициализировать арену уже выделенным платформой буфером. */
static inline void nv_dma_arena_init(nv_dma_arena_t *a, uint8_t *va,
                                     uint64_t dma_addr, uint64_t cap)
{
    a->va = va; a->dma_addr = dma_addr; a->off = 0; a->cap = cap;
}

/*
 * Выделить sz байт из арены с выравниванием 4К (как arena_alloc в Linux-стенде).
 * При успехе кладёт CPU-адрес в *out_va, GPU-адрес в *out_dma и возвращает 0.
 * При переполнении возвращает -1 (буфер не тронут).
 */
static inline int nv_dma_alloc(nv_dma_arena_t *a, uint64_t sz,
                               uint8_t **out_va, uint64_t *out_dma)
{
    uint64_t o = a->off;
    uint64_t next = (o + sz + 0xFFFu) & ~(uint64_t)0xFFFu;  /* выравнивание 4К */
    if (next > a->cap) return -1;
    a->off = next;
    if (out_va)  *out_va  = a->va + o;
    if (out_dma) *out_dma = a->dma_addr + o;
    return 0;
}

/* Сбросить арену для повторного использования (не освобождает буфер). */
static inline void nv_dma_arena_reset(nv_dma_arena_t *a) { a->off = 0; }

#endif /* RTXMACOC_NV_DMA_H */
