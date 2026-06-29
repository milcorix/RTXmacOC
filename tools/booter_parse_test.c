/*
 * booter_parse_test.c — офлайн-проверка разбора контейнера Booter (фаза 1-2 задачи 6).
 *
 * Грузит блоб через шим fw_blob (Linux: распаковка zst), разбирает контейнер
 * (booter.c) и печатает раскладку + базовые проверки. GPU НЕ нужен.
 *
 * Сборка:  make booter-parse-test   (или см. ниже)
 *   cc -Wall -O2 tools/booter_parse_test.c tools/fw_blob_linux.c \
 *      driver/gsp/booter.c -o booter_parse_test
 * Запуск:  ./booter_parse_test [booter_load|booter_unload]
 *   (env RTX_FW_DIR / RTX_FW_VERSION переопределяют путь/версию)
 */
#include "../driver/gsp/fw_blob.h"
#include "../driver/gsp/booter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *name = (argc >= 2) ? argv[1] : "booter_load";

    uint8_t *blob = NULL;
    size_t blob_len = 0;
    int rc = nv_fw_blob_get(name, &blob, &blob_len);
    if (rc != NV_FW_BLOB_OK) {
        fprintf(stderr, "FAIL: не загрузить блоб '%s' (rc=%d). "
                "Проверь zstd и путь (RTX_FW_DIR/RTX_FW_VERSION).\n", name, rc);
        return 1;
    }
    printf("blob '%s': %zu байт (распаковано)\n", name, blob_len);

    nv_booter_desc_t d;
    rc = nv_booter_parse(blob, blob_len, &d);
    if (rc != NV_BOOTER_OK) {
        fprintf(stderr, "FAIL: nv_booter_parse rc=%d "
                "(-2 BOUNDS, -3 MAGIC, -4 LAYOUT)\n", rc);
        nv_fw_blob_free(blob);
        return 1;
    }

    printf("=== bin ===\n");
    printf("  data_abs=%u data_size=%u (0x%x)\n", d.data_abs, d.data_size, d.data_size);
    printf("=== HS header v2 ===\n");
    printf("  signatures: abs=%u size=%u → num_sig=%u (по %u байт)\n",
           d.sig_prod_abs, d.sig_prod_size, d.num_sig, NV_BOOTER_SIG_SIZE);
    printf("  patch_loc=%u (data-rel) patch_sig=%u\n", d.patch_loc, d.patch_sig);
    printf("  meta abs=%u size=%u: [%u, %u, %u]\n",
           d.meta_abs, d.meta_size, d.meta[0], d.meta[1], d.meta[2]);
    printf("=== load header v2 ===\n");
    printf("  os_code: offset=%u size=%u\n", d.os_code_offset, d.os_code_size);
    printf("  os_data: offset=%u size=%u\n", d.os_data_offset, d.os_data_size);
    printf("  num_apps=%u\n", d.num_apps);
    for (uint32_t i = 0; i < d.num_apps; i++)
        printf("    app[%u]: code(off=%u,sz=%u) data(off=%u,sz=%u)\n", i,
               d.app[i].code_offset, d.app[i].code_size,
               d.app[i].data_offset, d.app[i].data_size);
    printf("=== вычислено ===\n");
    printf("  pkc_data_offset = patch_loc - os_data_offset = %u\n", d.pkc_data_offset);

    /* Базовые инварианты (метрика фазы 1-2). */
    int ok = 1;
    if (d.num_sig < 1)                         { printf("  ✗ num_sig < 1\n"); ok = 0; }
    if (d.os_code_size == 0)                   { printf("  ✗ os_code_size == 0\n"); ok = 0; }
    if (d.os_data_size == 0)                   { printf("  ✗ os_data_size == 0\n"); ok = 0; }
    /* IMEM(os_code+apps) + DMEM(os_data) должны укладываться в регион данных. */
    uint64_t imem = (uint64_t)d.os_code_size;
    for (uint32_t i = 0; i < d.num_apps; i++) imem += d.app[i].code_size;
    if (imem + d.os_data_size > d.data_size)   { printf("  ✗ IMEM+DMEM > data_size\n"); ok = 0; }
    if ((uint64_t)d.sig_prod_abs + d.sig_prod_size > blob_len) { printf("  ✗ подписи вне блоба\n"); ok = 0; }

    nv_fw_blob_free(blob);

    if (ok) {
        printf("\n=== РЕЗУЛЬТАТ: OK (контейнер Booter разобран, инварианты выполнены) ===\n");
        return 0;
    }
    printf("\n=== РЕЗУЛЬТАТ: FAIL (см. ✗ выше) ===\n");
    return 1;
}
