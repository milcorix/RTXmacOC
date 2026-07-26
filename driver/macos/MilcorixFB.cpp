/*
 * MilcorixFB.cpp — реализация IOFramebuffer для RTX 4070S. См. MilcorixFB.h
 * (там же — два жёстких правила: адресные пространства и «уметь не запускаться»).
 *
 * Порядок старта здесь НАМЕРЕННО обратный обычному: сначала мы поднимаем железо и
 * убеждаемся, что получили ГОДНЫЙ scanout-буфер, и только потом зовём
 * super::start() и становимся фреймбуфером системы. Смысл: если что-то пошло не
 * так, мы просто не подключаемся — macOS продолжает рисовать через EFI-фреймбуфер,
 * и у пользователя остаётся рабочий стол вместо чёрного экрана и ребута.
 */
#include "MilcorixFB.h"
#include "mfb_klog.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>

/* Переносимое ядро GSP — чистый C, компилируется как kernel-C. В C++-TU его
   символы (nv_gsp_bringup и т.д.) нужно объявлять с C-линковкой, иначе clang++
   ищет манглированное имя и линковка kext падает. */
extern "C" {
#include "../gsp/nv_dma.h"
#include "../gsp/falcon.h"
#include "../gsp/gsp_bringup.h"
#include "../gsp/gsp_disp.h"   /* NV_CTXDMA_TARGET_* */
#include "../gsp/gmmu.h"       /* окно PRAMIN — для проверки identity-маппинга BAR1 */
}

#define super IOFramebuffer
OSDefineMetaClassAndStructors(MilcorixFB, IOFramebuffer);

/* --- nv_mmio_t обёртки над BAR0 (IOKit) ---
 * Ядро GSP дергает io->rd/io->wr/io->udelay; здесь они читают/пишут BAR0 и IODelay.
 */
static uint32_t mfb_rd(void *ctx, uint32_t off)
{ return *(volatile uint32_t *)((volatile uint8_t *)ctx + off); }
static void mfb_wr(void *ctx, uint32_t off, uint32_t val)
{ *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = val; }
static void mfb_udelay(void *ctx, uint32_t us)
{ (void)ctx; IODelay(us); }

/* Трассировка оркестрации GSP. Идёт СРАЗУ В ДВА МЕСТА: в системный лог (IOLogv)
   и в наш дисковый журнал (mfb_klog) — второй читается из Linux даже тогда, когда
   экран чёрный и сфотографировать нечего. */
static void mfb_log(void *ctx, const char *fmt, ...)
{
    (void)ctx;
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    IOLogv(fmt, ap);
    mfb_klog_vprintf(fmt, ap2);
    va_end(ap2);
    va_end(ap);
}

/* Физ. база BAR по его config-регистру (для GspSystemInfo). getDeviceMemory* —
   get-конвенция: возвращённый объект не наш, освобождать не нужно. */
static uint64_t mfb_bar_phys(IOPCIDevice *pci, UInt8 reg)
{
    IODeviceMemory *m = pci->getDeviceMemoryWithRegister(reg);
    return m ? (uint64_t)m->getPhysicalAddress() : 0;
}

/* Мост от C-ядра к методу класса: провайдер scanout-FB (nv_gsp_fb_provider_t). */
static int mfb_fb_alloc_cb(void *ctx, uint32_t w, uint32_t h, uint32_t pitch,
                           uint64_t *out_gpu_addr, uint32_t *out_target, void **out_cpu_va)
{
    MilcorixFB *self = (MilcorixFB *)ctx;
    if (!self) return -1;
    return self->allocScanoutFb(w, h, pitch, out_gpu_addr, out_target, out_cpu_va);
}

/*
 * Трассировка обращений IOGraphics/WindowServer к нам. Ключевой диагностический
 * вопрос при чёрном экране: «мы вообще подключены к композитору или система нас
 * игнорирует?». Без этих отметок в журнале ответить нечем. Каждая точка
 * печатается ОДИН раз — иначе WindowServer зальёт лог за секунды.
 */
#define MFB_TRACE_ONCE(tag, ...)                          \
    do {                                                  \
        static bool _seen = false;                        \
        if (!_seen) { _seen = true;                       \
            mfb_log(nullptr, "MilcorixFB[trace] " tag "\n", ##__VA_ARGS__); } \
    } while (0)

/* Стадия из boot-arg `milcorix=N`. По умолчанию — OFF: свежеустановленный kext
   не должен менять поведение машины, пока его явно не попросили. */
static uint32_t mfb_read_boot_uint(const char *key, uint32_t def, uint32_t max)
{
    int val = 0;
    if (PE_parse_boot_argn(key, &val, sizeof(val))) {
        if (val >= 0 && (uint32_t)val <= max) return (uint32_t)val;
    }
    return def;
}

bool MilcorixFB::mapBars(void)
{
    fBar0Map = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fBar0Map) { IOLog("MilcorixFB: BAR0 map failed\n"); return false; }
    fBar0 = (volatile void *)fBar0Map->getVirtualAddress();
    mfb_log(nullptr, "MilcorixFB: BAR0 @%p len=0x%llx\n", fBar0,
            (unsigned long long)fBar0Map->getLength());

    /* BAR1 — апертура VRAM. Не критично для bring-up'а (без неё пойдём через
       системную память), поэтому провал маппинга не фатален. */
    fBar1Map = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1);
    if (fBar1Map) {
        fBar1     = (volatile void *)fBar1Map->getVirtualAddress();
        fBar1Len  = (uint64_t)fBar1Map->getLength();
        fBar1Phys = mfb_bar_phys(fPci, kIOPCIConfigBaseAddress1);
        mfb_log(nullptr, "MilcorixFB: BAR1 @%p phys=0x%llx len=0x%llx (%llu МиБ)\n",
                fBar1, (unsigned long long)fBar1Phys,
                (unsigned long long)fBar1Len, (unsigned long long)(fBar1Len >> 20));
    } else {
        mfb_log(nullptr, "MilcorixFB: BAR1 не замаплен — путь через системную память\n");
    }
    return true;
}

/*
 * probeBar1Identity — жив ли identity-маппинг «BAR1+X ↔ VRAM offset X».
 *
 * UEFI GOP на Ada кладёт консольный фреймбуфер во VRAM и открывает его через
 * BAR1 identity-маппингом. Но BAR1 транслируется GMMU, а его таблицы после
 * инициализации GSP-RM принадлежат уже прошивке — верить в маппинг «по
 * документации» нельзя, его нужно ИЗМЕРИТЬ. Пишем метку во VRAM через окно
 * PRAMIN и смотрим, видно ли её по BAR1 на том же смещении. Проверяем оба конца
 * диапазона: начало страницы может быть отображено, а хвост — уже нет.
 *
 * Состояние восстанавливается дважды: исходные слова кладутся назад, И регистр
 * окна PRAMIN (0x1700) возвращается в прежнее положение. Второе — обязательно:
 * оркестратор кэширует базу окна в своей переменной, и если подвинуть окно у
 * него за спиной, все его последующие обращения уйдут не по тем адресам.
 */
bool MilcorixFB::probeBar1Identity(uint64_t vramOff, uint64_t len)
{
    if (!fBar1 || !fBar0 || len < 4) return false;
    if (vramOff + len > fBar1Len) {
        mfb_log(nullptr, "MilcorixFB: BAR1 окно %llu МиБ мало под FB (%llu КиБ @0x%llx)\n",
                (unsigned long long)(fBar1Len >> 20), (unsigned long long)(len >> 10),
                (unsigned long long)vramOff);
        return false;
    }

    nv_mmio_t io;
    io.ctx = (void *)fBar0; io.rd = mfb_rd; io.wr = mfb_wr;
    io.udelay = mfb_udelay; io.log = mfb_log;
    uint64_t win = ~0ull;   /* заставит pramin_aim записать регистр на первом же доступе */

    const uint32_t savedWindow = mfb_rd((void *)fBar0, NV_PBUS_BAR0_WINDOW);

    const uint32_t kMagicA = 0x4D494C43u;   /* 'MILC' */
    const uint32_t kMagicB = 0x46423031u;   /* 'FB01' */
    uint64_t offs[2]  = { vramOff, vramOff + len - 4u };
    uint32_t magic[2] = { kMagicA, kMagicB };

    bool ok = true;
    for (int i = 0; i < 2 && ok; i++) {
        uint32_t orig = nv_pramin_rd32(&io, &win, offs[i]);
        nv_pramin_wr32(&io, &win, offs[i], magic[i]);
        /* Чтение по BAR1 — MMIO, компилятор не должен его выбрасывать. */
        uint32_t seen = *(volatile uint32_t *)((volatile uint8_t *)fBar1 + offs[i]);
        nv_pramin_wr32(&io, &win, offs[i], orig);
        if (seen != magic[i]) {
            mfb_log(nullptr, "MilcorixFB: BAR1 identity НЕ подтверждён @0x%llx (записали 0x%08x, видим 0x%08x)\n",
                    (unsigned long long)offs[i], magic[i], seen);
            ok = false;
        }
    }

    /* Вернуть окно PRAMIN как было — см. комментарий выше. */
    mfb_wr((void *)fBar0, NV_PBUS_BAR0_WINDOW, savedWindow);

    if (ok)
        mfb_log(nullptr, "MilcorixFB: BAR1 identity ПОДТВЕРЖДЁН для VRAM 0x%llx..0x%llx\n",
                (unsigned long long)vramOff, (unsigned long long)(vramOff + len));
    return ok;
}

/*
 * consoleFbOffsetInBar1 — где внутри BAR1 лежит консольный буфер, который
 * оставил UEFI GOP.
 *
 * Это не догадка: штатный «немой» фреймбуфер macOS (IOBootNDRV) берёт ровно
 * этот адрес — `PE_state.video.v_baseAddr` с обнулёнными двумя младшими битами —
 * и ищет BAR, в диапазон которого он попадает. Значит и мы получаем точное
 * смещение, а не предположение «наверное, ноль». Возврат ~0 — адрес недоступен
 * или лежит вне BAR1.
 */
uint64_t MilcorixFB::consoleFbOffsetInBar1(void)
{
    IOPlatformExpert *pe = getPlatform();
    if (!pe || !fBar1Phys || !fBar1Len) return ~0ull;

    PE_Video info;
    bzero(&info, sizeof(info));
    if (pe->getConsoleInfo(&info) != kIOReturnSuccess) return ~0ull;

    uint64_t base = (uint64_t)info.v_baseAddr & ~3ull;   /* младшие биты — флаги */
    if (!base || base < fBar1Phys) return ~0ull;
    uint64_t off = base - fBar1Phys;
    if (off >= fBar1Len) return ~0ull;

    mfb_log(nullptr, "MilcorixFB: консоль EFI @phys=0x%llx = BAR1+0x%llx, %lux%lu pitch=%lu bpp=%lu\n",
            (unsigned long long)base, (unsigned long long)off,
            (unsigned long)info.v_width, (unsigned long)info.v_height,
            (unsigned long)info.v_rowBytes, (unsigned long)info.v_depth);
    return off;
}

/*
 * allocDmaArena — физически непрерывная арена под весь bring-up (fwimage ~36 МиБ +
 * radix3 + bootloader + libos-логи + shm + rmargs). IOMMU выключен
 * (DisableIoMapper=true в OpenCore), поэтому GPU видит ФИЗИЧЕСКИЙ адрес → буфер
 * должен быть непрерывным, и его физадрес = IOVA для GSP. Маска 32 бита держит
 * арену в нижних 4 ГиБ (GSP DMA по sysmem, как ARENA_IOVA на Linux).
 */
bool MilcorixFB::allocDmaArena(void)
{
    if (fDmaBuf) return true;

    IOOptionBits opts = kIODirectionInOut | kIOMemoryPhysicallyContiguous;
    mach_vm_address_t physMask = 0x00000000FFFFF000ULL;

    fDmaBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, opts, NV_DMA_ARENA_SIZE, physMask);
    if (!fDmaBuf) { mfb_log(nullptr, "MilcorixFB: DMA-арена alloc FAIL\n"); return false; }

    if (fDmaBuf->prepare() != kIOReturnSuccess) {
        mfb_log(nullptr, "MilcorixFB: DMA-арена prepare FAIL\n");
        fDmaBuf->release(); fDmaBuf = nullptr; return false;
    }

    fArenaVa = fDmaBuf->getBytesNoCopy();
    IOByteCount segLen = 0;
    addr64_t phys = fDmaBuf->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    fArenaPhys = (uint64_t)phys;
    fArenaSize = NV_DMA_ARENA_SIZE;

    if (!fArenaVa || !fArenaPhys || segLen < NV_DMA_ARENA_SIZE) {
        mfb_log(nullptr, "MilcorixFB: DMA-арена не непрерывна (segLen=0x%llx)\n",
                (unsigned long long)segLen);
        fDmaBuf->complete(); fDmaBuf->release(); fDmaBuf = nullptr; return false;
    }

    bzero(fArenaVa, NV_DMA_ARENA_SIZE);
    mfb_log(nullptr, "MilcorixFB: DMA-арена VA=%p phys=0x%llx size=0x%llx\n",
            fArenaVa, (unsigned long long)fArenaPhys, (unsigned long long)fArenaSize);
    return true;
}

/*
 * allocScanoutFb — выделить scanout-фреймбуфер В СИСТЕМНОЙ ПАМЯТИ под конкретный
 * режим. Зовётся переносимым core уже после разбора EDID.
 *
 * Почему не VRAM: апертура, которую мы отдаём IOFramebuffer, мапится как обычная
 * физическая память хоста. VRAM в это пространство не отображён (CPU достаёт её
 * только через 1-МиБ окно PRAMIN), поэтому FB обязан быть настоящей RAM. Дисплей
 * читает его по PCIe — ctx-dma с апертурой SYSMEM (когерентно, snooped), так что
 * записи CPU видны движку без явного flush'а.
 */
int MilcorixFB::allocScanoutFb(uint32_t w, uint32_t h, uint32_t pitch,
                               uint64_t *outGpuAddr, uint32_t *outTarget, void **outCpuVa)
{
    if (!outGpuAddr || !outTarget) return -1;
    *outGpuAddr = 0; *outTarget = NV_CTXDMA_TARGET_VRAM;
    if (outCpuVa) *outCpuVa = nullptr;
    if (!w || !h || !pitch) return -1;

    uint64_t bytes = (uint64_t)pitch * (uint64_t)h;
    bytes = (bytes + 0xFFFFull) & ~0xFFFFull;    /* округление до 64 КиБ */
    if (bytes > 128ull * 1024ull * 1024ull) {
        mfb_log(nullptr, "MilcorixFB: режим %ux%u требует %llu МиБ — отказ\n",
                w, h, (unsigned long long)(bytes >> 20));
        return -1;
    }

    freeScanoutFb();

    /* --- Путь 1: VRAM через identity-окно BAR1 ---
       Берём VRAM offset 0 — именно там UEFI GOP держит консольный фреймбуфер, то
       есть это единственное место, про которое достоверно известно, что оно
       отображено в BAR1. Заодно приятное свойство: если наш modeset не
       состоится, по этому адресу продолжит сканироваться консоль EFI. */
    if (fFbMode != MILCORIX_FBMODE_SYSMEM && fBar1) {
        uint64_t kVramOff = consoleFbOffsetInBar1();
        if (kVramOff == ~0ull) {
            /* Платформа адрес не дала — пробуем начало окна: UEFI GOP кладёт
               консоль в начало VRAM, и IOBootNDRV рассчитывает на то же. */
            kVramOff = 0;
        }
        if (probeBar1Identity(kVramOff, bytes)) {
            fFbGpuAddr       = kVramOff;
            fFbTarget        = NV_CTXDMA_TARGET_VRAM;
            fApertureCpuPhys = fBar1Phys + kVramOff;
            fApertureLen     = bytes;

            *outGpuAddr = kVramOff;
            *outTarget  = NV_CTXDMA_TARGET_VRAM;
            if (outCpuVa) *outCpuVa = (void *)((volatile uint8_t *)fBar1 + kVramOff);

            mfb_log(nullptr, "MilcorixFB: scanout-FB во VRAM@0x%llx через BAR1, "
                             "CPU-апертура phys=0x%llx %ux%u pitch=%u (%llu КиБ)\n",
                    (unsigned long long)kVramOff, (unsigned long long)fApertureCpuPhys,
                    w, h, pitch, (unsigned long long)(bytes >> 10));
            return 0;
        }
    }
    if (fFbMode == MILCORIX_FBMODE_BAR1) {
        mfb_log(nullptr, "MilcorixFB: запрошен только BAR1, а identity-окна нет — отказ\n");
        return -1;
    }

    /* --- Путь 2: системная память, дисплей читает её по PCIe --- */
    IOOptionBits opts = kIODirectionInOut | kIOMemoryPhysicallyContiguous;
    mach_vm_address_t physMask = 0x00000000FFFFF000ULL;   /* < 4 ГиБ, как арена */

    fFbBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, opts, bytes, physMask);
    if (!fFbBuf) {
        mfb_log(nullptr, "MilcorixFB: scanout-FB alloc FAIL (%llu байт)\n",
                (unsigned long long)bytes);
        return -1;
    }
    if (fFbBuf->prepare() != kIOReturnSuccess) {
        mfb_log(nullptr, "MilcorixFB: scanout-FB prepare FAIL\n");
        fFbBuf->release(); fFbBuf = nullptr; return -1;
    }

    void *va = fFbBuf->getBytesNoCopy();
    IOByteCount segLen = 0;
    addr64_t phys = fFbBuf->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    if (!va || !phys || (uint64_t)segLen < bytes) {
        mfb_log(nullptr, "MilcorixFB: scanout-FB не непрерывен (segLen=0x%llx нужно 0x%llx)\n",
                (unsigned long long)segLen, (unsigned long long)bytes);
        freeScanoutFb();
        return -1;
    }

    bzero(va, bytes);
    fFbGpuAddr       = (uint64_t)phys;
    fFbTarget        = NV_CTXDMA_TARGET_SYSMEM;
    fApertureCpuPhys = (uint64_t)phys;      /* та же память — CPU и GPU видят одно */
    fApertureLen     = bytes;

    mfb_log(nullptr, "MilcorixFB: scanout-FB в СИСТЕМНОЙ памяти VA=%p phys=0x%llx %ux%u pitch=%u (%llu КиБ)\n",
            va, (unsigned long long)phys, w, h, pitch, (unsigned long long)(bytes >> 10));

    *outGpuAddr = (uint64_t)phys;
    *outTarget  = NV_CTXDMA_TARGET_SYSMEM;
    if (outCpuVa) *outCpuVa = va;
    return 0;
}

void MilcorixFB::freeScanoutFb(void)
{
    if (fFbBuf) {
        fFbBuf->complete();
        fFbBuf->release();
        fFbBuf = nullptr;
    }
    fFbGpuAddr = 0; fFbTarget = 0;
    fApertureCpuPhys = 0; fApertureLen = 0;
}

bool MilcorixFB::gspBringUp(void)
{
    if (!allocDmaArena()) return false;

    nv_mmio_t io;
    io.ctx    = (void *)fBar0;
    io.rd     = mfb_rd;
    io.wr     = mfb_wr;
    io.udelay = mfb_udelay;
    io.log    = mfb_log;

    /* Арена: IOMMU выключен → GPU видит физадрес, dma_addr == fArenaPhys. */
    nv_dma_arena_t arena;
    nv_dma_arena_init(&arena, (uint8_t *)fArenaVa, fArenaPhys, fArenaSize);

    /* Физ. BAR/PCI-id для GspSystemInfo — из IOPCIDevice (аналог sysfs на Linux). */
    nv_gsp_pci_info_t pci;
    pci.bar0 = mfb_bar_phys(fPci, kIOPCIConfigBaseAddress0);
    pci.bar1 = mfb_bar_phys(fPci, kIOPCIConfigBaseAddress1);
    pci.bar3 = mfb_bar_phys(fPci, kIOPCIConfigBaseAddress3);
    pci.devid = ((uint64_t)fPci->getBusNumber() << 8)
              | (uint64_t)(((fPci->getDeviceNumber() & 0x1f) << 3) | (fPci->getFunctionNumber() & 7));

    /* Провайдер FB: на стадии BRINGUP просим НЕ трогать вывод, на FULL — выдаём
       буфер в системной памяти под нужный режим. */
    nv_gsp_fb_provider_t fbp;
    fbp.ctx          = this;
    fbp.alloc        = (fStage >= MILCORIX_STAGE_FULL) ? mfb_fb_alloc_cb : nullptr;
    fbp.skip_modeset = (fStage < MILCORIX_STAGE_FULL) ? 1 : 0;

    /* dbg=nullptr: в ядре некуда дампить бинарные логи, и это же отключает
       секундные диагностические паузы, которые повесили бы старт. */
    nv_gsp_scanout_t scan;
    /* С этого момента прошивка живёт в арене — освобождать её больше нельзя. */
    fGspRunning = true;
    int rc = nv_gsp_bringup(&io, &arena, &pci, /*dbg=*/nullptr, &scan, &fbp);
    if (rc != 0) {
        mfb_log(nullptr, "MilcorixFB: gspBringUp FAIL (rc=%d)\n", rc);
        return false;
    }

    if (scan.ok) {
        /* ЗАЩИТА: апертуру публикуем только если МЫ САМИ её посчитали в
           allocScanoutFb. Адрес из core — это адрес для GPU (возможно VRAM), и
           в getApertureRange ему делать нечего. */
        if (!fApertureCpuPhys || !fApertureLen) {
            mfb_log(nullptr, "MilcorixFB: modeset прошёл, но CPU-апертуры нет — не публикуем\n");
            fModeset = false;
            return true;
        }
        fWidth   = scan.width;
        fHeight  = scan.height;
        fPitch   = scan.pitch;
        fModeset = true;
        if (scan.edid_ok) {
            memcpy(fEdid, scan.edid, sizeof(fEdid));
            fEdidOk = true;
        }
        mfb_log(nullptr, "MilcorixFB: gspBringUp OK — GSP-RM active, scanout %ux%u pitch=%u "
                         "gpu=0x%llx target=%u, CPU-апертура phys=0x%llx\n",
                fWidth, fHeight, fPitch, (unsigned long long)scan.fb_phys, scan.fb_target,
                (unsigned long long)fApertureCpuPhys);
    } else {
        mfb_log(nullptr, "MilcorixFB: gspBringUp OK — GSP-RM active, modeset не выполнен%s\n",
                fStage < MILCORIX_STAGE_FULL ? " (стадия bring-up)" : " (нет EDID/SOR?)");
    }
    return true;
}

bool MilcorixFB::modeset(uint32_t w, uint32_t h)
{
    /* Слой 5 (modeset+scanout на нативный режим из EDID) выполняется внутри
       nv_gsp_bringup единым потоком. Отдельного re-modeset на произвольный wxh
       пока нет — перечисляем один режим (нативный), поэтому сюда приходит он же. */
    if (!fModeset) {
        mfb_log(nullptr, "MilcorixFB: modeset %ux%u — голова не поднята\n", w, h);
        return false;
    }
    if (w != fWidth || h != fHeight) {
        mfb_log(nullptr, "MilcorixFB: modeset %ux%u не поддержан (нативный %ux%u)\n",
                w, h, fWidth, fHeight);
        return false;
    }
    return true;
}

bool MilcorixFB::start(IOService *provider)
{
    /* Обнулить состояние (ivar'ы kext не гарантированно занулены). */
    fPci = nullptr; fBar0Map = nullptr; fBar0 = nullptr; fFbMem = nullptr;
    fBar1Map = nullptr; fBar1 = nullptr; fBar1Phys = 0; fBar1Len = 0;
    fDmaBuf = nullptr;  fArenaVa = nullptr; fArenaPhys = 0; fArenaSize = 0;
    fFbBuf = nullptr;   fFbGpuAddr = 0;     fFbTarget = 0;
    fApertureCpuPhys = 0; fApertureLen = 0;
    fModeset = false;   fEdidOk = false;    fGspRunning = false;
    fWidth = 1280; fHeight = 1024; fPitch = fWidth * 4u;

    fStage  = mfb_read_boot_uint("milcorix",   MILCORIX_STAGE_OFF,   MILCORIX_STAGE_FULL);
    fFbMode = mfb_read_boot_uint("milcorixfb", MILCORIX_FBMODE_AUTO, MILCORIX_FBMODE_SYSMEM);
    if (fStage == MILCORIX_STAGE_OFF) {
        /* Явно выключен — молча уступаем EFI-фреймбуферу. Это дефолт: установка
           kext'а сама по себе не должна менять поведение машины. */
        IOLog("MilcorixFB: выключен (boot-arg milcorix=0 или не задан) — не подключаюсь\n");
        return false;
    }

    mfb_klog_init();
    mfb_klog_printf("=== MilcorixFB: старт, стадия %u, режим FB %u ===\n", fStage, fFbMode);
    mfb_klog_status("start");

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) return false;

    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);   /* GSP DMA читает арену из sysmem */
    if (!mapBars()) { mfb_klog_status("fail:bar0"); mfb_klog_flush(); teardown(); return false; }

    /* --- Железо поднимаем ДО super::start(): пока мы не знаем, что получили
       годный фреймбуфер, становиться фреймбуфером системы нельзя. --- */
    bool ok = gspBringUp();

    if (!ok) {
        mfb_log(nullptr, "MilcorixFB: bring-up провален — НЕ подключаюсь, "
                         "экран остаётся за EFI-фреймбуфером\n");
        mfb_klog_status("fail:bringup");
        mfb_klog_flush();
        teardown();
        return false;
    }

    if (fStage < MILCORIX_STAGE_FULL) {
        /* Диагностическая стадия: GSP поднят, вывод не тронут. Фреймбуфером не
           становимся — так «GSP работает» проверяется без риска для картинки. */
        mfb_log(nullptr, "MilcorixFB: стадия %u пройдена (GSP поднят, вывод не тронут) — "
                         "не подключаюсь как фреймбуфер\n", fStage);
        mfb_klog_status("ok:bringup-only");
        mfb_klog_flush();
        teardown();
        return false;
    }

    if (!fModeset || !fApertureCpuPhys || !fApertureLen) {
        mfb_log(nullptr, "MilcorixFB: годной scanout-апертуры нет (modeset=%d apert=0x%llx) — "
                         "НЕ подключаюсь\n", (int)fModeset,
                (unsigned long long)fApertureCpuPhys);
        mfb_klog_status("fail:no-aperture");
        mfb_klog_flush();
        teardown();
        return false;
    }

    if (!super::start(provider)) {
        mfb_log(nullptr, "MilcorixFB: IOFramebuffer::start FAIL\n");
        mfb_klog_status("fail:iofb-start");
        mfb_klog_flush();
        return false;
    }

    mfb_log(nullptr, "MilcorixFB: start OK (RTX 4070S, milcorix-1.0) — фреймбуфер %ux%u @0x%llx\n",
            fWidth, fHeight, (unsigned long long)fApertureCpuPhys);
    {
        char st[128];
        snprintf(st, sizeof(st), "ok:fb %ux%u pitch=%u phys=0x%llx",
                 fWidth, fHeight, fPitch, (unsigned long long)fApertureCpuPhys);
        mfb_klog_status(st);
    }
    mfb_klog_flush();
    return true;
}

/*
 * teardown — отпустить ресурсы хоста на неуспешных путях. Арена освобождается
 * ТОЛЬКО если GSP-RM не стартовал: после старта прошивка продолжает писать в неё
 * по DMA, и возврат этих страниц ядру = порча чужой памяти.
 */
void MilcorixFB::teardown(void)
{
    freeScanoutFb();
    if (!fGspRunning) {
        freeDmaArena();
    } else if (fDmaBuf) {
        mfb_log(nullptr, "MilcorixFB: DMA-арена НЕ освобождается — GSP-RM работает и пишет в неё\n");
    }
    if (fFbMem)   { fFbMem->release();   fFbMem = nullptr; }
    if (fBar1Map) { fBar1Map->release(); fBar1Map = nullptr; fBar1 = nullptr; }
    if (fBar0Map) { fBar0Map->release(); fBar0Map = nullptr; fBar0 = nullptr; }
}

void MilcorixFB::freeDmaArena(void)
{
    if (fDmaBuf) {
        fDmaBuf->complete();
        fDmaBuf->release();
        fDmaBuf = nullptr;
    }
    fArenaVa = nullptr; fArenaPhys = 0; fArenaSize = 0;
}

void MilcorixFB::stop(IOService *provider)
{
    mfb_klog_flush();
    teardown();
    mfb_klog_free();
    super::stop(provider);
}

// --- Перечисление режимов (один — нативный из EDID) ---
IOItemCount MilcorixFB::getDisplayModeCount(void)
{
    MFB_TRACE_ONCE("getDisplayModeCount — система перечисляет режимы");
    return 1;
}

IOReturn MilcorixFB::getDisplayModes(IODisplayModeID *allModes)
{
    if (!allModes) return kIOReturnBadArgument;
    allModes[0] = 1;   // id режима 1
    return kIOReturnSuccess;
}

IOReturn MilcorixFB::getInformationForDisplayMode(IODisplayModeID mode,
                                                  IODisplayModeInformation *info)
{
    if (!info) return kIOReturnBadArgument;
    mfb_klog_flush_lazy();
    bzero(info, sizeof(*info));
    info->nominalWidth  = fWidth;
    info->nominalHeight = fHeight;
    info->refreshRate   = 60 << 16;   // 60 Гц fixed-point
    info->maxDepthIndex = 0;
    return kIOReturnSuccess;
}

UInt64 MilcorixFB::getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) { return 0; }

const char * MilcorixFB::getPixelFormats(void)
{
    // 32bpp ARGB (X8R8G8B8) — как наш FB.
    static const char fmt[] = IO32BitDirectPixels "\0";
    return fmt;
}

IOReturn MilcorixFB::getPixelInformation(IODisplayModeID, IOIndex,
                                         IOPixelAperture aperture,
                                         IOPixelInformation *pi)
{
    if (aperture != kIOFBSystemAperture) return kIOReturnUnsupportedMode;
    if (!pi) return kIOReturnBadArgument;
    bzero(pi, sizeof(*pi));
    pi->bytesPerRow      = fPitch;
    pi->bytesPerPlane    = 0;
    pi->bitsPerPixel     = 32;
    pi->pixelType        = kIORGBDirectPixels;
    pi->componentCount   = 3;
    pi->bitsPerComponent = 8;
    pi->componentMasks[0] = 0x00FF0000;  // R
    pi->componentMasks[1] = 0x0000FF00;  // G
    pi->componentMasks[2] = 0x000000FF;  // B
    pi->activeWidth      = fWidth;
    pi->activeHeight     = fHeight;
    strncpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
    return kIOReturnSuccess;
}

IOReturn MilcorixFB::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
    if (mode)  *mode  = 1;
    if (depth) *depth = 0;
    return kIOReturnSuccess;
}

IOReturn MilcorixFB::setDisplayMode(IODisplayModeID, IOIndex)
{
    MFB_TRACE_ONCE("setDisplayMode — система выбирает режим");
    mfb_klog_flush_lazy();
    return modeset(fWidth, fHeight) ? kIOReturnSuccess : kIOReturnError;
}

IODeviceMemory * MilcorixFB::getApertureRange(IOPixelAperture aperture)
{
    MFB_TRACE_ONCE("getApertureRange(%u) — система запросила буфер кадра", (unsigned)aperture);
    if (aperture != kIOFBSystemAperture) return nullptr;
    /* Отдаём ТОЛЬКО буфер в системной памяти. Без этой проверки сюда уехал бы
       VRAM-адрес, а WindowServer писал бы пиксели в чужую физическую RAM. */
    if (!fApertureCpuPhys || !fApertureLen) {
        IOLog("MilcorixFB: getApertureRange — CPU-апертуры нет, отказ\n");
        return nullptr;
    }
    /* Длина должна быть НЕ МЕНЬШЕ bytesPerRow*activeHeight, иначе IOGraphics
       молча отвергает апертуру («VENDOR_BUG: length insufficient»). Штатный
       IONDRVFramebuffer отдаёт с запасом в 128 байт — делаем так же. */
    uint64_t len = (uint64_t)fPitch * fHeight + 128ull;
    if (len > fApertureLen) len = fApertureLen;
    if (!fFbMem)
        fFbMem = IODeviceMemory::withRange(fApertureCpuPhys, len);
    if (fFbMem) fFbMem->retain();
    return fFbMem;
}

/* --- EDID для IOGraphics: монитор должен опознаться, иначе система подставит
   дефолтный профиль и может отказаться от нашего режима. --- */
bool MilcorixFB::hasDDCConnect(IOIndex connectIndex)
{
    (void)connectIndex;
    MFB_TRACE_ONCE("hasDDCConnect -> %d (EDID %s)", (int)fEdidOk,
                   fEdidOk ? "есть" : "нет");
    return fEdidOk;
}

/*
 * isConsoleDevice — считает ли система нашу карту загрузочным дисплеем. Ответ
 * решает, отдадут ли нам консоль/рабочий стол, поэтому его надо ВИДЕТЬ, а не
 * угадывать. Поведение не меняем: возвращаем решение базового класса, но
 * записываем и его, и наличие свойства "AAPL,boot-display" у PCI-устройства
 * (именно по нему базовый класс и решает).
 */
bool MilcorixFB::isConsoleDevice(void)
{
    /* Базовый IOFramebuffer::isConsoleDevice() возвращает false — то есть кто
       НЕ переопределил этот метод, тот консоль и не получает. Поэтому все
       «немые» фреймбуферы (IOBootNDRV, AppleBochVGAFB, MacHyperVFramebuffer)
       его переопределяют. Мы единственный фреймбуфер этой карты и сами
       программируем её вывод — значит консоль наша. */
    bool prop = (fPci && fPci->getProperty("AAPL,boot-display") != nullptr);
    MFB_TRACE_ONCE("isConsoleDevice -> 1 (AAPL,boot-display у PCI: %d)", (int)prop);
    return true;
}

/*
 * setAttribute — важен ровно один селектор. IOFramebuffer::close() шлёт
 * kIOWindowServerActiveAttribute со значением kIOWSAA_Unaccelerated (0), и его
 * надо принять: это штатный режим «рисует CPU, GPU не участвует», ровно наш
 * случай. Остальное отдаём базовому классу.
 */
IOReturn MilcorixFB::setAttribute(IOSelect attribute, uintptr_t value)
{
    if (attribute == kIOWindowServerActiveAttribute) {
        MFB_TRACE_ONCE("WindowServer подключился (kIOWindowServerActive=%lu)",
                       (unsigned long)value);
        mfb_klog_flush_lazy();
        return kIOReturnSuccess;
    }
    return super::setAttribute(attribute, value);
}

/*
 * Гамма и палитра: аппаратной таблицы у нас (пока) нет, но отказ на этих
 * вызовах IOGraphics трактует как неисправность. Формат у нас всегда
 * 32-битный direct-color, палитра не используется — принимаем и игнорируем.
 */
IOReturn MilcorixFB::setGammaTable(UInt32, UInt32, UInt32, void *)
{
    MFB_TRACE_ONCE("setGammaTable — принято (аппаратной гаммы нет)");
    return kIOReturnSuccess;
}

IOReturn MilcorixFB::setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits)
{
    return kIOReturnSuccess;
}

IOReturn MilcorixFB::getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                 IOSelect blockType, IOOptionBits options,
                                 UInt8 *data, IOByteCount *length)
{
    (void)connectIndex; (void)options;
    if (!fEdidOk || !data || !length) return kIOReturnUnsupported;
    if (blockType != kIODDCBlockTypeEDID) return kIOReturnUnsupported;
    if (blockNumber != 1) return kIOReturnUnsupported;   /* только блок 0 (нумерация с 1) */

    IOByteCount want = *length;
    if (want > sizeof(fEdid)) want = sizeof(fEdid);
    memcpy(data, fEdid, want);
    *length = want;
    return kIOReturnSuccess;
}
