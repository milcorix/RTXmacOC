/*
 * fw_blob_linux.c — Linux-шим загрузки прошивок NVIDIA (см. fw_blob.h).
 *
 * Находит файл /usr/lib/firmware/nvidia/<chip>/gsp/<name>-<version>.bin.zst и
 * распаковывает его системным zstd (через popen "zstd -dcf"), чтобы не тащить в
 * проект зависимость на libzstd/zstd.h (правило 5: только системные средства).
 * Распаковка живёт ТОЛЬКО в этом стенд-шиме; портируемый код видит сырые байты.
 *
 * Переопределение через окружение:
 *   RTX_FW_DIR      — каталог с блобами (по умолчанию /usr/lib/firmware/nvidia/ad104/gsp)
 *   RTX_FW_VERSION  — версия прошивки   (по умолчанию 535.113.01)
 */
#define _GNU_SOURCE
#include "../driver/gsp/fw_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define DEFAULT_FW_DIR     "/usr/lib/firmware/nvidia/ad104/gsp"
#define DEFAULT_FW_VERSION "535.113.01"
#define READ_CHUNK         (256u * 1024u)

/* Имя блоба должно состоять из [a-z0-9_], чтобы безопасно подставлять в shell. */
static int name_is_safe(const char *name)
{
    if (!name || !*name) return 0;
    for (const char *p = name; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_'))
            return 0;
    return 1;
}

int nv_fw_blob_get(const char *name, uint8_t **out, size_t *out_len)
{
    if (!name || !out || !out_len) return NV_FW_BLOB_ERR_ARG;
    *out = NULL; *out_len = 0;
    if (!name_is_safe(name)) return NV_FW_BLOB_ERR_ARG;

    const char *dir = getenv("RTX_FW_DIR");
    const char *ver = getenv("RTX_FW_VERSION");
    if (!dir) dir = DEFAULT_FW_DIR;
    if (!ver) ver = DEFAULT_FW_VERSION;
    /* Версию тоже валидируем (цифры и точки) — она идёт в shell-команду. */
    for (const char *p = ver; *p; p++)
        if (!(isdigit((unsigned char)*p) || *p == '.')) return NV_FW_BLOB_ERR_ARG;

    char path[512];
    int n = snprintf(path, sizeof(path), "%s/%s-%s.bin.zst", dir, name, ver);
    if (n <= 0 || (size_t)n >= sizeof(path)) return NV_FW_BLOB_ERR_ARG;

    /* zstd -dcf <path>: распаковка в stdout. -f чтобы не ругался на расширение. */
    char cmd[640];
    n = snprintf(cmd, sizeof(cmd), "zstd -dcf '%s' 2>/dev/null", path);
    if (n <= 0 || (size_t)n >= sizeof(cmd)) return NV_FW_BLOB_ERR_ARG;

    FILE *fp = popen(cmd, "r");
    if (!fp) return NV_FW_BLOB_ERR_IO;

    size_t cap = READ_CHUNK, len = 0;
    uint8_t *buf = malloc(cap);
    if (!buf) { pclose(fp); return NV_FW_BLOB_ERR_MEM; }

    for (;;) {
        if (len + READ_CHUNK > cap) {
            size_t ncap = cap * 2;
            uint8_t *nb = realloc(buf, ncap);
            if (!nb) { free(buf); pclose(fp); return NV_FW_BLOB_ERR_MEM; }
            buf = nb; cap = ncap;
        }
        size_t r = fread(buf + len, 1, READ_CHUNK, fp);
        len += r;
        if (r < READ_CHUNK) {
            if (ferror(fp)) { free(buf); pclose(fp); return NV_FW_BLOB_ERR_IO; }
            break; /* EOF */
        }
    }

    int rc = pclose(fp); /* код возврата zstd: 0 = успех */
    if (rc != 0 || len == 0) { free(buf); return NV_FW_BLOB_ERR_DECOMP; }

    *out = buf;
    *out_len = len;
    return NV_FW_BLOB_OK;
}

void nv_fw_blob_free(uint8_t *buf)
{
    free(buf);
}
