/*
 * falcon_regs.h — регистры микроконтроллера Falcon (GSP/SEC2) для bring-up (слой 2).
 *
 * Все адреса/поля портированы из nova-core (ядро Linux), НЕ выдуманы:
 *   drivers/gpu/nova-core/regs.rs        — смещения PFALCON/PFALCON2, WPR2, GFW_BOOT
 *   drivers/gpu/nova-core/falcon/gsp.rs  — база движка GSP (PFALCON=0x110000)
 *   drivers/gpu/nova-core/falcon.rs      — последовательность reset/load/boot
 * Содержимое источников пересказано для соответствия лицензии.
 *
 * Смещения PFALCON_* даны относительно базы движка. Для GSP подставлена база
 * 0x110000 (PFALCON) / 0x111000 (PFALCON2). Тот же набор подойдёт для SEC2,
 * если задать его базу.
 */
#ifndef RTXMACOC_FALCON_REGS_H
#define RTXMACOC_FALCON_REGS_H

#include <stdint.h>

/* --- Базы движков (falcon/gsp.rs: RegisterBase<PFalconBase>/<PFalcon2Base>) --- */
#define NV_PGSP_FALCON_BASE   0x00110000u
#define NV_PGSP_FALCON2_BASE  0x00111000u

/* Помощник: регистр PFALCON по базе движка и смещению. */
#define PFALCON(base, off)   ((base) + (off))

/* ===================== PFALCON (regs.rs) ===================== */
#define NV_PFALCON_FALCON_IRQSCLR_OFF       0x00000004u /* swgen0=bit6, halt=bit4 */
#define NV_PFALCON_FALCON_MAILBOX0_OFF      0x00000040u
#define NV_PFALCON_FALCON_MAILBOX1_OFF      0x00000044u
#define NV_PFALCON_FALCON_OS_OFF            0x00000080u
#define NV_PFALCON_FALCON_RM_OFF            0x00000084u
#define NV_PFALCON_FALCON_HWCFG2_OFF        0x000000F4u
#define NV_PFALCON_FALCON_CPUCTL_OFF        0x00000100u
#define NV_PFALCON_FALCON_BOOTVEC_OFF       0x00000104u
#define NV_PFALCON_FALCON_DMACTL_OFF        0x0000010Cu
#define NV_PFALCON_FALCON_DMATRFBASE_OFF    0x00000110u
#define NV_PFALCON_FALCON_DMATRFMOFFS_OFF   0x00000114u
#define NV_PFALCON_FALCON_DMATRFCMD_OFF     0x00000118u
#define NV_PFALCON_FALCON_DMATRFFBOFFS_OFF  0x0000011Cu
#define NV_PFALCON_FALCON_DMATRFBASE1_OFF   0x00000128u
#define NV_PFALCON_FALCON_HWCFG1_OFF        0x0000012Cu
#define NV_PFALCON_FALCON_CPUCTL_ALIAS_OFF  0x00000130u
#define NV_PFALCON_FALCON_IMEMC_OFF         0x00000180u /* [4], stride 16 */
#define NV_PFALCON_FALCON_IMEMD_OFF         0x00000184u /* [4], stride 16 */
#define NV_PFALCON_FALCON_IMEMT_OFF         0x00000188u /* [4], stride 16 */
#define NV_PFALCON_FALCON_DMEMC_OFF         0x000001C0u /* [8], stride 8  */
#define NV_PFALCON_FALCON_DMEMD_OFF         0x000001C4u /* [8], stride 8  */
#define NV_PFALCON_FALCON_ENGINE_OFF        0x000003C0u /* reset=bit0 */
#define NV_PFALCON_FBIF_TRANSCFG_OFF        0x00000600u /* [8] */
#define NV_PFALCON_FBIF_CTL_OFF             0x00000624u /* allow_phys_no_ctx=bit7 */

/* ===================== PFALCON2 / PRISCV (regs.rs) ===================== */
#define NV_PFALCON2_FALCON_MOD_SEL_OFF            0x00000180u /* algo[7:0] */
#define NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID_OFF 0x00000198u
#define NV_PFALCON2_FALCON_BROM_ENGIDMASK_OFF     0x0000019Cu
#define NV_PFALCON2_FALCON_BROM_PARAADDR_OFF      0x00000210u
#define NV_PRISCV_RISCV_CPUCTL_OFF                0x00000388u /* active=bit7, halted=bit0 */
#define NV_PRISCV_RISCV_BCR_CTRL_OFF              0x00000668u /* core_select=bit4, valid=bit0 */

/* --- Готовые абсолютные адреса для GSP --- */
#define NV_PGSP_FALCON_IRQSCLR     PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_IRQSCLR_OFF)
#define NV_PGSP_FALCON_MAILBOX0    PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_MAILBOX0_OFF)
#define NV_PGSP_FALCON_MAILBOX1    PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_MAILBOX1_OFF)
#define NV_PGSP_FALCON_HWCFG2      PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_HWCFG2_OFF)
#define NV_PGSP_FALCON_CPUCTL      PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_CPUCTL_OFF)
#define NV_PGSP_FALCON_BOOTVEC     PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_BOOTVEC_OFF)
#define NV_PGSP_FALCON_DMACTL      PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_DMACTL_OFF)
#define NV_PGSP_FALCON_HWCFG1      PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_HWCFG1_OFF)
#define NV_PGSP_FALCON_ENGINE      PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FALCON_ENGINE_OFF)
#define NV_PGSP_FBIF_CTL           PFALCON(NV_PGSP_FALCON_BASE,  NV_PFALCON_FBIF_CTL_OFF)

/* ===================== Битовые поля (regs.rs) ===================== */
/* IRQSCLR */
#define NV_PFALCON_IRQSCLR_HALT      (1u << 4)
#define NV_PFALCON_IRQSCLR_SWGEN0    (1u << 6)
/* HWCFG2 */
#define NV_PFALCON_HWCFG2_RISCV              (1u << 10)
#define NV_PFALCON_HWCFG2_MEM_SCRUBBING     (1u << 12) /* 0 после завершения скраба */
#define NV_PFALCON_HWCFG2_RISCV_BR_LOCKDOWN (1u << 13)
#define NV_PFALCON_HWCFG2_RESET_READY       (1u << 31) /* GA102+ */
/* CPUCTL */
#define NV_PFALCON_CPUCTL_STARTCPU   (1u << 1)
#define NV_PFALCON_CPUCTL_HALTED     (1u << 4)
#define NV_PFALCON_CPUCTL_ALIAS_EN   (1u << 6)
/* CPUCTL_ALIAS */
#define NV_PFALCON_CPUCTL_ALIAS_STARTCPU (1u << 1)
/* DMACTL */
#define NV_PFALCON_DMACTL_REQUIRE_CTX     (1u << 0)
#define NV_PFALCON_DMACTL_DMEM_SCRUBBING  (1u << 1)
#define NV_PFALCON_DMACTL_IMEM_SCRUBBING  (1u << 2)
/* ENGINE */
#define NV_PFALCON_ENGINE_RESET      (1u << 0)
/* FBIF_CTL */
#define NV_PFALCON_FBIF_CTL_ALLOW_PHYS_NO_CTX (1u << 7)
/* IMEMC */
#define NV_PFALCON_IMEMC_AINCW       (1u << 24)
#define NV_PFALCON_IMEMC_SECURE      (1u << 28)
/* DMEMC */
#define NV_PFALCON_DMEMC_AINCW       (1u << 24)
/* DMATRFCMD */
#define NV_PFALCON_DMATRFCMD_FULL    (1u << 0)
#define NV_PFALCON_DMATRFCMD_IDLE    (1u << 1)
#define NV_PFALCON_DMATRFCMD_IMEM    (1u << 4)
#define NV_PFALCON_DMATRFCMD_IS_WRITE (1u << 5)
#define NV_PFALCON_DMATRFCMD_SIZE_SHIFT  8u   /* [10:8] */
#define NV_PFALCON_DMATRFCMD_SEC_SHIFT   2u   /* [3:2]  */
/* DmaTrfCmdSize (falcon.rs): единственный используемый размер. */
#define NV_DMA_TRF_CMD_SIZE_256B     0x6u
/* FBIF target (falcon.rs FalconFbifTarget) */
#define NV_FBIF_TARGET_LOCAL_FB           0u
#define NV_FBIF_TARGET_COHERENT_SYSMEM    1u
#define NV_FBIF_TARGET_NONCOHERENT_SYSMEM 2u
/* FBIF mem type */
#define NV_FBIF_MEM_TYPE_VIRTUAL   0u
#define NV_FBIF_MEM_TYPE_PHYSICAL  1u
/* Security model (HWCFG1[5:4], falcon.rs FalconSecurityModel) */
#define NV_FALCON_SEC_MODEL_NONE   0u
#define NV_FALCON_SEC_MODEL_LIGHT  2u
#define NV_FALCON_SEC_MODEL_HEAVY  3u

/* ===================== Прочие регистры bring-up (regs.rs) ===================== */
/* WPR2 границы — проверка результата FWSEC-FRTS. */
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO  0x001FA824u /* lo_val[31:4] << 12 = нижняя граница */
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI  0x001FA828u /* hi_val[31:4] << 12; 0 = WPR2 не задан */
/* GFW boot progress: 0xFF = завершено. */
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT 0x00118234u
#define NV_GFW_BOOT_PROGRESS_COMPLETED 0xFFu
/* Очередь команд GSP (адрес головы). */
#define NV_PGSP_QUEUE_HEAD           0x00110C00u

/* Теневая копия VBIOS ROM в BAR0 (vbios.rs ROM_OFFSET) и лимит сканирования. */
#define NV_ROM_SHADOW_BASE           0x00300000u
#define NV_VBIOS_SCAN_MAX            0x00100000u

/* --- FB layout / расчёт региона FRTS (fb.rs + fb/hal, GA102/Ada) --- */
/* Размер usable VRAM в МБ (alias NV_PGC6_AON_SECURE_SCRATCH_GROUP_42). */
#define NV_USABLE_FB_SIZE_IN_MB      0x001183A4u
/* VGA workspace base: addr[31:8]<<16, status_valid = bit3. */
#define NV_PDISP_VGA_WORKSPACE_BASE  0x00625F04u
/* Fuse: display_disabled = bit0 (ga100/Ada). */
#define NV_FUSE_STATUS_OPT_DISPLAY   0x00820C04u
/* Константы layout (fb.rs / tu102 fb-hal). */
#define NV_FRTS_SIZE                 0x00100000u  /* 1 MiB (frts_size_tu102) */
#define NV_PRAMIN_SIZE               0x00100000u  /* 1 MiB */
#define NV_VBIOS_WORKSPACE_SIZE      0x00020000u  /* 128 KiB */
#define NV_FRTS_ALIGN                0x00020000u  /* 128 KiB down-align */

/* --- FUSE: версии ucode для выбора HS-подписи (regs.rs, [16] stride 4) --- */
#define NV_FUSE_OPT_FPF_SIZE                  16u
#define NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION  0x00824100u
#define NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION   0x00824140u
#define NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION    0x008241C0u
/* Биты engine_id_mask для выбора fuse-регистра (hal/ga102.rs). */
#define NV_FALCON_ENGINE_ID_SEC2   0x0001u
#define NV_FALCON_ENGINE_ID_NVDEC  0x0004u
#define NV_FALCON_ENGINE_ID_GSP    0x0400u

/* Выравнивание блоков памяти Falcon (falcon.rs MEM_BLOCK_ALIGNMENT). */
#define NV_FALCON_MEM_BLOCK_ALIGNMENT 256u

/* Маска адреса DMA для DMATRFBASE/1 (falcon.rs): 49 бит. */
#define NV_FALCON_DMA_ADDR_BITS 49

/* Декод WPR2 границ. */
static inline uint64_t nv_wpr2_bound_from_reg(uint32_t reg)
{
    /* lo_val/hi_val = биты [31:4]; реальная граница = (val) << 12, где val уже
       содержит сдвинутые биты 12..40 в позиции 4.. — см. regs.rs lower_bound(). */
    return ((uint64_t)(reg >> 4)) << 12;
}

#endif /* RTXMACOC_FALCON_REGS_H */
