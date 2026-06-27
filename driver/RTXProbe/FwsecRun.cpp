/*
 * FwsecRun.cpp — реализация оркестратора FWSEC-FRTS (см. FwsecRun.hpp).
 *
 * Последовательность (design.md P1, fwsec.rs/falcon.rs):
 *   1. читаем VBIOS из теневой ROM (BAR0 + 0x300000);
 *   2. находим дескриптор FWSEC (fwsec_locate) и парсим его (fwsec_patch);
 *   3. копируем ucode (imem||dmem) в coherent DMA-буфер;
 *   4. патчим: FRTS-команда + HS-подпись (по fuse-версии);
 *   5. reset Falcon (GA102) -> DMA-load -> boot(mbox0=0);
 *   6. проверяем mbox0==0 и WPR2 set.
 *
 * На железе не проверялось. Регистры/последовательности — docs/PORTING-MAP.md.
 */
#include "FwsecRun.hpp"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

extern "C" {
#include "../gsp/falcon.h"
#include "../gsp/fwsec_patch.h"
#include "../gsp/fwsec_locate.h"
#include "../gsp/fb_layout.h"
}

/* --- glue: nv_mmio_t поверх смапленного BAR0 --- */
static uint32_t bar_rd(void *ctx, uint32_t off)
{
    return *(volatile uint32_t *)((volatile uint8_t *)ctx + off);
}
static void bar_wr(void *ctx, uint32_t off, uint32_t val)
{
    *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = val;
}
static void bar_udelay(void *ctx, uint32_t usec)
{
    (void)ctx;
    IODelay(usec);
}

#define FWSEC_TIMEOUT_US (2u * 1000u * 1000u) /* 2с, как в falcon.rs */
#define FALCON_DESC_V3_SIZE 44u

/*
 * Патч ucode в DMA-буфере + reset/load/boot/проверка. Без goto: только early-return.
 * ucode — указатель в DMA-буфере (уже скопирован), dma_addr — его физ. адрес.
 * vbios нужен для источника подписи (sigs array).
 */
static IOReturn fwsec_patch_and_run(const nv_mmio_t *io,
                                    const uint8_t *vbios, uint32_t vbios_len,
                                    const nv_fwsec_location_t *loc,
                                    const nv_fwsec_desc_t *desc,
                                    uint8_t *ucode, size_t ucode_size,
                                    uint64_t dma_addr,
                                    uint64_t frtsAddr, uint64_t frtsSize)
{
    /* 4. FRTS-команда. */
    if (nv_fwsec_patch_frts(ucode, ucode_size, desc, frtsAddr, frtsSize) != NV_FWSEC_OK) {
        IOLog("RTXFwsec: FRTS patch failed\n");
        return kIOReturnError;
    }

    /* 4b. Подпись (если требуется). */
    if (desc->signature_count != 0) {
        uint32_t fuse_ver = 0;
        if (nv_falcon_signature_fuse_version_ga102(io, desc->engine_id_mask,
                                                   desc->ucode_id, &fuse_ver) != NV_OK) {
            IOLog("RTXFwsec: fuse version read failed\n");
            return kIOReturnError;
        }
        uint32_t sig_idx = 0;
        if (nv_fwsec_select_signature_index(desc, fuse_ver, &sig_idx) != NV_FWSEC_OK) {
            IOLog("RTXFwsec: no matching signature (fuse %u)\n", fuse_ver);
            return kIOReturnError;
        }
        size_t sigs_base = loc->desc_abs + FALCON_DESC_V3_SIZE;
        size_t sig_at    = sigs_base + (size_t)sig_idx * NV_BCRT30_RSA3K_SIG_SIZE;
        if (sig_at + NV_BCRT30_RSA3K_SIG_SIZE > vbios_len) {
            IOLog("RTXFwsec: signature out of VBIOS bounds\n");
            return kIOReturnError;
        }
        if (nv_fwsec_patch_signature(ucode, ucode_size, desc, vbios + sig_at) != NV_FWSEC_OK) {
            IOLog("RTXFwsec: signature patch failed\n");
            return kIOReturnError;
        }
    }

    /* 5. reset -> dma_load -> boot. */
    uint32_t boot0 = bar_rd(io->ctx, 0x0); /* PMC_BOOT_0 */
    if (nv_falcon_reset_ga102(io, NV_PGSP_FALCON_BASE, NV_PGSP_FALCON2_BASE,
                              boot0, FWSEC_TIMEOUT_US) != NV_OK) {
        IOLog("RTXFwsec: falcon reset failed\n");
        return kIOReturnError;
    }

    nv_dma_load_target_t imem_sec = { 0, desc->imem_phys_base, desc->imem_load_size };
    nv_dma_load_target_t dmem     = { desc->imem_load_size, desc->dmem_phys_base, desc->dmem_load_size };
    if (nv_falcon_dma_load_ga102(io, NV_PGSP_FALCON_BASE, NV_PGSP_FALCON2_BASE,
                                 dma_addr, &imem_sec, &dmem,
                                 desc->pkc_data_offset, desc->engine_id_mask,
                                 desc->ucode_id, 0 /*boot_addr*/, FWSEC_TIMEOUT_US) != NV_OK) {
        IOLog("RTXFwsec: dma_load failed\n");
        return kIOReturnError;
    }

    uint32_t mbox0 = 0xFFFFFFFFu;
    if (nv_falcon_boot(io, NV_PGSP_FALCON_BASE, 1, 0, 0, 0,
                       FWSEC_TIMEOUT_US, &mbox0) != NV_OK) {
        IOLog("RTXFwsec: falcon boot timed out\n");
        return kIOReturnError;
    }

    /* 6. Проверка результата. */
    uint64_t wpr_lo = 0, wpr_hi = 0;
    int wpr_set = nv_wpr2_is_set(io, &wpr_lo, &wpr_hi);
    IOLog("RTXFwsec: FWSEC done, mbox0=0x%08x, WPR2 set=%d [0x%llx..0x%llx]\n",
          mbox0, wpr_set, wpr_lo, wpr_hi);

    if (mbox0 == 0 && wpr_set) {
        IOLog("RTXFwsec: *** FWSEC-FRTS OK, WPR2 created ***\n");
        return kIOReturnSuccess;
    }
    IOLog("RTXFwsec: FWSEC reported error or WPR2 not set\n");
    return kIOReturnError;
}

IOReturn RTXRunFwsecFrts(IOPCIDevice *pci, volatile void *bar0Base)
{
    (void)pci;
    if (bar0Base == nullptr) {
        IOLog("RTXFwsec: bar0Base is null\n");
        return kIOReturnBadArgument;
    }

    nv_mmio_t io;
    io.ctx    = (void *)bar0Base;
    io.rd     = bar_rd;
    io.wr     = bar_wr;
    io.udelay = bar_udelay;

    /* Регион WPR2/FRTS из объёма VRAM (fb_layout). */
    uint64_t frtsAddr = 0, frtsSize = 0;
    if (nv_fb_compute_frts(&io, &frtsAddr, &frtsSize) != 0) {
        IOLog("RTXFwsec: failed to compute FRTS region (VRAM size?)\n");
        return kIOReturnError;
    }
    IOLog("RTXFwsec: FRTS region addr=0x%llx size=0x%llx\n", frtsAddr, frtsSize);

    /* 1. Читаем VBIOS из теневой ROM в BAR0. */
    const uint32_t vbios_len = NV_VBIOS_SCAN_MAX;
    uint8_t *vbios = (uint8_t *)IOMalloc(vbios_len);
    if (vbios == nullptr) {
        IOLog("RTXFwsec: IOMalloc(vbios) failed\n");
        return kIOReturnNoMemory;
    }
    for (uint32_t i = 0; i < vbios_len; i += 4) {
        uint32_t w = bar_rd(io.ctx, NV_ROM_SHADOW_BASE + i);
        vbios[i + 0] = (uint8_t)w;
        vbios[i + 1] = (uint8_t)(w >> 8);
        vbios[i + 2] = (uint8_t)(w >> 16);
        vbios[i + 3] = (uint8_t)(w >> 24);
    }

    /* 2. Находим и парсим дескриптор FWSEC. */
    nv_fwsec_location_t loc;
    nv_fwsec_desc_t desc;
    if (nv_fwsec_locate(vbios, vbios_len, &loc) != NV_FWSEC_LOC_OK) {
        IOLog("RTXFwsec: FWSEC not located in VBIOS\n");
        IOFree(vbios, vbios_len);
        return kIOReturnNotFound;
    }
    if (nv_fwsec_desc_parse(vbios + loc.desc_abs, vbios_len - loc.desc_abs, &desc) != NV_FWSEC_OK) {
        IOLog("RTXFwsec: desc parse failed\n");
        IOFree(vbios, vbios_len);
        return kIOReturnError;
    }

    size_t ucode_off  = loc.desc_abs + desc.hdr_size;
    size_t ucode_size = (size_t)desc.imem_load_size + (size_t)desc.dmem_load_size;
    if (ucode_size == 0 || ucode_off > vbios_len || ucode_size > vbios_len - ucode_off) {
        IOLog("RTXFwsec: ucode out of VBIOS bounds\n");
        IOFree(vbios, vbios_len);
        return kIOReturnError;
    }

    /* 3. Coherent DMA-буфер под ucode (выравниваем на 256). */
    size_t dma_size = (ucode_size + 255) & ~(size_t)255;
    IOBufferMemoryDescriptor *dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIODirectionOut | kIOMemoryPhysicallyContiguous | kIOMapInhibitCache,
        dma_size,
        0x0001FFFFFFFFFFFFULL /* 49-битная маска DMA (DMATRFBASE/1) */);
    if (dma == nullptr) {
        IOLog("RTXFwsec: DMA buffer alloc failed\n");
        IOFree(vbios, vbios_len);
        return kIOReturnNoMemory;
    }
    if (dma->prepare() != kIOReturnSuccess) {
        IOLog("RTXFwsec: DMA prepare failed\n");
        dma->release();
        IOFree(vbios, vbios_len);
        return kIOReturnError;
    }

    uint8_t *ucode = (uint8_t *)dma->getBytesNoCopy();
    memcpy(ucode, vbios + ucode_off, ucode_size);

    IOByteCount segLen = 0;
    addr64_t dma_addr = dma->getPhysicalSegment(0, &segLen, kIOMemoryMapperNone);

    IOReturn ret = fwsec_patch_and_run(&io, vbios, vbios_len, &loc, &desc,
                                       ucode, ucode_size, (uint64_t)dma_addr,
                                       frtsAddr, frtsSize);

    dma->complete();
    dma->release();
    IOFree(vbios, vbios_len);
    return ret;
}
