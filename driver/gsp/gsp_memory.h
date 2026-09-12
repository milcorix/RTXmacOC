/* Раскрой принадлежащей драйверу VRAM для одного внешнего VMM. */
#ifndef RTXMACOC_GSP_MEMORY_H
#define RTXMACOC_GSP_MEMORY_H
#include "gmmu.h"
#include "gsp_rm.h"
#include "vram.h"

#define NV_GSP_SERVICE_BYTES 0x100000ull /* кольцо, pushbuffer, semaphore + запас */
#define NV_GSP_CHANNEL_BYTES 0x10000ull /* instance, USERD, method buffer */
#define NV_GSP_APP_VA_BASE 0x20000000ull

typedef struct {
    nv_vram_span owned;
    nv_gmmu_range tables;
    uint64_t service_phys, app_phys, app_va, app_bytes, channel_phys;
} nv_gsp_memory_layout;

/* Пользовательская память отдельно от всех служебных страниц. Функция только
   считает раскрой: доступность подтверждается после GMMU+SET_PAGE_DIRECTORY
   и выполнения GPU-команд. extra — область VRAM консоли платформы, если есть.
   Низкие 512 МиБ исключены под существующий display bring-up и firmware GOP. */
int nv_gsp_memory_plan(const nv_gsp_static_info *si, uint64_t app_bytes,
                       const nv_vram_span *extra, nv_gsp_memory_layout *out);
#endif
