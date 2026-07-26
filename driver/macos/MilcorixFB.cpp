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
#include <pexpert/pexpert.h>
#include <stdarg.h>

/* Переносимое ядро GSP — чистый C, компилируется как kernel-C. В C++-TU его
   символы (nv_gsp_bringup и т.д.) нужно объявлять с C-линковкой, иначе clang++
   ищет манглированное имя и линковка kext падает. */
extern "C" {
#include "../gsp/nv_dma.h"
#include "../gsp/falcon.h"
#include "../gsp/gsp_bringup.h"
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
                           uint64_t *out_phys, void **out_va)
{
    MilcorixFB *self = (MilcorixFB *)ctx;
    if (!self) return -1;
    return self->allocScanoutFb(w, h, pitch, out_phys, out_va);
}

/* Стадия из boot-arg `milcorix=N`. По умолчанию — OFF: свежеустановленный kext
   не должен менять поведение машины, пока его явно не попросили. */
static uint32_t mfb_read_stage(void)
{
    uint32_t stage = MILCORIX_STAGE_OFF;
    int val = 0;
    if (PE_parse_boot_argn("milcorix", &val, sizeof(val))) {
        if (val >= 0 && val <= (int)MILCORIX_STAGE_FULL) stage = (uint32_t)val;
    }
    return stage;
}

bool MilcorixFB::mapBars(void)
{
    fBar0Map = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fBar0Map) { IOLog("MilcorixFB: BAR0 map failed\n"); return false; }
    fBar0 = (volatile void *)fBar0Map->getVirtualAddress();
    mfb_log(nullptr, "MilcorixFB: BAR0 @%p len=0x%llx\n", fBar0,
            (unsigned long long)fBar0Map->getLength());
    return true;
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
                               uint64_t *outPhys, void **outVa)
{
    if (!outPhys) return -1;
    *outPhys = 0;
    if (outVa) *outVa = nullptr;
    if (!w || !h || !pitch) return -1;

    uint64_t bytes = (uint64_t)pitch * (uint64_t)h;
    /* Округляем до страницы — дескриптор ctx-dma адресует с гранулярностью 256 б,
       но маппинг в userspace всё равно постраничный. */
    bytes = (bytes + 0xFFFFull) & ~0xFFFFull;
    if (bytes > 128ull * 1024ull * 1024ull) {
        mfb_log(nullptr, "MilcorixFB: режим %ux%u требует %llu МиБ — отказ\n",
                w, h, (unsigned long long)(bytes >> 20));
        return -1;
    }

    if (fFbBuf) freeScanoutFb();

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
    fFbVa = va; fFbPhys = (uint64_t)phys; fFbBytes = bytes;

    mfb_log(nullptr, "MilcorixFB: scanout-FB в СИСТЕМНОЙ памяти VA=%p phys=0x%llx %ux%u pitch=%u (%llu КиБ)\n",
            va, (unsigned long long)fFbPhys, w, h, pitch,
            (unsigned long long)(bytes >> 10));

    *outPhys = fFbPhys;
    if (outVa) *outVa = va;
    return 0;
}

void MilcorixFB::freeScanoutFb(void)
{
    if (fFbBuf) {
        fFbBuf->complete();
        fFbBuf->release();
        fFbBuf = nullptr;
    }
    fFbVa = nullptr; fFbPhys = 0; fFbBytes = 0; fFbSysmem = false;
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
    fbp.sysmem       = 1;
    fbp.skip_modeset = (fStage < MILCORIX_STAGE_FULL) ? 1 : 0;

    /* dbg=nullptr: в ядре некуда дампить бинарные логи, и это же отключает
       секундные диагностические паузы, которые повесили бы старт. */
    nv_gsp_scanout_t scan;
    int rc = nv_gsp_bringup(&io, &arena, &pci, /*dbg=*/nullptr, &scan, &fbp);
    if (rc != 0) {
        mfb_log(nullptr, "MilcorixFB: gspBringUp FAIL (rc=%d)\n", rc);
        return false;
    }

    if (scan.ok) {
        /* ЗАЩИТА: апертуру принимаем ТОЛЬКО если FB реально в системной памяти.
           Адрес VRAM здесь означал бы запись пикселей в чужую RAM. */
        if (!scan.fb_sysmem) {
            mfb_log(nullptr, "MilcorixFB: scanout во VRAM (0x%llx) — апертуру НЕ публикуем "
                             "(этот адрес не в адресном пространстве хоста)\n",
                    (unsigned long long)scan.fb_phys);
            fModeset = false; fFbSysmem = false;
            return true;
        }
        fFbPhys   = scan.fb_phys;
        fWidth    = scan.width;
        fHeight   = scan.height;
        fPitch    = scan.pitch;
        fFbSysmem = true;
        fModeset  = true;
        if (scan.edid_ok) {
            memcpy(fEdid, scan.edid, sizeof(fEdid));
            fEdidOk = true;
        }
        mfb_log(nullptr, "MilcorixFB: gspBringUp OK — GSP-RM active, scanout %ux%u pitch=%u fb=0x%llx (sysmem)\n",
                fWidth, fHeight, fPitch, (unsigned long long)fFbPhys);
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
    fDmaBuf = nullptr;  fArenaVa = nullptr; fArenaPhys = 0; fArenaSize = 0;
    fFbBuf = nullptr;   fFbVa = nullptr;    fFbPhys = 0;    fFbBytes = 0;
    fFbSysmem = false;  fModeset = false;   fEdidOk = false;
    fWidth = 1280; fHeight = 1024; fPitch = fWidth * 4u;

    fStage = mfb_read_stage();
    if (fStage == MILCORIX_STAGE_OFF) {
        /* Явно выключен — молча уступаем EFI-фреймбуферу. Это дефолт: установка
           kext'а сама по себе не должна менять поведение машины. */
        IOLog("MilcorixFB: выключен (boot-arg milcorix=0 или не задан) — не подключаюсь\n");
        return false;
    }

    mfb_klog_init();
    mfb_klog_printf("=== MilcorixFB: старт, стадия %u ===\n", fStage);
    mfb_klog_status("start");

    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) return false;

    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);   /* GSP DMA читает арену из sysmem */
    if (!mapBars()) { mfb_klog_status("fail:bar0"); mfb_klog_flush(); return false; }

    /* --- Железо поднимаем ДО super::start(): пока мы не знаем, что получили
       годный фреймбуфер, становиться фреймбуфером системы нельзя. --- */
    bool ok = gspBringUp();

    if (!ok) {
        mfb_log(nullptr, "MilcorixFB: bring-up провален — НЕ подключаюсь, "
                         "экран остаётся за EFI-фреймбуфером\n");
        mfb_klog_status("fail:bringup");
        mfb_klog_flush();
        freeScanoutFb(); freeDmaArena();
        if (fBar0Map) { fBar0Map->release(); fBar0Map = nullptr; }
        return false;
    }

    if (fStage < MILCORIX_STAGE_FULL) {
        /* Диагностическая стадия: GSP поднят, вывод не тронут. Фреймбуфером не
           становимся — так «GSP работает» проверяется без риска для картинки. */
        mfb_log(nullptr, "MilcorixFB: стадия %u пройдена (GSP поднят, вывод не тронут) — "
                         "не подключаюсь как фреймбуфер\n", fStage);
        mfb_klog_status("ok:bringup-only");
        mfb_klog_flush();
        return false;
    }

    if (!fModeset || !fFbSysmem || !fFbPhys) {
        mfb_log(nullptr, "MilcorixFB: годной scanout-апертуры нет (modeset=%d sysmem=%d) — "
                         "НЕ подключаюсь\n", (int)fModeset, (int)fFbSysmem);
        mfb_klog_status("fail:no-aperture");
        mfb_klog_flush();
        freeScanoutFb(); freeDmaArena();
        if (fBar0Map) { fBar0Map->release(); fBar0Map = nullptr; }
        return false;
    }

    if (!super::start(provider)) {
        mfb_log(nullptr, "MilcorixFB: IOFramebuffer::start FAIL\n");
        mfb_klog_status("fail:iofb-start");
        mfb_klog_flush();
        return false;
    }

    mfb_log(nullptr, "MilcorixFB: start OK (RTX 4070S, milcorix-1.0) — фреймбуфер %ux%u @0x%llx\n",
            fWidth, fHeight, (unsigned long long)fFbPhys);
    {
        char st[128];
        snprintf(st, sizeof(st), "ok:fb %ux%u pitch=%u phys=0x%llx",
                 fWidth, fHeight, fPitch, (unsigned long long)fFbPhys);
        mfb_klog_status(st);
    }
    mfb_klog_flush();
    return true;
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
    freeScanoutFb();
    freeDmaArena();
    if (fFbMem)   { fFbMem->release();   fFbMem = nullptr; }
    if (fBar0Map) { fBar0Map->release(); fBar0Map = nullptr; }
    mfb_klog_free();
    super::stop(provider);
}

IOReturn MilcorixFB::enableController(void)
{
    /* Железо уже поднято в start() — сюда мы доходим только когда есть годный
       scanout в системной памяти. Остаётся подтвердить готовность. */
    mfb_klog_flush_lazy();
    if (!fModeset || !fFbSysmem || !fFbPhys) return kIOReturnNotReady;
    return kIOReturnSuccess;
}

// --- Перечисление режимов (один — нативный из EDID) ---
IOItemCount MilcorixFB::getDisplayModeCount(void) { return 1; }

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
    mfb_klog_flush_lazy();
    return modeset(fWidth, fHeight) ? kIOReturnSuccess : kIOReturnError;
}

IODeviceMemory * MilcorixFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture) return nullptr;
    /* Отдаём ТОЛЬКО буфер в системной памяти. Без этой проверки сюда уехал бы
       VRAM-адрес, а WindowServer писал бы пиксели в чужую физическую RAM. */
    if (!fFbSysmem || !fFbPhys || !fFbBytes) {
        IOLog("MilcorixFB: getApertureRange — годного sysmem-FB нет, отказ\n");
        return nullptr;
    }
    uint64_t len = (uint64_t)fPitch * fHeight;
    if (len > fFbBytes) len = fFbBytes;
    if (!fFbMem)
        fFbMem = IODeviceMemory::withRange(fFbPhys, len);
    if (fFbMem) fFbMem->retain();
    return fFbMem;
}

IOReturn MilcorixFB::getAttributeForConnection(IOIndex, IOSelect attribute, uintptr_t *value)
{
    switch (attribute) {
        case kConnectionEnable:      if (value) *value = 1; return kIOReturnSuccess;
        case kConnectionFlags:       if (value) *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
        case kConnectionSupportsHLDDCSense:
            return fEdidOk ? kIOReturnSuccess : kIOReturnUnsupported;
        default: return super::getAttributeForConnection(0, attribute, value);
    }
}

IOReturn MilcorixFB::setAttributeForConnection(IOIndex, IOSelect attribute, uintptr_t value)
{
    mfb_klog_flush_lazy();
    return super::setAttributeForConnection(0, attribute, value);
}

/* --- EDID для IOGraphics: монитор должен опознаться, иначе система подставит
   дефолтный профиль и может отказаться от нашего режима. --- */
bool MilcorixFB::hasDDCConnect(IOIndex connectIndex)
{
    (void)connectIndex;
    return fEdidOk;
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
