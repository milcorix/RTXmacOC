/*
 * fb_layout.h — расчёт региона WPR2/FRTS из объёма VRAM (слой 2/3, для FWSEC-FRTS).
 *
 * Портировано из nova-core fb.rs (FbLayout) + fb/hal/ga102.rs + tu102.rs для Ada.
 * Нужно, чтобы вычислить frts_addr/frts_size, передаваемые в FWSEC-FRTS, без
 * внешнего параметра. Пересказано для соответствия лицензии (PORTING-MAP).
 *
 * Только MMIO через nv_mmio_t (falcon.h).
 */
#ifndef RTXMACOC_FB_LAYOUT_H
#define RTXMACOC_FB_LAYOUT_H

#include <stdint.h>
#include "falcon.h"

/* Размер usable VRAM в байтах (GA102: NV_USABLE_FB_SIZE_IN_MB * 1MiB). */
uint64_t nv_fb_vidmem_size(const nv_mmio_t *io);

/* Поддерживается ли дисплей (fuse display_disabled == 0). */
int nv_fb_supports_display(const nv_mmio_t *io);

/*
 * Вычислить регион FRTS (= нижняя часть WPR2) по логике fb.rs:
 *   base = vidmem - 1MiB; vga_start учитывает NV_PDISP_VGA_WORKSPACE_BASE;
 *   frts_base = align_down(vga_start, 128KiB) - frts_size; frts_size = 1MiB.
 * Возвращает 0 при успехе и заполняет out_addr/out_size.
 */
int nv_fb_compute_frts(const nv_mmio_t *io, uint64_t *out_addr, uint64_t *out_size);

#endif /* RTXMACOC_FB_LAYOUT_H */
