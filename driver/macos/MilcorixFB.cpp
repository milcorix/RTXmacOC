/*
 * MilcorixFB.cpp — реализация IOFramebuffer для RTX 4070S (Фаза 1, скелет).
 * См. MilcorixFB.h и docs/macos-display-plan.md.
 *
 * Скелет: matching + маппинг BAR0 + каркас методов IOFramebuffer. GSP-boot/modeset
 * подключаются из переносимого core (driver/gsp/*) через nv_mmio_t (обёртки IOKit ниже).
 * Реальный scanout заводится после Фазы 0 (голова физически сканирует).
 */
#include "MilcorixFB.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
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
 * (Прототип nv_mmio_t — driver/gsp/*.h; тут показан контракт колбэков.)
 */
static uint32_t mfb_rd(void *ctx, uint32_t off)
{ return *(volatile uint32_t *)((volatile uint8_t *)ctx + off); }
static void mfb_wr(void *ctx, uint32_t off, uint32_t val)
{ *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = val; }
static void mfb_udelay(void *ctx, uint32_t us)
{ (void)ctx; IODelay(us); }
/* Трассировка оркестрации GSP → системный лог ядра. Core зовёт io->log в
   printf-стиле; IOLogv принимает va_list и делает форматирование в ядре. */
static void mfb_log(void *ctx, const char *fmt, ...)
{ (void)ctx; va_list ap; va_start(ap, fmt); IOLogv(fmt, ap); va_end(ap); }

/* Физ. база BAR по его config-регистру (для GspSystemInfo). getDeviceMemory* —
   get-конвенция: возвращённый объект не наш, освобождать не нужно. */
static uint64_t mfb_bar_phys(IOPCIDevice *pci, UInt8 reg)
{
    IODeviceMemory *m = pci->getDeviceMemoryWithRegister(reg);
    return m ? (uint64_t)m->getPhysicalAddress() : 0;
}

bool MilcorixFB::mapBars(void)
{
    fBar0Map = fPci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fBar0Map) { IOLog("MilcorixFB: BAR0 map failed\n"); return false; }
    fBar0 = (volatile void *)fBar0Map->getVirtualAddress();
    IOLog("MilcorixFB: BAR0 @%p len=0x%llx\n", fBar0,
          (unsigned long long)fBar0Map->getLength());
    return true;
}

/*
 * allocDmaArena — выделить физически непрерывную DMA-арену под весь bring-up
 * (fwimage ~36 МиБ + radix3 + bootloader + libos-логи + shm + rmargs). На Linux
 * это делал VFIO (VA→IOVA линейно). В macOS IOMMU выключен (DisableIoMapper=true
 * в OpenCore config), поэтому GPU видит ФИЗИЧЕСКИЙ адрес → буфер должен быть
 * физически непрерывным, и его физадрес = IOVA для GSP.
 *
 * IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kIOMemoryPhysicallyContiguous)
 * даёт непрерывный блок; маска 32 бита — GSP DMA-движок адресует sysmem внизу 4 ГиБ
 * (как ARENA_IOVA=0x10000000 на Linux). arena_alloc (переносимый nv_dma.h) — та же
 * арифметика поверх базы, платформа лишь поставляет (va, phys, size).
 */
bool MilcorixFB::allocDmaArena(void)
{
    if (fDmaBuf) return true;   // уже выделена

    IOOptionBits opts = kIODirectionInOut | kIOMemoryPhysicallyContiguous;
    // Маска физадреса: держим арену в нижних 4 ГиБ (GSP sysmem-DMA как на Linux).
    mach_vm_address_t physMask = 0x00000000FFFFF000ULL;

    fDmaBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, opts, NV_DMA_ARENA_SIZE, physMask);
    if (!fDmaBuf) { IOLog("MilcorixFB: DMA-арена alloc FAIL\n"); return false; }

    if (fDmaBuf->prepare() != kIOReturnSuccess) {
        IOLog("MilcorixFB: DMA-арена prepare FAIL\n");
        fDmaBuf->release(); fDmaBuf = nullptr; return false;
    }

    fArenaVa = fDmaBuf->getBytesNoCopy();
    IOByteCount segLen = 0;
    addr64_t phys = fDmaBuf->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);
    fArenaPhys = (uint64_t)phys;
    fArenaSize = NV_DMA_ARENA_SIZE;

    if (!fArenaVa || !fArenaPhys || segLen < NV_DMA_ARENA_SIZE) {
        IOLog("MilcorixFB: DMA-арена не непрерывна (segLen=0x%llx)\n",
              (unsigned long long)segLen);
        fDmaBuf->complete(); fDmaBuf->release(); fDmaBuf = nullptr; return false;
    }

    bzero(fArenaVa, NV_DMA_ARENA_SIZE);
    IOLog("MilcorixFB: DMA-арена VA=%p phys=0x%llx size=0x%llx\n",
          fArenaVa, (unsigned long long)fArenaPhys, (unsigned long long)fArenaSize);
    return true;
}

bool MilcorixFB::gspBringUp(void)
{
    // DMA-арена под весь bring-up (физически непрерывная, IOMMU off).
    if (!allocDmaArena()) return false;

    // Переносимый оркестратор (driver/gsp/gsp_bringup.c): FWSEC-FRTS → staging
    // GSP-RM → Booter → RPC → слои 3-5. Тот же код, что гоняет Linux-стенд;
    // платформа даёт лишь колбэки (BAR0/задержка/лог), арену и PCI-инфо.
    nv_mmio_t io;
    io.ctx    = (void *)fBar0;
    io.rd     = mfb_rd;
    io.wr     = mfb_wr;
    io.udelay = mfb_udelay;
    io.log    = mfb_log;

    // Арена: IOMMU выключен (DisableIoMapper=true), поэтому GPU видит физадрес,
    // и физ-адрес буфера == IOVA для GSP (dma_addr = fArenaPhys).
    nv_dma_arena_t arena;
    nv_dma_arena_init(&arena, (uint8_t *)fArenaVa, fArenaPhys, fArenaSize);

    // Физ. BAR/PCI-id для GspSystemInfo — из IOPCIDevice (аналог sysfs на Linux).
    nv_gsp_pci_info_t pci;
    pci.bar0 = mfb_bar_phys(fPci, kIOPCIConfigBaseAddress0);
    pci.bar1 = mfb_bar_phys(fPci, kIOPCIConfigBaseAddress1);
    pci.bar3 = mfb_bar_phys(fPci, kIOPCIConfigBaseAddress3);
    // devid = (bus<<8)|(dev<<3)|fn — как build_sysinfo на Linux.
    pci.devid = ((uint64_t)fPci->getBusNumber() << 8)
              | (uint64_t)(((fPci->getDeviceNumber() & 0x1f) << 3) | (fPci->getFunctionNumber() & 7));

    // Дамп libos-логов/shm нам в ядре некуда писать (нет FS) — не диагностируем.
    int rc = nv_gsp_bringup(&io, &arena, &pci, /*dbg=*/nullptr);
    if (rc != 0) {
        IOLog("MilcorixFB: gspBringUp FAIL (rc=%d)\n", rc);
        return false;
    }
    IOLog("MilcorixFB: gspBringUp OK — GSP-RM загружен, RISC-V active\n");
    return true;
}

bool MilcorixFB::modeset(uint32_t w, uint32_t h)
{
    // TODO(Фаза 0): GSP-modeset на режим wxh (build_core_sor/init/modeset + window image,
    // interlocked update). Требует рабочей активации головы (супервизор GSP).
    fWidth = w; fHeight = h; fPitch = w * 4u;
    IOLog("MilcorixFB: modeset %ux%u pitch=%u (TODO: GSP supervisor)\n", w, h, fPitch);
    return true;
}

bool MilcorixFB::start(IOService *provider)
{
    fPci = OSDynamicCast(IOPCIDevice, provider);
    if (!fPci) return false;
    if (!super::start(provider)) return false;

    // Обнулить состояние (ivar'ы kext не гарантированно занулены).
    fBar0Map = nullptr; fBar0 = nullptr; fFbMem = nullptr;
    fDmaBuf = nullptr;  fArenaVa = nullptr; fArenaPhys = 0; fArenaSize = 0;

    fPci->setMemoryEnable(true);
    fPci->setBusMasterEnable(true);   // GSP DMA читает арену из sysmem (BusMaster)
    if (!mapBars()) return false;

    // Дефолтный режим до чтения EDID (переопределится в enableController/modeset).
    fWidth = 1280; fHeight = 1024; fPitch = fWidth * 4u;

    IOLog("MilcorixFB: start OK (RTX 4070S, milcorix-1.0)\n");
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
    freeDmaArena();
    if (fFbMem)   { fFbMem->release();   fFbMem = nullptr; }
    if (fBar0Map) { fBar0Map->release(); fBar0Map = nullptr; }
    super::stop(provider);
}

IOReturn MilcorixFB::enableController(void)
{
    // Поднять GSP + дисплей, выбрать нативный режим из EDID, сделать modeset.
    if (!gspBringUp()) return kIOReturnError;
    if (!modeset(fWidth, fHeight)) return kIOReturnError;
    return kIOReturnSuccess;
}

// --- Перечисление режимов (пока один — нативный из EDID; расширим) ---
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
    return modeset(fWidth, fHeight) ? kIOReturnSuccess : kIOReturnError;
}

IODeviceMemory * MilcorixFB::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture) return nullptr;
    // Scanout-апертура: наш FB. На старте — VRAM-регион через BAR1 или сис.память.
    // TODO(Фаза 1): вернуть IODeviceMemory на реальную scanout-память (pitch*height).
    uint64_t len = (uint64_t)fPitch * fHeight;
    if (!fFbMem)
        fFbMem = IODeviceMemory::withRange(MILCORIX_FB_VRAM_PHYS, len);
    if (fFbMem) fFbMem->retain();
    return fFbMem;
}

IOReturn MilcorixFB::getAttributeForConnection(IOIndex, IOSelect attribute, uintptr_t *value)
{
    switch (attribute) {
        case kConnectionEnable:      if (value) *value = 1; return kIOReturnSuccess;
        case kConnectionFlags:       if (value) *value = kIOConnectionBuiltIn; return kIOReturnSuccess;
        case kConnectionSupportsHLDDCSense: return kIOReturnSuccess;  // есть DDC/EDID
        default: return super::getAttributeForConnection(0, attribute, value);
    }
}

IOReturn MilcorixFB::setAttributeForConnection(IOIndex, IOSelect attribute, uintptr_t value)
{
    return super::setAttributeForConnection(0, attribute, value);
}
