/*
 * falcon.c — реализация примитивов Falcon (см. falcon.h).
 *
 * Последовательности портированы из nova-core (пересказано для лицензии):
 *   falcon.rs            — start/boot/dma_wr/dma_load
 *   falcon/hal/ga102.rs  — reset_eng/select_core/program_brom (Ampere/Ada)
 *   regs.rs              — смещения и поля регистров
 */
#include "falcon.h"

#define NV_POLL_STEP_US 10u
#define NV_DMA_BLOCK    256u   /* DMA-передачи кратны 256 байтам (falcon.rs) */

static inline uint32_t rd(const nv_mmio_t *io, uint32_t off) { return io->rd(io->ctx, off); }
static inline void     wr(const nv_mmio_t *io, uint32_t off, uint32_t v) { io->wr(io->ctx, off, v); }
static inline void     udelay(const nv_mmio_t *io, uint32_t us) { if (io->udelay) io->udelay(io->ctx, us); }

/* ===================== базовые примитивы ===================== */

void nv_falcon_reset_engine(const nv_mmio_t *io, uint32_t base)
{
    uint32_t eng = PFALCON(base, NV_PFALCON_FALCON_ENGINE_OFF);
    wr(io, eng, rd(io, eng) | NV_PFALCON_ENGINE_RESET);
    udelay(io, 10); /* regs.rs: <=10us */
    wr(io, eng, rd(io, eng) & ~NV_PFALCON_ENGINE_RESET);
    udelay(io, 10);
}

int nv_falcon_wait_mem_scrubbing(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us)
{
    uint32_t hwcfg2 = PFALCON(base, NV_PFALCON_FALCON_HWCFG2_OFF);
    uint32_t waited = 0;
    for (;;) {
        if ((rd(io, hwcfg2) & NV_PFALCON_HWCFG2_MEM_SCRUBBING) == 0)
            return NV_OK;
        if (waited >= timeout_us) return NV_ERR_TIMEOUT;
        udelay(io, NV_POLL_STEP_US);
        waited += NV_POLL_STEP_US;
    }
}

void nv_falcon_start(const nv_mmio_t *io, uint32_t base)
{
    uint32_t cpuctl = PFALCON(base, NV_PFALCON_FALCON_CPUCTL_OFF);
    if (rd(io, cpuctl) & NV_PFALCON_CPUCTL_ALIAS_EN)
        wr(io, PFALCON(base, NV_PFALCON_FALCON_CPUCTL_ALIAS_OFF), NV_PFALCON_CPUCTL_ALIAS_STARTCPU);
    else
        wr(io, cpuctl, NV_PFALCON_CPUCTL_STARTCPU);
}

int nv_falcon_wait_halted(const nv_mmio_t *io, uint32_t base, uint32_t timeout_us)
{
    uint32_t cpuctl = PFALCON(base, NV_PFALCON_FALCON_CPUCTL_OFF);
    uint32_t waited = 0;
    for (;;) {
        if (rd(io, cpuctl) & NV_PFALCON_CPUCTL_HALTED) return NV_OK;
        if (waited >= timeout_us) return NV_ERR_TIMEOUT;
        udelay(io, NV_POLL_STEP_US);
        waited += NV_POLL_STEP_US;
    }
}

void nv_falcon_write_mailbox0(const nv_mmio_t *io, uint32_t base, uint32_t v)
{ wr(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX0_OFF), v); }
void nv_falcon_write_mailbox1(const nv_mmio_t *io, uint32_t base, uint32_t v)
{ wr(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX1_OFF), v); }
uint32_t nv_falcon_read_mailbox0(const nv_mmio_t *io, uint32_t base)
{ return rd(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX0_OFF)); }
uint32_t nv_falcon_read_mailbox1(const nv_mmio_t *io, uint32_t base)
{ return rd(io, PFALCON(base, NV_PFALCON_FALCON_MAILBOX1_OFF)); }

int nv_falcon_boot(const nv_mmio_t *io, uint32_t base,
                   int set_mbox0, uint32_t mbox0,
                   int set_mbox1, uint32_t mbox1,
                   uint32_t timeout_us, uint32_t *out_mbox0)
{
    if (set_mbox0) nv_falcon_write_mailbox0(io, base, mbox0);
    if (set_mbox1) nv_falcon_write_mailbox1(io, base, mbox1);
    nv_falcon_start(io, base);
    int rc = nv_falcon_wait_halted(io, base, timeout_us);
    if (out_mbox0) *out_mbox0 = nv_falcon_read_mailbox0(io, base);
    return rc;
}

/* ===================== GA102/Ada HAL ===================== */

int nv_falcon_select_core_ga102(const nv_mmio_t *io, uint32_t falcon2_base, uint32_t timeout_us)
{
    uint32_t bcr = PFALCON(falcon2_base, NV_PRISCV_RISCV_BCR_CTRL_OFF);
    uint32_t v = rd(io, bcr);
    /* core_select bit4: Falcon=0, RISC-V=1. Если выбран RISC-V — переключаем. */
    if (v & (1u << 4)) {
        wr(io, bcr, 0); /* zeroed().with_core_select(Falcon) */
        uint32_t waited = 0;
        for (;;) {
            if (rd(io, bcr) & 0x1u) return NV_OK; /* valid bit0 */
            if (waited >= timeout_us) return NV_ERR_TIMEOUT;
            udelay(io, NV_POLL_STEP_US);
            waited += NV_POLL_STEP_US;
        }
    }
    return NV_OK;
}

void nv_falcon_program_brom_ga102(const nv_mmio_t *io, uint32_t falcon2_base,
                                  uint32_t pkc_data_offset, uint16_t engine_id_mask,
                                  uint8_t ucode_id)
{
    wr(io, PFALCON(falcon2_base, NV_PFALCON2_FALCON_BROM_PARAADDR_OFF), pkc_data_offset);
    wr(io, PFALCON(falcon2_base, NV_PFALCON2_FALCON_BROM_ENGIDMASK_OFF), (uint32_t)engine_id_mask);
    wr(io, PFALCON(falcon2_base, NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID_OFF), (uint32_t)ucode_id);
    wr(io, PFALCON(falcon2_base, NV_PFALCON2_FALCON_MOD_SEL_OFF), 1u); /* FalconModSelAlgo::Rsa3k */
}

int nv_falcon_reset_ga102(const nv_mmio_t *io, uint32_t base, uint32_t falcon2_base,
                          uint32_t boot0, uint32_t timeout_us)
{
    uint32_t hwcfg2 = PFALCON(base, NV_PFALCON_FALCON_HWCFG2_OFF);
    (void)rd(io, hwcfg2);

    /* Ожидание reset_ready — НЕобязательное (HW иногда не выставляет), до 150us. */
    uint32_t waited = 0;
    while (!(rd(io, hwcfg2) & NV_PFALCON_HWCFG2_RESET_READY)) {
        if (waited >= 150u) break;
        udelay(io, 10);
        waited += 10;
    }

    nv_falcon_reset_engine(io, base);
    int rc = nv_falcon_wait_mem_scrubbing(io, base, timeout_us);
    if (rc != NV_OK) return rc;

    rc = nv_falcon_select_core_ga102(io, falcon2_base, timeout_us);
    if (rc != NV_OK) return rc;

    /* falcon.rs reset(): FALCON_RM = PMC_BOOT_0. */
    wr(io, PFALCON(base, NV_PFALCON_FALCON_RM_OFF), boot0);
    return NV_OK;
}

/* ===================== DMA-загрузка ===================== */

void nv_falcon_dma_reset(const nv_mmio_t *io, uint32_t base)
{
    uint32_t ctl = PFALCON(base, NV_PFALCON_FBIF_CTL_OFF);
    wr(io, ctl, rd(io, ctl) | NV_PFALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX);
    wr(io, PFALCON(base, NV_PFALCON_FALCON_DMACTL_OFF), 0);
}

int nv_falcon_dma_wr(const nv_mmio_t *io, uint32_t base, uint64_t dma_addr,
                     int target_mem, const nv_dma_load_target_t *t, uint32_t timeout_us)
{
    int is_imem = (target_mem != NV_FALCON_MEM_DMEM);
    int secure  = (target_mem == NV_FALCON_MEM_IMEM_SECURE);

    /* IMEM: tag-адрес виртуальный -> src в FB = src_start, dma_start = base addr.
       DMEM: src_start складываем в dma-адрес (falcon.rs). */
    uint64_t dma_start;
    uint32_t fb_src;
    if (is_imem) { fb_src = t->src_start; dma_start = dma_addr; }
    else         { fb_src = 0;            dma_start = dma_addr + t->src_start; }

    if (dma_start % NV_DMA_BLOCK) return NV_ERR_STATE; /* должно быть кратно 256 */

    uint32_t num = (t->len + NV_DMA_BLOCK - 1) / NV_DMA_BLOCK;

    wr(io, PFALCON(base, NV_PFALCON_FALCON_DMATRFBASE_OFF),  (uint32_t)(dma_start >> 8));
    wr(io, PFALCON(base, NV_PFALCON_FALCON_DMATRFBASE1_OFF), (uint32_t)((dma_start >> 40) & 0x1FFu));

    uint32_t cmd = (NV_DMA_TRF_CMD_SIZE_256B << NV_PFALCON_DMATRFCMD_SIZE_SHIFT)
                 | (is_imem ? NV_PFALCON_DMATRFCMD_IMEM : 0)
                 | ((uint32_t)(secure ? 1u : 0u) << NV_PFALCON_DMATRFCMD_SEC_SHIFT);

    uint32_t trfcmd = PFALCON(base, NV_PFALCON_FALCON_DMATRFCMD_OFF);

    for (uint32_t i = 0; i < num; i++) {
        uint32_t pos = i * NV_DMA_BLOCK;
        wr(io, PFALCON(base, NV_PFALCON_FALCON_DMATRFMOFFS_OFF),  t->dst_start + pos);
        wr(io, PFALCON(base, NV_PFALCON_FALCON_DMATRFFBOFFS_OFF), fb_src + pos);
        wr(io, trfcmd, cmd);

        uint32_t waited = 0;
        while (!(rd(io, trfcmd) & NV_PFALCON_DMATRFCMD_IDLE)) {
            if (waited >= timeout_us) return NV_ERR_TIMEOUT;
            udelay(io, NV_POLL_STEP_US);
            waited += NV_POLL_STEP_US;
        }
    }
    return NV_OK;
}

int nv_falcon_dma_load_ga102(const nv_mmio_t *io, uint32_t base, uint32_t falcon2_base,
                             uint64_t dma_addr,
                             const nv_dma_load_target_t *imem_sec,
                             const nv_dma_load_target_t *dmem,
                             uint32_t pkc_data_offset, uint16_t engine_id_mask,
                             uint8_t ucode_id, uint32_t boot_addr, uint32_t timeout_us)
{
    nv_falcon_dma_reset(io, base);

    /* FBIF_TRANSCFG[0] = target CoherentSysmem, mem_type Physical. */
    uint32_t tc = PFALCON(base, NV_PFALCON_FBIF_TRANSCFG_OFF); /* [0] */
    uint32_t v = rd(io, tc);
    v &= ~0x7u;
    v |= (NV_FBIF_MEM_TYPE_PHYSICAL << 2) | NV_FBIF_TARGET_COHERENT_SYSMEM;
    wr(io, tc, v);

    int rc = nv_falcon_dma_wr(io, base, dma_addr, NV_FALCON_MEM_IMEM_SECURE, imem_sec, timeout_us);
    if (rc != NV_OK) return rc;
    rc = nv_falcon_dma_wr(io, base, dma_addr, NV_FALCON_MEM_DMEM, dmem, timeout_us);
    if (rc != NV_OK) return rc;

    nv_falcon_program_brom_ga102(io, falcon2_base, pkc_data_offset, engine_id_mask, ucode_id);

    /* BOOTVEC = старт (для FWSEC = 0). */
    wr(io, PFALCON(base, NV_PFALCON_FALCON_BOOTVEC_OFF), boot_addr);
    return NV_OK;
}

/* ===================== проверки результата ===================== */

int nv_wpr2_is_set(const nv_mmio_t *io, uint64_t *out_lo, uint64_t *out_hi)
{
    uint32_t lo = rd(io, NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    uint32_t hi = rd(io, NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    if (out_lo) *out_lo = nv_wpr2_bound_from_reg(lo);
    if (out_hi) *out_hi = nv_wpr2_bound_from_reg(hi);
    return (hi >> 4) != 0; /* regs.rs is_wpr2_set: hi_val != 0 */
}

int nv_gfw_boot_completed(const nv_mmio_t *io)
{
    uint32_t v = rd(io, NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
    return (v & 0xFFu) == NV_GFW_BOOT_PROGRESS_COMPLETED;
}
