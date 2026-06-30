/*
 * gsp_rpc.c — реализация очередей сообщений GSP-RM (см. gsp_rpc.h).
 * Порт nouveau r535_gsp_shared_init: PTE-таблица + cmdq + msgq, заголовки очередей.
 */
#include "gsp_rpc.h"

#define GSP_PAGE 0x1000u

static void st32(uint8_t *p, uint32_t v)
{ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void st64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

int nv_gsp_shm_compute(nv_gsp_shm_layout_t *out)
{
    if (!out) return NV_GSP_RPC_ERR_ARG;
    uint32_t nr = (NV_GSP_QUEUE_SIZE + NV_GSP_QUEUE_SIZE) / GSP_PAGE; /* страниц cmdq+msgq */
    nr += (uint32_t)(((uint64_t)nr * 8u + GSP_PAGE - 1) / GSP_PAGE);  /* + страницы под сами PTE */
    uint64_t ptes_size = (((uint64_t)nr * 8u) + GSP_PAGE - 1) & ~(uint64_t)(GSP_PAGE - 1);
    out->ptes_nr   = nr;
    out->ptes_size = ptes_size;
    out->cmdq_off  = ptes_size;
    out->msgq_off  = ptes_size + NV_GSP_QUEUE_SIZE;
    out->total_size= ptes_size + (uint64_t)NV_GSP_QUEUE_SIZE * 2u;
    out->msg_count = (NV_GSP_QUEUE_SIZE - NV_GSP_QUEUE_ENTRYOFF) / NV_GSP_QUEUE_MSGSIZE;
    return NV_GSP_RPC_OK;
}

static void init_queue_hdr(uint8_t *q, uint32_t msg_count)
{
    st32(q + NV_MSGQ_TX_VERSION_OFF,  0u);
    st32(q + NV_MSGQ_TX_SIZE_OFF,     NV_GSP_QUEUE_SIZE);
    st32(q + NV_MSGQ_TX_MSGSIZE_OFF,  NV_GSP_QUEUE_MSGSIZE);
    st32(q + NV_MSGQ_TX_MSGCOUNT_OFF, msg_count);
    st32(q + NV_MSGQ_TX_WRITEPTR_OFF, 0u);
    st32(q + NV_MSGQ_TX_FLAGS_OFF,    1u);
    st32(q + NV_MSGQ_TX_RXHDROFF_OFF, NV_GSP_MSGQ_RXHDROFF);
    st32(q + NV_MSGQ_TX_ENTRYOFF_OFF, NV_GSP_QUEUE_ENTRYOFF);
    st32(q + NV_GSP_MSGQ_RXHDROFF,    0u); /* rx.readPtr = 0 */
}

int nv_gsp_shm_init(uint8_t *buf, size_t buflen, uint64_t dma_base, nv_gsp_shm_layout_t *out)
{
    if (!buf || !out) return NV_GSP_RPC_ERR_ARG;
    nv_gsp_shm_compute(out);
    if (out->total_size > buflen) return NV_GSP_RPC_ERR_BOUNDS;

    for (size_t i = 0; i < out->total_size; i++) buf[i] = 0;
    /* PTE-таблица: страница за страницей весь регион. */
    for (uint32_t i = 0; i < out->ptes_nr; i++)
        st64(buf + (size_t)i * 8u, dma_base + (uint64_t)i * GSP_PAGE);
    /* Заголовки очередей. */
    init_queue_hdr(buf + out->cmdq_off, out->msg_count);
    init_queue_hdr(buf + out->msgq_off, out->msg_count);
    return NV_GSP_RPC_OK;
}
