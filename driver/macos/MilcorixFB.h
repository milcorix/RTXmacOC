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
 *    «Bad CPU type in executable»). Поэтому CPU-адрес апертуры kext считает САМ
 *    (fApertureCpuPhys) — либо это окно BAR1, через которое VRAM видна процессору,
 *    либо буфер в системной памяти. Адрес, вернувшийся из ядра GSP, в апертуру
 *    не попадает никогда.
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

extern "C" {
#include "../gsp/gsp_bringup.h"   /* nv_gsp_gpu_ctx_t — живой контекст исполнения */
}

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

/*
 * Откуда берётся scanout-буфер (boot-arg `milcorixfb=N`).
 *
 * BAR1 — предпочтительный путь. На Ada EFI GOP кладёт консольный фреймбуфер во
 * VRAM и показывает его через BAR1 с identity-маппингом (VRAM offset X виден по
 * BAR1+X). То есть CPU-видимая линейная апертура уже существует, а scanout идёт
 * из VRAM — ровно тот путь, что доказан на железе под Linux. Живость окна
 * проверяется на старте (probeBar1Identity), потому что после инициализации
 * GSP-RM маппинг мог измениться.
 *
 * SYSMEM — запасной путь: буфер в обычной системной памяти, дисплей читает его
 * по PCIe (ctx-dma PHYSICAL_PCI_COHERENT). Не требует BAR1 вообще, но нагружает
 * шину и на dGPU в открытых исходниках не подтверждён.
 */
#define MILCORIX_FBMODE_AUTO    0u   /* BAR1, если identity-окно живо; иначе sysmem */
#define MILCORIX_FBMODE_BAR1    1u   /* только BAR1 (без него — отказ) */
#define MILCORIX_FBMODE_SYSMEM  2u   /* только системная память */

class MilcorixFB : public IOFramebuffer
{
    OSDeclareDefaultStructors(MilcorixFB);

public:
    // --- IOService ---
    virtual bool     start(IOService *provider) override;
    virtual void     stop(IOService *provider) override;

    /*
     * ВАЖНО: enableController СОЗНАТЕЛЬНО НЕ переопределён.
     *
     * IOFramebuffer помечает фреймбуфер как `dead`, если enableController вернул
     * что-либо кроме успеха — а мёртвый фреймбуфер это гарантированный чёрный
     * экран. Проверять готовность здесь незачем: железо поднимается в start(), и
     * фреймбуфером мы становимся только когда всё готово. Штатный
     * AppleBochVGAFB (единственный прямой subclass IOFramebuffer, который Apple
     * сама поставляет для Intel в Sequoia) этот метод тоже не переопределяет.
     */

    // --- IOFramebuffer: перечисление/выбор режима (из EDID) ---
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

    virtual const char * getPixelFormats(void) override;

    /*
     * get/setAttributeForConnection тоже НЕ переопределяем. Причина конкретная:
     * IOFramebuffer::updateOnline() считает фреймбуфер ОФФЛАЙНОВЫМ, если запрос
     * kConnectionCheckEnable вернул успех со значением 0. Ошибка (или
     * «не поддерживаю») трактуется как «онлайн», то есть безопаснее. Апертуру
     * это не затрагивает, а риск потерять рабочий стол убирает.
     */

    // --- Консоль и «немой» фреймбуфер ---
    virtual bool     isConsoleDevice(void) override;
    virtual IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    virtual IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount,
                                   UInt32 dataWidth, void *data) override;
    virtual IOReturn setCLUTWithEntries(IOColorEntry *colors, UInt32 index,
                                        UInt32 numEntries, IOOptionBits options) override;

    // --- EDID активного монитора ---
    virtual bool     hasDDCConnect(IOIndex connectIndex) override;
    virtual IOReturn getDDCBlock(IOIndex connectIndex, UInt32 blockNumber,
                                 IOSelect blockType, IOOptionBits options,
                                 UInt8 *data, IOByteCount *length) override;

    // --- колбэк провайдера FB для переносимого core (см. nv_gsp_fb_provider_t) ---
    int  allocScanoutFb(uint32_t w, uint32_t h, uint32_t pitch,
                        uint64_t *outGpuAddr, uint32_t *outTarget, void **outCpuVa);

private:
    IOPCIDevice          *fPci;        // провайдер
    IOMemoryMap          *fBar0Map;    // маппинг регистров (BAR0)
    volatile void        *fBar0;       // база регистров
    IOMemoryMap          *fBar1Map;    // маппинг апертуры VRAM (BAR1)
    volatile void        *fBar1;       // CPU-база окна BAR1
    uint64_t              fBar1Phys;   // физ. база BAR1 (её отдаём как апертуру)
    uint64_t              fBar1Len;    // длина окна BAR1 (на Ada обычно 256 МиБ)

    IODeviceMemory       *fFbMem;      // опубликованная апертура scanout
    IOBufferMemoryDescriptor *fDmaBuf; // DMA-арена bring-up (физически непрерывная)
    void                 *fArenaVa;    // CPU-адрес арены
    uint64_t              fArenaPhys;  // физ-адрес арены (= IOVA для GSP, IOMMU off)
    uint64_t              fArenaSize;  // размер арены

    IOBufferMemoryDescriptor *fFbBuf;  // scanout-FB, если он в СИСТЕМНОЙ памяти
    uint64_t              fFbGpuAddr;  // адрес, из которого читает дисплей (VRAM-offset либо физ.)
    uint32_t              fFbTarget;   // апертура ctx-dma (NV_CTXDMA_TARGET_*)

    /* CPU-сторона апертуры: физический адрес, который получает ОС. Считается
       САМИМ kext'ом и никогда не выводится из адреса, вернувшегося из core —
       так VRAM-адрес не может случайно уехать в getApertureRange. */
    uint64_t              fApertureCpuPhys;
    uint64_t              fApertureLen;

    uint32_t              fStage;      // MILCORIX_STAGE_*
    uint32_t              fFbMode;     // MILCORIX_FBMODE_*
    uint32_t              fWidth, fHeight, fPitch;   // текущий режим (из EDID)
    bool                  fModeset;    // слой 5 запрограммирован (scan.ok)
    uint8_t               fEdid[128];  // EDID блок 0 активного монитора
    bool                  fEdidOk;

    /* GSP-RM запущен и продолжает работать. Пока это так, DMA-арену освобождать
       НЕЛЬЗЯ: в ней лежат очереди RPC и логи, куда прошивка пишет по DMA. Отдать
       эти страницы обратно ядру — значит получить порчу чужой памяти, то есть
       ровно тот класс бага, из-за которого всё и началось. */
    bool                  fGspRunning;

    /* Живой контекст исполнения на GPU (канал GPFIFO + движок копирования),
       оставшийся после bring-up'а. Основа для слоя 6: через него платформа
       отправляет собственную работу, не пересоздавая канал. */
    nv_gsp_gpu_ctx_t      fGpu;

    bool  mapBars(void);
    bool  allocDmaArena(void);    // выделить физически непрерывную DMA-арену
    void  freeDmaArena(void);     // освободить DMA-арену
    void  freeScanoutFb(void);    // освободить scanout-FB
    bool  probeBar1Identity(uint64_t vramOff, uint64_t len);  // живо ли identity-окно BAR1
    uint64_t consoleFbOffsetInBar1(void);   // где EFI GOP оставил консольный буфер
    void  teardown(void);         // отпустить ресурсы хоста (арену — только если GSP не запущен)
    bool  gspBringUp(void);       // слои 2–5 через переносимый core (nv_mmio_t)
    void  runGpuSelfTest(void);   // слой 6: реальная GPU-копия через наш же канал
    bool  modeset(uint32_t w, uint32_t h);  // GSP-modeset на режим
};

#endif /* MILCORIX_FB_H */
