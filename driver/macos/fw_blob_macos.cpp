/*
 * fw_blob_macos.cpp — macOS-kext-шим загрузки прошивок NVIDIA (см. driver/gsp/fw_blob.h).
 *
 * Ядро macOS не имеет popen/libzstd, поэтому (в отличие от Linux-шима
 * tools/fw_blob_linux.c) РАСПАКОВКУ ЗДЕСЬ НЕ ДЕЛАЕМ. Блобы кладутся на диск уже
 * распакованными установочным скриптом:
 *
 *   /Library/Application Support/Milcorix/fw/<name>-<version>.bin
 *
 * (booter_load / booter_unload / bootloader / gsp / scrubber). Правило 7 соблюдено:
 * блобы в git не коммитим — их разворачивает установщик из системного
 * linux-firmware эквивалента (легальный источник NVIDIA, версия 535.113.01).
 *
 * Читаем файл через BSD vnode API (vnode_open/vn_rdwr) — единственный способ
 * файлового I/O из kext. Портируемый core (booter.c/gsp_fw.c) видит только сырые
 * байты через nv_fw_blob_get, как и на Linux.
 */
#include <IOKit/IOLib.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>

extern "C" {
#include "../gsp/fw_blob.h"
}

#define FW_DIR      "/Library/Application Support/Milcorix/fw"
#define FW_VERSION  "535.113.01"
#define FW_MAX_SIZE (64u * 1024u * 1024u)   /* потолок здравого смысла: gsp ~36 МиБ */

/*
 * Аллокация под блоб. nv_fw_blob_free получает ТОЛЬКО указатель, а IOFree требует
 * длину — поэтому прячем длину в 8-байтовом префиксе и возвращаем buf+8. Префикс
 * держит выравнивание (IOMalloc уже выровнен, +8 сохраняет 8-байтовость).
 */
static uint8_t *blob_alloc(size_t len, size_t *total_out)
{
    size_t total = len + sizeof(size_t);
    uint8_t *raw = (uint8_t *)IOMalloc(total);
    if (!raw) return nullptr;
    *(size_t *)raw = total;              /* полный размер аллокации для IOFree */
    if (total_out) *total_out = total;
    return raw + sizeof(size_t);         /* пользовательские байты */
}

/* Имя блоба должно быть [a-z0-9_] — оно идёт в путь файла. */
static bool name_is_safe(const char *name)
{
    if (!name || !*name) return false;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return true;
}

int nv_fw_blob_get(const char *name, uint8_t **out, size_t *out_len)
{
    if (!name || !out || !out_len) return NV_FW_BLOB_ERR_ARG;
    *out = nullptr; *out_len = 0;
    if (!name_is_safe(name)) return NV_FW_BLOB_ERR_ARG;

    char path[256];
    int n = snprintf(path, sizeof(path), "%s/%s-%s.bin", FW_DIR, name, FW_VERSION);
    if (n <= 0 || (size_t)n >= sizeof(path)) return NV_FW_BLOB_ERR_ARG;

    vfs_context_t ctx = vfs_context_create(NULL);
    if (!ctx) return NV_FW_BLOB_ERR_IO;

    vnode_t vp = NULLVP;
    /* O_RDONLY, cmode 0 (не создаём), flags 0. */
    if (vnode_open(path, FREAD, 0, 0, &vp, ctx) != 0) {
        IOLog("MilcorixFW: не открыть %s\n", path);
        vfs_context_rele(ctx);
        return NV_FW_BLOB_ERR_IO;
    }

    /* Размер файла — через публичный vnode_getattr (va_data_size), а не
       vnode_size (тот не всегда экспортирован в KPI). */
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    off_t fsize = 0;
    if (vnode_getattr(vp, &va, ctx) != 0 || !VATTR_IS_SUPPORTED(&va, va_data_size)) {
        IOLog("MilcorixFW: не прочитать размер %s\n", path);
        vnode_close(vp, FREAD, ctx);
        vfs_context_rele(ctx);
        return NV_FW_BLOB_ERR_IO;
    }
    fsize = (off_t)va.va_data_size;
    if (fsize <= 0 || (uint64_t)fsize > FW_MAX_SIZE) {
        IOLog("MilcorixFW: некорректный размер %s (%lld)\n", path, (long long)fsize);
        vnode_close(vp, FREAD, ctx);
        vfs_context_rele(ctx);
        return NV_FW_BLOB_ERR_IO;
    }

    size_t len = (size_t)fsize, total = 0;
    uint8_t *buf = blob_alloc(len, &total);
    if (!buf) {
        vnode_close(vp, FREAD, ctx);
        vfs_context_rele(ctx);
        return NV_FW_BLOB_ERR_MEM;
    }

    /* vn_rdwr читает len байт с offset 0 в системный буфер. Читаем в цикле:
       для обычного файла обычно за один вызов, но добираем при частичном чтении. */
    int rc = NV_FW_BLOB_OK;
    off_t off = 0;
    while ((size_t)off < len) {
        int chunk = (int)(len - (size_t)off);   /* len <= 64 МиБ < INT_MAX */
        int resid = 0;
        int e = vn_rdwr(UIO_READ, vp, (caddr_t)(buf + off), chunk, off,
                        UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (e != 0) { IOLog("MilcorixFW: vn_rdwr %s ошибка %d\n", path, e); rc = NV_FW_BLOB_ERR_IO; break; }
        int got = chunk - resid;
        if (got <= 0) { rc = NV_FW_BLOB_ERR_IO; break; }   /* EOF раньше времени */
        off += got;
    }

    vnode_close(vp, FREAD, ctx);
    vfs_context_rele(ctx);

    if (rc != NV_FW_BLOB_OK) {
        nv_fw_blob_free(buf);
        return rc;
    }

    IOLog("MilcorixFW: загружен %s (%zu байт)\n", name, len);
    *out = buf;
    *out_len = len;
    return NV_FW_BLOB_OK;
}

void nv_fw_blob_free(uint8_t *buf)
{
    if (!buf) return;
    uint8_t *raw = buf - sizeof(size_t);
    size_t total = *(size_t *)raw;
    IOFree(raw, total);
}
