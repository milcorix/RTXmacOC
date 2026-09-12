/*
 * vram.h — учёт VRAM, принадлежащей драйверу. Собственная платформенно-независимая
 * логика: не содержит регистров, RM RPC или допущений о CPU-апертуре.
 *
 * Владелец предоставляет проверенный свободный диапазон из карты GSP, исключив
 * firmware, scanout, каналы и page tables. Пул должен быть отображён в GPU VA
 * отдельно. Метаданные ниже сами по себе не доказывают доступность памяти GPU.
 */
#ifndef RTXMACOC_VRAM_H
#define RTXMACOC_VRAM_H

#include <stdint.h>

#define NV_VRAM_PAGE_BYTES 4096ull
#define NV_VRAM_MIN_APP_BYTES (1ull << 30)
#define NV_VRAM_MAX_ALLOCS 128u

typedef struct { uint64_t base, size; } nv_vram_span;
typedef struct { uint64_t handle, offset, size; } nv_vram_allocation;
typedef struct {
    uint64_t phys, va, size, used, next_handle;
    nv_vram_allocation allocations[NV_VRAM_MAX_ALLOCS];
} nv_vram_pool;

/* Подобрать один непрерывный диапазон, обходя все исключения (порядок любой).
   Размеры spans в байтах; границы полуоткрытые. Некорректные входные spans
   отвергаются целиком. 0 — найден, -1 — неверный вход, -2 — нет места. */
int nv_vram_find_range(const nv_vram_span *available, uint32_t count,
                       const nv_vram_span *excluded, uint32_t excluded_count,
                       uint64_t bytes, uint64_t alignment, nv_vram_span *out);

/* Вызывать один раз для нового пула. Все операции сериализует владелец
   (IOLock на macOS). init не предназначен для сброса занятого пула. */
int nv_vram_pool_init(nv_vram_pool *pool, uint64_t phys, uint64_t va, uint64_t size);
int nv_vram_alloc(nv_vram_pool *pool, uint64_t bytes, uint64_t alignment, uint64_t *handle);
/* Перед free владелец обязан дождаться всех GPU-команд с этой аллокацией.
   Перед передачей новой аллокации другому клиенту данные надо очистить. */
int nv_vram_free(nv_vram_pool *pool, uint64_t handle);
/* Разрешает диапазон внутри существующего ресурса. CPU-адрес НЕ возвращает:
   phys — только VRAM-offset для PRAMIN, va — адрес для команд GPU. */
int nv_vram_resolve(const nv_vram_pool *pool, uint64_t handle, uint64_t offset,
                    uint64_t bytes, uint64_t *phys, uint64_t *va);

#endif
