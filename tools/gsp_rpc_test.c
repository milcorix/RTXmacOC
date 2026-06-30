/*
 * gsp_rpc_test.c — офлайн-проверка раскладки очередей GSP-RM (задача 7, часть 1).
 * Строит общий регион (PTE+cmdq+msgq) со синтетической базой, проверяет заголовки.
 * Сборка: make gsp-rpc-test ; запуск: ./tools/gsp_rpc_test   (GPU не нужен)
 */
#include "../driver/gsp/gsp_rpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t ld32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint64_t ld64(const uint8_t *p){ uint64_t v=0; for(int i=0;i<8;i++) v|=(uint64_t)p[i]<<(8*i); return v; }

int main(void)
{
    int ok = 1;
    nv_gsp_shm_layout_t L;
    nv_gsp_shm_compute(&L);
    printf("layout: ptes_nr=%u ptes_size=0x%llx cmdq@0x%llx msgq@0x%llx total=0x%llx msg_count=%u\n",
           L.ptes_nr,(unsigned long long)L.ptes_size,(unsigned long long)L.cmdq_off,
           (unsigned long long)L.msgq_off,(unsigned long long)L.total_size,L.msg_count);

    const uint64_t DMA = 0x40000000ull;
    uint8_t *buf = calloc(1, L.total_size);
    if (!buf){ fprintf(stderr,"alloc\n"); return 1; }
    if (nv_gsp_shm_init(buf,L.total_size,DMA,&L)!=NV_GSP_RPC_OK){ fprintf(stderr,"FAIL: shm_init\n"); return 1; }

    /* Инварианты раскладки. */
    if (L.msg_count != 63) { printf("  ✗ msg_count != 63\n"); ok=0; }
    if (L.cmdq_off != L.ptes_size) { printf("  ✗ cmdq_off\n"); ok=0; }
    if (L.msgq_off != L.cmdq_off + NV_GSP_QUEUE_SIZE) { printf("  ✗ msgq_off\n"); ok=0; }

    /* PTE-цепочка. */
    if (ld64(buf) != DMA) { printf("  ✗ pte[0]\n"); ok=0; }
    if (ld64(buf + (size_t)(L.ptes_nr-1)*8) != DMA + (uint64_t)(L.ptes_nr-1)*0x1000) { printf("  ✗ pte[last]\n"); ok=0; }

    /* Заголовок cmdq. */
    const uint8_t *cq = buf + L.cmdq_off;
    printf("cmdq tx: version=%u size=0x%x msgSize=0x%x msgCount=%u writePtr=%u flags=%u rxHdrOff=%u entryOff=0x%x rx.readPtr=%u\n",
           ld32(cq+0),ld32(cq+4),ld32(cq+8),ld32(cq+12),ld32(cq+16),ld32(cq+20),ld32(cq+24),ld32(cq+28),ld32(cq+NV_GSP_MSGQ_RXHDROFF));
    if (ld32(cq+NV_MSGQ_TX_SIZE_OFF)     != NV_GSP_QUEUE_SIZE)    { printf("  ✗ cmdq size\n"); ok=0; }
    if (ld32(cq+NV_MSGQ_TX_MSGSIZE_OFF)  != NV_GSP_QUEUE_MSGSIZE) { printf("  ✗ cmdq msgSize\n"); ok=0; }
    if (ld32(cq+NV_MSGQ_TX_MSGCOUNT_OFF) != 63)                  { printf("  ✗ cmdq msgCount\n"); ok=0; }
    if (ld32(cq+NV_MSGQ_TX_FLAGS_OFF)    != 1)                   { printf("  ✗ cmdq flags\n"); ok=0; }
    if (ld32(cq+NV_MSGQ_TX_RXHDROFF_OFF) != NV_GSP_MSGQ_RXHDROFF){ printf("  ✗ cmdq rxHdrOff\n"); ok=0; }
    if (ld32(cq+NV_MSGQ_TX_ENTRYOFF_OFF) != NV_GSP_QUEUE_ENTRYOFF){printf("  ✗ cmdq entryOff\n"); ok=0; }

    /* msgq существует с тем же заголовком. */
    const uint8_t *mq = buf + L.msgq_off;
    if (ld32(mq+NV_MSGQ_TX_MSGCOUNT_OFF) != 63) { printf("  ✗ msgq msgCount\n"); ok=0; }

    free(buf);
    printf(ok ? "\n=== РЕЗУЛЬТАТ: OK (раскладка очередей GSP согласована) ===\n"
              : "\n=== РЕЗУЛЬТАТ: FAIL (см. ✗) ===\n");
    return ok ? 0 : 1;
}
