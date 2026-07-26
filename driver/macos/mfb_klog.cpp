/*
 * mfb_klog.cpp — реализация журнала, переживающего чёрный экран (см. mfb_klog.h).
 *
 * Файловый I/O — тем же BSD vnode API, что и загрузка прошивок
 * (driver/macos/fw_blob_macos.cpp): единственный доступный из kext путь.
 * NVRAM — через IODeviceTree:/options, имя переменной с Apple-GUID, чтобы она
 * появилась в efivarfs как <GUID>-milcorix-status и читалась из Linux.
 */
#include "mfb_klog.h"

#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSString.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kernel.h>

#define KLOG_DIR    "/Library/Application Support/Milcorix"
#define KLOG_PATH   KLOG_DIR "/lastboot.log"
#define KLOG_SIZE   (512u * 1024u)     /* трейс bring-up'а помещается с запасом */
#define KLOG_LINE   1024u              /* потолок одной строки */
#define KLOG_TRIES  24                 /* попыток отложенного сброса за загрузку */

/* Apple vendor GUID — тот же namespace, где живут boot-args. Имя в этом виде
   AppleEFINVRAM кладёт в efivarfs как <GUID>-milcorix-status. */
#define KLOG_NVRAM_VAR "7C436110-AB2A-4BBB-A880-FE41995C9F82:milcorix-status"

static char    *gBuf      = nullptr;
static uint32_t gLen      = 0;         /* занято байт */
static bool     gOverflow = false;
static int      gTriesLeft = KLOG_TRIES;
static IOLock  *gLock     = nullptr;
/* Последний статус и признак того, что NVRAM его принял. На раннем старте
   IODeviceTree:/options может быть ещё не поднят — тогда пробуем позже. */
static char     gStatus[160];
static bool     gStatusWritten = false;

void mfb_klog_init(void)
{
    if (gBuf) return;
    gLock = IOLockAlloc();
    gBuf  = (char *)IOMalloc(KLOG_SIZE);
    if (gBuf) { gBuf[0] = '\0'; gLen = 0; gOverflow = false; }
}

void mfb_klog_free(void)
{
    if (gBuf) { IOFree(gBuf, KLOG_SIZE); gBuf = nullptr; gLen = 0; }
    if (gLock) { IOLockFree(gLock); gLock = nullptr; }
}

void mfb_klog_vprintf(const char *fmt, va_list ap)
{
    if (!gBuf || !fmt) return;

    char line[KLOG_LINE];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) return;
    uint32_t len = (uint32_t)((size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);

    if (gLock) IOLockLock(gLock);
    if (gLen + len + 1 >= KLOG_SIZE) {
        /* Переполнение: держим НАЧАЛО лога — там причина, а не следствие. */
        gOverflow = true;
    } else {
        memcpy(gBuf + gLen, line, len);
        gLen += len;
        gBuf[gLen] = '\0';
    }
    if (gLock) IOLockUnlock(gLock);
}

void mfb_klog_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mfb_klog_vprintf(fmt, ap);
    va_end(ap);
}

/* Каталог KLOG_DIR создаёт установщик прошивок (tools/install_milcorix_fw.sh) —
   отдельно его здесь не заводим: vnode-API создания директорий в KPI нет. */

int mfb_klog_flush(void)
{
    if (!gBuf || gLen == 0) return -1;

    vfs_context_t ctx = vfs_context_create(NULL);
    if (!ctx) return -1;

    vnode_t vp = NULLVP;
    /* O_WRONLY|O_CREAT|O_TRUNC, режим 0644. */
    if (vnode_open(KLOG_PATH, FWRITE | O_CREAT | O_TRUNC, 0644, 0, &vp, ctx) != 0) {
        vfs_context_rele(ctx);
        return -1;                       /* корень ещё не смонтирован — ретрай позже */
    }

    int rc = 0;
    if (gLock) IOLockLock(gLock);
    uint32_t len = gLen;
    if (gLock) IOLockUnlock(gLock);

    off_t off = 0;
    while ((uint32_t)off < len) {
        int chunk = (int)(len - (uint32_t)off);
        int resid = 0;
        int e = vn_rdwr(UIO_WRITE, vp, (caddr_t)(gBuf + off), chunk, off,
                        UIO_SYSSPACE, IO_NODELOCKED,
                        vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (e != 0) { rc = -1; break; }
        int put = chunk - resid;
        if (put <= 0) { rc = -1; break; }
        off += put;
    }
    if (rc == 0 && gOverflow) {
        static const char tail[] = "\n[журнал переполнен — хвост обрезан]\n";
        int resid = 0;
        vn_rdwr(UIO_WRITE, vp, (caddr_t)tail, (int)sizeof(tail) - 1, off,
                UIO_SYSSPACE, IO_NODELOCKED,
                vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    }

    vnode_close(vp, FWRITE, ctx);
    vfs_context_rele(ctx);
    return rc;
}

/* Попытка положить gStatus в NVRAM. true — принято. */
static bool klog_status_commit(void)
{
    if (!gStatus[0]) return false;
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) return false;
    bool done = false;
    OSString *s = OSString::withCString(gStatus);
    if (s) {
        done = opts->setProperty(KLOG_NVRAM_VAR, s);
        s->release();
    }
    opts->release();
    return done;
}

void mfb_klog_flush_lazy(void)
{
    if (!gStatusWritten && klog_status_commit()) gStatusWritten = true;
    if (gTriesLeft <= 0) return;
    gTriesLeft--;
    if (mfb_klog_flush() == 0)
        gTriesLeft = 0;                  /* записали — больше не дёргаем ФС */
}

void mfb_klog_status(const char *line)
{
    if (!line) return;
    size_t i = 0;
    for (; i + 1 < sizeof(gStatus) && line[i]; i++) gStatus[i] = line[i];
    gStatus[i] = '\0';
    gStatusWritten = klog_status_commit();
}
