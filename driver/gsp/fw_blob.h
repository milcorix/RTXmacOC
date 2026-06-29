/*
 * fw_blob.h — портируемый интерфейс загрузки подписанных прошивок NVIDIA
 * (Booter Loader/Unloader, GSP bootloader, GSP-RM) для слоя 2 (задача 6).
 *
 * Зачем: FWSEC берётся из VBIOS самой карты, а Booter и GSP-RM — это ОТДЕЛЬНЫЕ
 * подписанные блобы NVIDIA, распространяемые в составе linux-firmware
 * (/usr/lib/firmware/nvidia/<chip>/gsp/<name>-<version>.bin.zst). Их грузим КАК
 * ЕСТЬ (правило 2), в репозиторий не коммитим (правило 7).
 *
 * Портируемый код (driver/gsp/booter.c и т.д.) файловой системы и распаковки НЕ
 * видит — он получает уже распакованные байты через этот интерфейс. Платформенные
 * шимы:
 *   Linux (стенд):  tools/fw_blob_linux.c — поиск файла + распаковка системным zstd;
 *   macOS (kext):   позже — загрузка прошивки + распаковка в ядре.
 */
#ifndef RTXMACOC_FW_BLOB_H
#define RTXMACOC_FW_BLOB_H

#include <stdint.h>
#include <stddef.h>

/* Коды возврата. */
#define NV_FW_BLOB_OK         0
#define NV_FW_BLOB_ERR_ARG   (-1)
#define NV_FW_BLOB_ERR_IO    (-2)  /* файл не найден/не читается */
#define NV_FW_BLOB_ERR_DECOMP (-3) /* распаковка не удалась */
#define NV_FW_BLOB_ERR_MEM   (-4)

/*
 * Загрузить и (при необходимости) распаковать блоб прошивки по логическому имени:
 *   "booter_load", "booter_unload", "bootloader", "gsp", "scrubber".
 * При успехе возвращает NV_FW_BLOB_OK, *out = malloc-буфер с распакованными
 * байтами, *out_len = длина. Буфер освобождать через nv_fw_blob_free().
 * Конкретная версия/путь — забота шима (Linux-шим: env RTX_FW_DIR/RTX_FW_VERSION,
 * по умолчанию ad104/gsp + 535.113.01).
 */
int nv_fw_blob_get(const char *name, uint8_t **out, size_t *out_len);

void nv_fw_blob_free(uint8_t *buf);

#endif /* RTXMACOC_FW_BLOB_H */
