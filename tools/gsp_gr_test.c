/*
 * gsp_gr_test.c — офлайн-проверка раскладок привилегированных ответов GSP
 * (контекстные буферы GR и таблица векторов прерываний).
 *
 * Смысл теста тот же, что у проверки кодировки движка копирования: раскладку
 * получаем не из чтения заголовков глазами, а из компиляции оригинальных
 * структур NVIDIA — и закрепляем здесь, чтобы случайная правка не сдвинула
 * смещения молча. На железе такую ошибку видно только как «ноль во всех полях»,
 * что легко принять за «карта не поддерживает».
 *
 * Сборка: make gsp-gr-test && ./tools/gsp_gr_test
 */
#include <stdio.h>
#include <string.h>
#include "../driver/gsp/gsp_gr.h"

static int failed = 0;
#define CHECK(c, msg) do { if (c) printf("  ok: %s\n", msg); \
                           else { printf("  ПРОВАЛ: %s\n", msg); failed = 1; } } while (0)

static void st32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void st16(uint8_t *p, uint16_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

static void test_ctxbuf_layout(void)
{
    printf("[test_ctxbuf_layout]\n");
    CHECK(NV_GR_CTXBUF_PARAMS_SIZE == 1600u, "params = 1600 байт (8 движков * 25 * 8)");
    CHECK(NV_GR_CTXBUF_ENGINE_STRIDE == 200u, "шаг движка = 200 байт");
    CHECK(NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO == 0x20800A32u,
          "номер контрола KGR_GET_CONTEXT_BUFFERS_INFO");

    static uint8_t p[NV_GR_CTXBUF_PARAMS_SIZE];
    memset(p, 0, sizeof(p));

    /* Движок 0: буфер 3 = 128 КиБ/выравн. 4096; буфер 7 = 1 МиБ/выравн. 65536. */
    st32(p + 0 * NV_GR_CTXBUF_ENGINE_STRIDE + 3 * 8 + 0, 128u * 1024u);
    st32(p + 0 * NV_GR_CTXBUF_ENGINE_STRIDE + 3 * 8 + 4, 4096u);
    st32(p + 0 * NV_GR_CTXBUF_ENGINE_STRIDE + 7 * 8 + 0, 1024u * 1024u);
    st32(p + 0 * NV_GR_CTXBUF_ENGINE_STRIDE + 7 * 8 + 4, 65536u);
    /* Движок 1: буфер 0 = 42 — не должен попасть в разбор движка 0. */
    st32(p + 1 * NV_GR_CTXBUF_ENGINE_STRIDE + 0 * 8 + 0, 42u);

    nv_gsp_gr_ctxbufs cb;
    memset(&cb, 0xAA, sizeof(cb));
    nv_gsp_gr_parse_ctxbuf_info(p, 0, &cb);

    CHECK(cb.count_nonzero == 2, "у движка 0 ровно два ненулевых буфера");
    CHECK(cb.buf[3].size == 128u*1024u && cb.buf[3].alignment == 4096u, "буфер 3 разобран");
    CHECK(cb.buf[7].size == 1024u*1024u && cb.buf[7].alignment == 65536u, "буфер 7 разобран");
    CHECK(cb.buf[0].size == 0, "чужой движок не протёк в разбор");

    nv_gsp_gr_ctxbufs cb1;
    memset(&cb1, 0, sizeof(cb1));
    nv_gsp_gr_parse_ctxbuf_info(p, 1, &cb1);
    CHECK(cb1.buf[0].size == 42u && cb1.count_nonzero == 1, "движок 1 разобран отдельно");

    /* Выход за число движков не должен ничего трогать. */
    nv_gsp_gr_ctxbufs cbx;
    memset(&cbx, 0, sizeof(cbx));
    nv_gsp_gr_parse_ctxbuf_info(p, NV_GR_CTXBUF_MAX_ENGINES, &cbx);
    CHECK(cbx.count_nonzero == 0, "индекс движка вне диапазона отвергнут");
}

static void test_intr_layout(void)
{
    printf("[test_intr_layout]\n");
    CHECK(NV_INTR_TABLE_PARAMS_SIZE == 2068u, "params = 2068 байт");
    CHECK(NV_INTR_TABLE_ENTRY_STRIDE == 16u, "шаг записи = 16 байт");
    CHECK(NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE == 0x20800A5Cu,
          "номер контрола INTR_GET_KERNEL_TABLE");

    static uint8_t p[NV_INTR_TABLE_PARAMS_SIZE];
    memset(p, 0, sizeof(p));
    st32(p + 0, 3u);                       /* tableLen */
    /* Запись 0: engineIdx 16-битный, затем 2 байта пада, затем три dword. */
    st16(p + 4 + 0*16 + 0,  1u);
    st32(p + 4 + 0*16 + 4,  0xDEADBEEFu);
    st32(p + 4 + 0*16 + 8,  11u);
    st32(p + 4 + 0*16 + 12, 12u);
    st16(p + 4 + 2*16 + 0,  0x1234u);
    st32(p + 4 + 2*16 + 12, 99u);

    nv_gsp_intr_table it;
    memset(&it, 0, sizeof(it));
    nv_gsp_intr_parse_table(p, &it);

    CHECK(it.count == 3, "длина таблицы прочитана");
    CHECK(it.entry[0].engine_idx == 1u, "engineIdx (16 бит)");
    CHECK(it.entry[0].pmc_intr_mask == 0xDEADBEEFu, "pmcIntrMask после 2 байт пада");
    CHECK(it.entry[0].vector_stall == 11u && it.entry[0].vector_nonstall == 12u, "векторы");
    CHECK(it.entry[2].engine_idx == 0x1234u && it.entry[2].vector_nonstall == 99u,
          "третья запись — шаг 16 выдержан");

    /* Защита от вранья прошивки про длину. */
    st32(p + 0, 0xFFFFFFFFu);
    memset(&it, 0, sizeof(it));
    nv_gsp_intr_parse_table(p, &it);
    CHECK(it.count == NV_INTR_TABLE_MAX_ENTRIES, "длина ограничена ёмкостью таблицы");
}

int main(void)
{
    test_ctxbuf_layout();
    test_intr_layout();
    printf(failed ? "\n=== gsp_gr_test: ЕСТЬ ПРОВАЛЫ ===\n"
                  : "\n=== gsp_gr_test: ВСЕ ТЕСТЫ ПРОШЛИ ===\n");
    return failed ? 1 : 0;
}
