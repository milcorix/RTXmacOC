/*
 * gsp_gr.h — привилегированные запросы к GSP-RM, нужные для графического движка
 * и для обработки прерываний.
 *
 * Оба контрола из семейства `NV2080_CTRL_CMD_INTERNAL_*` и шлются НЕ на наш
 * RM-клиент, а на приватные хэндлы самого GSP (`hInternalClient` /
 * `hInternalSubdevice` из ответа GET_STATIC_INFO). Это не деталь оформления:
 * на наш клиент те же контролы приходят отказом, и наивная проверка «работает
 * ли compute» дала бы ложное «нет». Что путь через внутренние хэндлы у нас
 * открыт — уже доказано слоем 5: на них идут INTERNAL_DISPLAY_WRITE_INST_MEM и
 * INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER, и картинка на железе получается.
 *
 * Раскладки структур сверены компиляцией оригинальных заголовков NVIDIA
 * (open-gpu-kernel-modules, тег 535.113.01) и перекрёстно — с вендорированной
 * копией в nouveau.
 */
#ifndef RTXMACOC_GSP_GR_H
#define RTXMACOC_GSP_GR_H

#include <stdint.h>
#include "gsp_rm.h"

/* ---- Размеры контекстных буферов графического движка ----
 * NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO.
 * params: engineContextBuffersInfo[8], каждый — engine[25] пар {size, alignment}.
 * Итого 8*25*8 = 1600 байт.
 */
#define NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO 0x20800A32u
#define NV_GR_CTXBUF_PARAMS_SIZE      1600u
#define NV_GR_CTXBUF_MAX_ENGINES         8u
#define NV_GR_CTXBUF_MAX_IDS            25u
#define NV_GR_CTXBUF_ENGINE_STRIDE     200u   /* 25 * 8 */

typedef struct {
    uint32_t size;
    uint32_t alignment;
} nv_gsp_gr_ctxbuf;

typedef struct {
    nv_gsp_gr_ctxbuf buf[NV_GR_CTXBUF_MAX_IDS];
    uint32_t         count_nonzero;   /* сколько буферов имеют ненулевой размер */
} nv_gsp_gr_ctxbufs;

/*
 * Прочитать размеры/выравнивания контекстных буферов графического движка
 * gr_index (0 — основной GR). hIntClient/hIntSubdevice — ВНУТРЕННИЕ хэндлы GSP.
 * Возврат: NV_GSP_RM_OK и *status == 0 — данные валидны.
 */
int nv_gsp_gr_get_ctxbuf_info(nv_gsp_rpc_chan *ch, uint32_t hIntClient,
                              uint32_t hIntSubdevice, uint32_t gr_index,
                              nv_gsp_gr_ctxbufs *out, uint32_t *status);

/* Разбор ответа отдельно от транспорта — чтобы раскладку можно было проверить
   офлайн, без железа и без RPC. params — буфер NV_GR_CTXBUF_PARAMS_SIZE байт. */
void nv_gsp_gr_parse_ctxbuf_info(const uint8_t *params, uint32_t gr_index,
                                 nv_gsp_gr_ctxbufs *out);

/* ---- Таблица векторов прерываний ----
 * NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE. Нужна для событий и recovery:
 * под GSP завершение и ошибки приходят прерываниями, а не опросом регистров.
 * params: tableLen@0, table[128] по 16 байт @4, subtreeMap[7] @2052. Всего 2068.
 * ВНИМАНИЕ: engineIdx — 16-битный, за ним 2 байта выравнивания; шаг записи 16.
 */
#define NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE 0x20800A5Cu
#define NV_INTR_TABLE_PARAMS_SIZE     2068u
#define NV_INTR_TABLE_MAX_ENTRIES      128u
#define NV_INTR_TABLE_ENTRY_STRIDE      16u

typedef struct {
    uint16_t engine_idx;
    uint32_t pmc_intr_mask;
    uint32_t vector_stall;
    uint32_t vector_nonstall;
} nv_gsp_intr_entry;

typedef struct {
    uint32_t          count;
    nv_gsp_intr_entry entry[NV_INTR_TABLE_MAX_ENTRIES];
} nv_gsp_intr_table;

/*
 * Прочитать таблицу векторов прерываний. hIntClient/hIntSubdevice — ВНУТРЕННИЕ
 * хэндлы GSP. Возврат: NV_GSP_RM_OK и *status == 0 — данные валидны.
 */
int nv_gsp_intr_get_table(nv_gsp_rpc_chan *ch, uint32_t hIntClient,
                          uint32_t hIntSubdevice,
                          nv_gsp_intr_table *out, uint32_t *status);

/* Разбор таблицы прерываний отдельно от транспорта (см. выше). */
void nv_gsp_intr_parse_table(const uint8_t *params, nv_gsp_intr_table *out);

#endif /* RTXMACOC_GSP_GR_H */
