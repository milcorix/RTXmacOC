/*
 * MilcorixFB.h — subclass IOFramebuffer для вывода рабочего стола macOS через RTX 4070S
 * (Ada AD104, milcorix-1.0). Фаза 1 плана docs/macos-display-plan.md: НЕускоренный
 * фреймбуфер (отдельный путь от Metal, не упирается в Library Validation).
 *
 * Ядро GSP переносимо через nv_mmio_t (driver/gsp/*). Здесь — IOKit-обёртка: matching
 * PCI 10DE:2783, маппинг BAR0, публикация scanout-апертуры и режимов (из EDID) для
 * WindowServer, setDisplayMode → GSP-modeset.
 *
 * ВАЖНО: сетевой/дисплейный код внутри — доверенная своя реализация; данные из EDID и
 * регистров трактуем как непроверенные (валидация в парсерах).
 */
#ifndef MILCORIX_FB_H
#define MILCORIX_FB_H

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>

// Идентификатор карты (Ada AD104, RTX 4070 Super).
#define MILCORIX_PCI_VENDOR   0x10DE
#define MILCORIX_PCI_DEVICE   0x2783

// Наши физ-адреса во VRAM (как в Linux-оркестраторе; будут вычисляться из FB-layout).
#define MILCORIX_FB_VRAM_PHYS   0x14000000ull   // scanout framebuffer во VRAM

class MilcorixFB : public IOFramebuffer
{
    OSDeclareDefaultStructors(MilcorixFB);

public:
    // --- IOService ---
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;

    // --- IOFramebuffer: перечисление/выбор режима (из EDID) ---
    virtual IOReturn enableController(void) override;
    virtual UInt64   getPixelFormatsForDisplayMode(IODisplayModeID displayMode,
                                                   IOIndex depth) override;
    virtual IOItemCount getDisplayModeCount(void) override;
    virtual IOReturn getDisplayModes(IODisplayModeID *allModes) override;
    virtual IOReturn getInformationForDisplayMode(IODisplayModeID displayMode,
                                                  IODisplayModeInformation *info) override;
    virtual IOReturn getPixelInformation(IODisplayModeID displayMode, IOIndex depth,
                                         IOPixelAperture aperture,
                                         IOPixelInformation *pixelInfo) override;
    virtual IOReturn getCurrentDisplayMode(IODisplayModeID *displayMode,
                                           IOIndex *depth) override;
    virtual IOReturn setDisplayMode(IODisplayModeID displayMode, IOIndex depth) override;

    // --- IOFramebuffer: апертура scanout-буфера ---
    virtual IODeviceMemory * getApertureRange(IOPixelAperture aperture) override;

    // --- IOFramebuffer: коннектор ---
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex,
                                               IOSelect attribute, uintptr_t *value) override;
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex,
                                               IOSelect attribute, uintptr_t value) override;
    virtual const char * getPixelFormats(void) override;

private:
    IOPCIDevice     *fPci;        // провайдер
    IOMemoryMap     *fBar0Map;    // маппинг регистров (BAR0)
    volatile void   *fBar0;       // база регистров
    IODeviceMemory  *fFbMem;      // scanout-апертура (VRAM/сис.память)
    uint32_t         fWidth, fHeight, fPitch;   // текущий режим (из EDID)

    bool  mapBars(void);
    bool  gspBringUp(void);       // слои 2–5 через переносимый core (nv_mmio_t)
    bool  modeset(uint32_t w, uint32_t h);  // GSP-modeset на режим
};

#endif /* MILCORIX_FB_H */
