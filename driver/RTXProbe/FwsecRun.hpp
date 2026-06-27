/*
 * FwsecRun.hpp — kext-оркестратор запуска FWSEC-FRTS на GSP-Falcon (слой 2, задача 5).
 *
 * Связывает портируемые модули (driver/gsp/falcon, fwsec_patch, fwsec_locate) с
 * реальным железом: BAR0 (MMIO) + DMA-буфер (IOBufferMemoryDescriptor). Читает
 * VBIOS из теневой ROM в BAR0, извлекает и патчит FWSEC, грузит на Falcon и
 * запускает, проверяет результат (mbox0==0 и WPR2 set).
 *
 * ВНИМАНИЕ: на железе ещё не проверялось (нет стенда). Логика сверена с nova-core.
 */
#ifndef RTXMACOC_FWSECRUN_HPP
#define RTXMACOC_FWSECRUN_HPP

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>

/*
 * Прогнать FWSEC-FRTS.
 *   pci        — устройство;
 *   bar0Base   — виртуальный адрес смапленного BAR0.
 * Регион WPR2/FRTS вычисляется внутри из объёма VRAM (fb_layout).
 *
 * Возвращает kIOReturnSuccess, если FWSEC завершился успешно (mbox0==0) и WPR2
 * выставлен; иначе — код ошибки.
 */
IOReturn RTXRunFwsecFrts(IOPCIDevice *pci, volatile void *bar0Base);

#endif /* RTXMACOC_FWSECRUN_HPP */
