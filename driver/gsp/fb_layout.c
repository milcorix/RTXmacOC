/*
 * fb_layout.c — реализация расчёта региона FRTS (см. fb_layout.h).
 * Порт nova-core fb.rs::FbLayout::new + fb/hal/ga102.rs/tu102.rs (Ada).
 * Пересказано для соответствия лицензии.
 */
#include "fb_layout.h"

uint64_t nv_fb_vidmem_size(const nv_mmio_t *io)
{
    /* GA102/Ada: NV_USABLE_FB_SIZE_IN_MB.usable_fb_size() = value * 1MiB. */
    uint32_t mb = io->rd(io->ctx, NV_USABLE_FB_SIZE_IN_MB);
    return (uint64_t)mb * (uint64_t)0x100000u;
}

int nv_fb_supports_display(const nv_mmio_t *io)
{
    /* display_enabled = !display_disabled (bit0). */
    uint32_t f = io->rd(io->ctx, NV_FUSE_STATUS_OPT_DISPLAY);
    return (f & 0x1u) == 0u;
}

int nv_fb_compute_frts(const nv_mmio_t *io, uint64_t *out_addr, uint64_t *out_size)
{
    if (!io || !out_addr || !out_size) return -1;

    uint64_t fb_end = nv_fb_vidmem_size(io);
    if (fb_end < (uint64_t)NV_PRAMIN_SIZE) return -1;

    /* base = fb.end - NV_PRAMIN_SIZE (1MiB). */
    uint64_t base = fb_end - (uint64_t)NV_PRAMIN_SIZE;

    /* vga_workspace.start (fb.rs). */
    uint64_t vga_start;
    if (nv_fb_supports_display(io)) {
        uint32_t r = io->rd(io->ctx, NV_PDISP_VGA_WORKSPACE_BASE);
        int valid = (r >> 3) & 0x1;              /* status_valid bit3 */
        if (valid) {
            uint64_t addr = ((uint64_t)(r >> 8)) << 16; /* addr[31:8] << 16 */
            if (addr < base)
                vga_start = fb_end - (uint64_t)NV_VBIOS_WORKSPACE_SIZE; /* 128KiB */
            else
                vga_start = addr;
        } else {
            vga_start = base;
        }
    } else {
        vga_start = base;
    }

    /* frts_base = align_down(vga_start, 128KiB) - frts_size. */
    uint64_t aligned = vga_start & ~((uint64_t)NV_FRTS_ALIGN - 1u);
    uint64_t frts_size = (uint64_t)NV_FRTS_SIZE;
    if (aligned < frts_size) return -1;
    uint64_t frts_base = aligned - frts_size;

    *out_addr = frts_base;
    *out_size = frts_size;
    return 0;
}
