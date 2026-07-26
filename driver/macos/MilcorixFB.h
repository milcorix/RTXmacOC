/*
 * MilcorixFB.h — subclass IOFramebuffer для вывода рабочего стола macOS через RTX 4070S
 * (Ada AD104, milcorix-1.0). Фаза 1 плана docs/macos-display-plan.md: НЕускоренный
 * фреймбуфер (отдельный путь от Metal, не упирается в Library Validation).
 *
 * Ядро GSP переносимо через nv_mmio_t (driver/gsp/*). Здесь — IOKit-обёртка: matching
 * PCI 10DE:2783, маппинг BAR0, публикация scanout-апертуры и режимов (из EDID) для
 * WindowServer, setDisplayMode → GSP-modeset.
 *
 * --- ДВА ЖЁСТКИХ ПРАВИЛА, ВЫСТРАДАННЫХ НА ЖЕЛЕЗЕ ---
 *
 * 1. АДРЕСНЫЕ ПРОСТРАНСТВА НЕ ПУТАТЬ. Слой 5 на Linux кладёт scanout-FB во VRAM
 *    (GPU-локальный адрес) и пишет туда через окно PRAMIN. Отдать этот же адрес
 *    в getApertureRange НЕЛЬЗЯ: там он трактуется как ФИЗИЧЕСКИЙ адрес хоста, и
 *    WindowServer начинает писать пиксели в чужую системную память — чёрный экран
 *    плюс тихая порча RAM (именно так был убит dyld-кэш и получен boot-loop
 *    «Bad CPU type in executable»). Поэтому в macOS scanout-FB живёт в СИСТЕМНОЙ
 *    памяти, а дисплей читает его по PCIe (ctx-dma TARGET=SYSMEM). Апертура
 *    отдаётся ТОЛЬКО когда fFbSysmem==true.
 *
 * 2. ДРАЙВЕР ДОЛЖЕН УМЕТЬ НЕ ЗАПУСКАТЬСЯ. Драйвер дисплея, падающий на boot'е,
 *    превращает машину в кирпич без обратной связи. Поэтому активность задаётся
 *    boot-arg'ом milcorix=<стадия>, по умолчанию 0 (выключен), а весь трейс
 *    пишется на диск и в NVRAM (mfb_klog.h) — читается из Linux даже при чёрном
 *    экране.
 *
 * ВАЖНО: данные из EDID и регистров трактуем как непроверенные (валидация в парсерах).
 */
#ifndef MILCORIX_FB_H
#define MILCORIX_FB_H

#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

// Идентификатор карты (Ada AD104, RTX 4070 Super).
#define MILCORIX_PCI_VENDOR   0x10DE
#define MILCORIX_PCI_DEVICE   0x2783

/*
 * Стадии (boot-arg `milcorix=N`). Растут по степени вмешательства в железо —
 * так один ребут даёт один ответ, а не кашу из нескольких гипотез.
 */
#define MILCORIX_STAGE_OFF      0u   /* не подключаться вообще (ДЕФОЛТ) */
#define MILCORIX_STAGE_BRINGUP  1u   /* GSP bring-up без modeset: проверить слои 2-4,
                                        дисплей остаётся на EFI-фреймбуфере */
#define MILCORIX_STAGE_FULL     2u   /* + modeset и публикация нашей апертуры */

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

    // --- IOFramebuffer: коннектор + EDID ---
    virtual IOReturn getAttributeForConnection(IOIndex connectIndex,
                                               IOSelect attribute, uintptr_t *value) override;
    virtual IOReturn setAttributeForConnection(IOIndex connectIndex,
                                               IOSelect attribute, uintptr_t value) override;
    virtual const char * getPixelFormats(void) override;
    virtual bool     hasDDCConnect(IOIndex connectIndex) override;
    virtual IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                 IOSelect blockType, IOOptionBits options,
                                 UInt8 *data, IOByteCount *length) override;

    // --- колбэк провайдера FB для переносимого core (см. nv_gsp_fb_provider_t) ---
    int  allocScanoutFb(uint32_t w, uint32_t h, uint32_t pitch,
                        uint64_t *outPhys, void **outVa);

private:
    IOPCIDevice          *fPci;        // провайдер
    IOMemoryMap          *fBar0Map;    // маппинг регистров (BAR0)
    volatile void        *fBar0;       // база регистров
    IODeviceMemory       *fFbMem;      // апертура scanout (ТОЛЬКО системная память)
    IOBufferMemoryDescriptor *fDmaBuf; // DMA-арена bring-up (физически непрерывная)
    void                 *fArenaVa;    // CPU-адрес арены
    uint64_t              fArenaPhys;  // физ-адрес арены (= IOVA для GSP, IOMMU off)
    uint64_t              fArenaSize;  // размер арены

    IOBufferMemoryDescriptor *fFbBuf;  // scanout-FB в СИСТЕМНОЙ памяти
    void                 *fFbVa;       // CPU-адрес FB (сюда пишет WindowServer)
    uint64_t              fFbPhys;     // физ-адрес FB (его читает дисплей по PCIe)
    uint64_t              fFbBytes;    // размер выделенного FB

    uint32_t              fStage;      // MILCORIX_STAGE_*
    uint32_t              fWidth, fHeight, fPitch;   // текущий режим (из EDID)
    bool                  fFbSysmem;   // FB реально в системной памяти → апертуру можно отдавать
    bool                  fModeset;    // слой 5 запрограммирован (scan.ok)
    uint8_t               fEdid[128];  // EDID блок 0 активного монитора
    bool                  fEdidOk;

    bool  mapBars(void);
    bool  allocDmaArena(void);    // выделить физически непрерывную DMA-арену
    void  freeDmaArena(void);     // освободить DMA-арену
    void  freeScanoutFb(void);    // освободить scanout-FB
    bool  gspBringUp(void);       // слои 2–5 через переносимый core (nv_mmio_t)
    bool  modeset(uint32_t w, uint32_t h);  // GSP-modeset на режим
};

#endif /* MILCORIX_FB_H */
