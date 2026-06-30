/*
 * gsp_rpc.h — очереди сообщений GSP-RM в общей памяти (задача 7).
 *
 * GSP-RM общается с драйвером через две кольцевые очереди в sysmem:
 *   cmdq (CPU→GSP, команды/RPC) и msgq/statq (GSP→CPU, ответы/события).
 * Регион: [PTE-таблица][cmdq 0x40000][msgq 0x40000]; PTE маппят весь регион.
 * Заголовок каждой очереди — msgqTxHeader(32б)+msgqRxHeader(4б); записи с entryOff=4K,
 * размер записи 4K. Элемент = GSP_MSG_QUEUE_ELEMENT (48б шапка) + rpc.
 *
 * Источники: OGK msgq_priv.h (msgqTxHeader/RxHeader), message_queue_priv.h
 * (GSP_MSG_QUEUE_ELEMENT), nouveau r535_gsp_shared_init. Пересказано для лицензии.
 */
#ifndef RTXMACOC_GSP_RPC_H
#define RTXMACOC_GSP_RPC_H

#include <stdint.h>
#include <stddef.h>

#define NV_GSP_RPC_OK           0
#define NV_GSP_RPC_ERR_ARG    (-1)
#define NV_GSP_RPC_ERR_BOUNDS (-2)

#define NV_GSP_QUEUE_SIZE     0x40000u            /* размер cmdq и msgq (по 256 КиБ) */
#define NV_GSP_QUEUE_ENTRYOFF 0x1000u             /* записи с 1-й страницы */
#define NV_GSP_QUEUE_MSGSIZE  0x1000u             /* размер записи = 4K */
#define NV_GSP_MSGQ_RXHDROFF  32u                 /* offsetof(rx.readPtr) = sizeof(msgqTxHeader) */

/* GSP_MSG_QUEUE_ELEMENT: authTag[16]+aad[16]+checkSum+seqNum+elemCount, затем rpc@48. */
#define NV_GSP_MSG_ELEM_HDR_SIZE 48u
/* rpc_message_header_v03: header_version@0, signature@4, ... */
#define NV_GSP_RPC_HEADER_VERSION 0x03000000u
#define NV_GSP_RPC_SIGNATURE      0x43505256u     /* 'VRPC' */

/* msgqTxHeader (OGK msgq_priv.h), все u32: */
#define NV_MSGQ_TX_VERSION_OFF   0u
#define NV_MSGQ_TX_SIZE_OFF      4u
#define NV_MSGQ_TX_MSGSIZE_OFF   8u
#define NV_MSGQ_TX_MSGCOUNT_OFF  12u
#define NV_MSGQ_TX_WRITEPTR_OFF  16u
#define NV_MSGQ_TX_FLAGS_OFF     20u
#define NV_MSGQ_TX_RXHDROFF_OFF  24u
#define NV_MSGQ_TX_ENTRYOFF_OFF  28u
/* msgqRxHeader: readPtr@0 (по rxHdrOff от начала очереди). */

/* Раскладка общего региона очередей. */
typedef struct {
    uint32_t ptes_nr;        /* число PTE (страниц во всём регионе) */
    uint64_t ptes_size;      /* размер PTE-таблицы (= смещение cmdq) */
    uint64_t cmdq_off;       /* смещение cmdq от начала региона */
    uint64_t msgq_off;       /* смещение msgq */
    uint64_t total_size;     /* ptes_size + 2*QUEUE_SIZE */
    uint32_t msg_count;      /* записей в очереди = (size-entryOff)/msgSize */
} nv_gsp_shm_layout_t;

/* Рассчитать раскладку (без буфера). */
int nv_gsp_shm_compute(nv_gsp_shm_layout_t *out);

/*
 * Инициализировать общий регион очередей в buf (>= total_size, обнуляется): PTE-таблица
 * (ptes[i]=dma_base+i*4K) + заголовки cmdq/msgq (msgqTxHeader/RxHeader). dma_base — IOVA
 * региона. *out — раскладка. Порт nouveau r535_gsp_shared_init.
 */
int nv_gsp_shm_init(uint8_t *buf, size_t buflen, uint64_t dma_base, nv_gsp_shm_layout_t *out);

#endif /* RTXMACOC_GSP_RPC_H */
