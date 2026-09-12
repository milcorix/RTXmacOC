/* Общий ABI kext/приложения: только фиксированные целые, без заголовков IOKit. */
#ifndef MILCORIX_ABI_H
#define MILCORIX_ABI_H
#include <stdint.h>
#include <stddef.h>
#define MILCORIX_CONNECT_TYPE 0x4D4C4358u
#define MILCORIX_MEMORY_ABI_VERSION 1u
#define MILCORIX_MAX_XFER (4u * 1024u * 1024u)
#define MILCORIX_MEMORY_READY 1u
#define MILCORIX_MEMORY_EXTERNAL_VMM 2u
#define MILCORIX_MEMORY_GPU_PROBES 4u
enum {
    kMilcorixMethodGetInfo = 0,
    kMilcorixMethodWrite = 1, kMilcorixMethodRead = 2, kMilcorixMethodCopy = 3,
    kMilcorixMethodGetMemoryInfo = 4,
    kMilcorixMethodAlloc = 5,       /* in: bytes, alignment; out: handle, actualBytes */
    kMilcorixMethodFree = 6,        /* in: handle */
    kMilcorixMethodWriteBuffer = 7, /* in: handle, offset; structure: bytes */
    kMilcorixMethodReadBuffer = 8,  /* in: handle, offset; structure output: bytes */
    kMilcorixMethodCopyBuffer = 9,  /* in: srcHandle,srcOff,dstHandle,dstOff,bytes; out: ns */
    kMilcorixMethodCount
};
typedef struct {
    uint32_t ready, channel, copy_engine, reserved;
    uint64_t scratch_size;
} MilcorixGpuInfo;
typedef struct {
    uint32_t version, size, flags, page_size;
    uint64_t pool_bytes, allocated_bytes, max_transfer_bytes;
} MilcorixMemoryInfo;
/* Одинаковая раскладка при компиляции C-утилиты и C++-kext. */
#ifdef __cplusplus
static_assert(sizeof(MilcorixGpuInfo) == 24 && sizeof(MilcorixMemoryInfo) == 40,
              "Milcorix ABI size");
static_assert(offsetof(MilcorixMemoryInfo, pool_bytes) == 16 &&
              offsetof(MilcorixMemoryInfo, max_transfer_bytes) == 32, "Milcorix ABI offsets");
#else
_Static_assert(sizeof(MilcorixGpuInfo) == 24 && sizeof(MilcorixMemoryInfo) == 40,
               "Milcorix ABI size");
_Static_assert(offsetof(MilcorixMemoryInfo, pool_bytes) == 16 &&
               offsetof(MilcorixMemoryInfo, max_transfer_bytes) == 32, "Milcorix ABI offsets");
#endif
/* Буферы живут до Free или закрытия соединения, которым они созданы.
   Выделение очищает ресурс перед выдачей handle. Операции синхронные: успешный
   Copy означает проверенный fence; после таймаута новый доступ не разрешается.
   Read/Write offset кратен 4; длина 1..MAX_XFER. Alloc — страницы 4К, alignment
   задаёт GPU VA. Это ресурс VRAM, не физическая RAM и не CPU-апертура. */
#endif
