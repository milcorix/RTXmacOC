/*
 * fwsec_run_linux.c — headless-прогон FWSEC-FRTS на реальной RTX через VFIO (Linux).
 *
 * Зачем: проверить слой 2 (FWSEC создаёт WPR2) на голом железе БЕЗ macOS и БЕЗ
 * монитора. Целевая машина (i5-12400F + RTX, без iGPU) грузится с Linux live-USB,
 * RTX привязывается к vfio-pci, заходим по SSH, запускаем этот харнесс.
 *
 * Это Linux-«хост» для тех же портируемых модулей, что использует kext:
 *   driver/gsp/{falcon,fwsec_patch,fwsec_locate,fb_layout}.c
 * Здесь nv_mmio_t = mmap региона BAR0 (VFIO), DMA-буфер = страница, отображённая в
 * IOMMU через VFIO_IOMMU_MAP_DMA (IOVA скармливается в DMATRFBASE). Логика прогона
 * 1:1 повторяет driver/RTXProbe/FwsecRun.cpp (reset→dma_load→boot→mbox0/WPR2).
 *
 * Зелёный результат здесь = 🟢 для портируемой логики (90% слоя 2). macOS-обвязка
 * kext (IOPCIDevice + IOBufferMemoryDescriptor) — отдельный тонкий шим, проверяется
 * позже на самой macOS.
 *
 * ВАЖНО (R0/R11): статус 🟢 ставить только по реальному логу с `mbox0=0` и
 * `WPR2 set=1`, положенному в docs/hw-dumps/. Компиляция — это не прогон.
 *
 * Сборка (на Linux):
 *   cc -Wall -Wextra -O2 tools/fwsec_run_linux.c \
 *      driver/gsp/falcon.c driver/gsp/fwsec_patch.c \
 *      driver/gsp/fwsec_locate.c driver/gsp/fb_layout.c \
 *      -o fwsec_run_linux
 *
 * Запуск (root, RTX уже на vfio-pci):
 *   sudo ./fwsec_run_linux 0000:01:00.0
 *
 * Подготовка стенда и привязка vfio-pci — docs/bench-test-fwsec-linux.md.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/vfio.h>

#include "../driver/gsp/falcon.h"
#include "../driver/gsp/fwsec_patch.h"
#include "../driver/gsp/fwsec_locate.h"
#include "../driver/gsp/fb_layout.h"

/* Размер V3-дескриптора (sizeof FalconUCodeDescV3) — база массива подписей. */
#define FALCON_DESC_V3_SIZE 44u
/* Таймаут шагов Falcon, как в falcon.rs (2 c). */
#define FWSEC_TIMEOUT_US (2u * 1000u * 1000u)
/* IOVA для DMA-буфера ucode (произвольный, в пределах 49 бит, выровнен). */
#define DMA_IOVA 0x10000000ULL

/* ===================== glue: nv_mmio_t поверх mmap BAR0 ===================== */

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
    struct timespec ts = { .tv_sec = usec / 1000000u,
                           .tv_nsec = (long)(usec % 1000000u) * 1000L };
    nanosleep(&ts, NULL);
}

/* ===================== VFIO setup ===================== */

struct vfio_ctx {
    int container;
    int group;
    int device;
    volatile void *bar0;
    size_t bar0_size;
    int cfg_off_valid;
    uint64_t cfg_off;
};

/* Прочитать номер IOMMU-группы устройства из sysfs. */
static int read_iommu_group(const char *bdf, char *out, size_t outlen)
{
    char link[256];
    char target[256];
    snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n < 0) {
        fprintf(stderr, "readlink(%s): %s (IOMMU/VT-d включён? карта на vfio-pci?)\n",
                link, strerror(errno));
        return -1;
    }
    target[n] = '\0';
    const char *base = strrchr(target, '/');
    base = base ? base + 1 : target;
    snprintf(out, outlen, "%s", base);
    return 0;
}

static int vfio_open(struct vfio_ctx *v, const char *bdf)
{
    memset(v, 0, sizeof(*v));
    v->container = v->group = v->device = -1;

    char grpid[64];
    if (read_iommu_group(bdf, grpid, sizeof(grpid)) != 0)
        return -1;
    printf("VFIO: устройство %s, IOMMU-группа %s\n", bdf, grpid);

    v->container = open("/dev/vfio/vfio", O_RDWR);
    if (v->container < 0) { perror("open /dev/vfio/vfio"); return -1; }

    if (ioctl(v->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION) {
        fprintf(stderr, "VFIO: неподдерживаемая версия API\n");
        return -1;
    }
    if (!ioctl(v->container, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
        fprintf(stderr, "VFIO: нет TYPE1_IOMMU (включи VT-d и intel_iommu=on)\n");
        return -1;
    }

    char grppath[64];
    snprintf(grppath, sizeof(grppath), "/dev/vfio/%s", grpid);
    v->group = open(grppath, O_RDWR);
    if (v->group < 0) { perror("open group"); return -1; }

    struct vfio_group_status gs = { .argsz = sizeof(gs) };
    if (ioctl(v->group, VFIO_GROUP_GET_STATUS, &gs)) { perror("GROUP_GET_STATUS"); return -1; }
    if (!(gs.flags & VFIO_GROUP_FLAGS_VIABLE)) {
        fprintf(stderr, "VFIO: группа не viable — все устройства группы должны быть на vfio-pci\n");
        return -1;
    }

    if (ioctl(v->group, VFIO_GROUP_SET_CONTAINER, &v->container)) { perror("SET_CONTAINER"); return -1; }
    if (ioctl(v->container, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU)) { perror("SET_IOMMU"); return -1; }

    v->device = ioctl(v->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
    if (v->device < 0) { perror("GET_DEVICE_FD"); return -1; }

    /* BAR0 region. */
    struct vfio_region_info reg = { .argsz = sizeof(reg), .index = VFIO_PCI_BAR0_REGION_INDEX };
    if (ioctl(v->device, VFIO_DEVICE_GET_REGION_INFO, &reg)) { perror("REGION_INFO BAR0"); return -1; }
    if (!(reg.flags & VFIO_REGION_INFO_FLAG_MMAP)) {
        fprintf(stderr, "VFIO: BAR0 не mmap-able\n");
        return -1;
    }
    v->bar0_size = (size_t)reg.size;
    v->bar0 = mmap(NULL, reg.size, PROT_READ | PROT_WRITE, MAP_SHARED, v->device, reg.offset);
    if (v->bar0 == MAP_FAILED) { perror("mmap BAR0"); v->bar0 = NULL; return -1; }
    printf("VFIO: BAR0 смаплен, size=0x%zx\n", v->bar0_size);

    /* Config region — нужен для включения Bus Master (DMA). */
    struct vfio_region_info creg = { .argsz = sizeof(creg), .index = VFIO_PCI_CONFIG_REGION_INDEX };
    if (ioctl(v->device, VFIO_DEVICE_GET_REGION_INFO, &creg) == 0) {
        v->cfg_off = creg.offset;
        v->cfg_off_valid = 1;
    }
    return 0;
}

/* Включить Memory Space + Bus Master в PCI command-регистре (для DMA). */
static void vfio_enable_busmaster(struct vfio_ctx *v)
{
    if (!v->cfg_off_valid) {
        fprintf(stderr, "WARN: нет config-региона, Bus Master не включён вручную\n");
        return;
    }
    uint16_t cmd = 0;
    if (pread(v->device, &cmd, sizeof(cmd), v->cfg_off + 0x04) == (ssize_t)sizeof(cmd)) {
        cmd |= 0x0002 /* MEMORY_SPACE */ | 0x0004 /* BUS_MASTER */;
        if (pwrite(v->device, &cmd, sizeof(cmd), v->cfg_off + 0x04) != (ssize_t)sizeof(cmd))
            fprintf(stderr, "WARN: не удалось записать PCI command\n");
        else
            printf("VFIO: Memory Space + Bus Master включены (CMD=0x%04x)\n", cmd);
    }
}

/* Отобразить буфер в IOMMU: process VA -> IOVA. */
static int vfio_map_dma(struct vfio_ctx *v, void *vaddr, uint64_t iova, size_t size)
{
    struct vfio_iommu_type1_dma_map map = {
        .argsz = sizeof(map),
        .flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr = (uint64_t)(uintptr_t)vaddr,
        .iova  = iova,
        .size  = size,
    };
    if (ioctl(v->container, VFIO_IOMMU_MAP_DMA, &map)) { perror("IOMMU_MAP_DMA"); return -1; }
    return 0;
}

static void vfio_close(struct vfio_ctx *v)
{
    if (v->bar0) munmap((void *)v->bar0, v->bar0_size);
    if (v->device >= 0) close(v->device);
    if (v->group >= 0) close(v->group);
    if (v->container >= 0) close(v->container);
}

/* ===================== Прогон FWSEC-FRTS ===================== */

static int run_fwsec(const nv_mmio_t *io, uint64_t dma_iova, uint8_t *dma_buf, size_t dma_cap)
{
    /* Диагностика состояния карты до прогона. */
    uint32_t boot0 = bar_rd(io->ctx, 0x0);
    printf("PMC_BOOT_0 = 0x%08x\n", boot0);

    /* vfio-pci при привязке делает FLR — после сброса карта заново гоняет devinit
       (GFW boot). Дождаться завершения, иначе FB-регистры читаются как priv-фолт
       0xBADFxxxx. Поллим до 4с (как nova-core при инициализации GPU). */
    uint32_t gfw_raw = nv_gfw_boot_raw(io);
    printf("GFW boot (до ожидания) = 0x%08x (completed=%d)\n",
           gfw_raw, (gfw_raw & 0xFFu) == 0xFFu);
    int gfw_rc = nv_wait_gfw_boot_completed(io, 4u * 1000u * 1000u);
    gfw_raw = nv_gfw_boot_raw(io);
    printf("GFW boot (после ожидания ≤4с) = 0x%08x rc=%d\n", gfw_raw, gfw_rc);
    if (gfw_rc != NV_OK) {
        fprintf(stderr, "FAIL: GFW boot не завершился за 4с — карта не прошла devinit "
                        "после reset/FLR.\n");
        fprintf(stderr, "  диагностика: HWCFG2(GSP)=0x%08x USABLE_FB_MB=0x%08x "
                        "BOOT_0=0x%08x\n",
                bar_rd(io->ctx, NV_PGSP_FALCON_HWCFG2),
                bar_rd(io->ctx, NV_USABLE_FB_SIZE_IN_MB), boot0);
        return -1;
    }

    uint64_t vram = nv_fb_vidmem_size(io);
    printf("VRAM usable = %llu MiB (0x%llx байт)\n",
           (unsigned long long)(vram >> 20), (unsigned long long)vram);

    /* Регион FRTS из объёма VRAM. */
    uint64_t frts_addr = 0, frts_size = 0;
    if (nv_fb_compute_frts(io, &frts_addr, &frts_size) != 0) {
        fprintf(stderr, "FAIL: не удалось вычислить регион FRTS (VRAM size = 0?)\n");
        return -1;
    }
    printf("FRTS region addr=0x%llx size=0x%llx\n",
           (unsigned long long)frts_addr, (unsigned long long)frts_size);

    /* 1. VBIOS из теневой ROM (BAR0 + 0x300000). */
    const uint32_t vbios_len = NV_VBIOS_SCAN_MAX;
    uint8_t *vbios = malloc(vbios_len);
    if (!vbios) { fprintf(stderr, "FAIL: malloc(vbios)\n"); return -1; }
    for (uint32_t i = 0; i < vbios_len; i += 4) {
        uint32_t w = bar_rd(io->ctx, NV_ROM_SHADOW_BASE + i);
        vbios[i + 0] = (uint8_t)w;
        vbios[i + 1] = (uint8_t)(w >> 8);
        vbios[i + 2] = (uint8_t)(w >> 16);
        vbios[i + 3] = (uint8_t)(w >> 24);
    }
    printf("VBIOS ROM-shadow: первые байты %02x %02x (ожидается 55 AA)\n",
           vbios[0], vbios[1]);

    /* Дамп прочитанного PROM-shadow на диск для офлайн-разбора структуры. */
    {
        FILE *df = fopen("/tmp/rtx-prom.bin", "wb");
        if (df) {
            size_t w = fwrite(vbios, 1, vbios_len, df);
            fclose(df);
            printf("PROM-shadow дамп: /tmp/rtx-prom.bin (%zu байт)\n", w);
        } else {
            printf("PROM-shadow дамп: не удалось открыть /tmp/rtx-prom.bin\n");
        }
    }

    /* 2. Локализация и парсинг дескриптора FWSEC. */
    nv_fwsec_location_t loc;
    int lrc = nv_fwsec_locate(vbios, vbios_len, &loc);
    if (lrc != NV_FWSEC_LOC_OK) {
        fprintf(stderr, "FAIL: FWSEC не найден в VBIOS, nv_fwsec_locate rc=%d "
                        "(-1 NOSIG, -2 NOIMG, -3 PARSE, -4 BOUNDS)\n", lrc);
        free(vbios); return -1;
    }
    printf("FWSEC desc @ vbios+0x%zx\n", loc.desc_abs);

    nv_fwsec_desc_t desc;
    if (nv_fwsec_desc_parse(vbios + loc.desc_abs, vbios_len - loc.desc_abs, &desc) != NV_FWSEC_OK) {
        fprintf(stderr, "FAIL: разбор дескриптора\n");
        free(vbios); return -1;
    }
    printf("desc: version=%d hdr_size=%u imem_load=%u dmem_load=%u "
           "engine_id_mask=0x%x ucode_id=%u sig_count=%u sig_versions=0x%x pkc=%u\n",
           desc.version, desc.hdr_size, desc.imem_load_size, desc.dmem_load_size,
           desc.engine_id_mask, desc.ucode_id, desc.signature_count,
           desc.signature_versions, desc.pkc_data_offset);

    if (desc.version != 3) {
        fprintf(stderr, "FAIL: дескриптор != V3 (этот путь V3-only)\n");
        free(vbios); return -1;
    }

    size_t ucode_off  = loc.desc_abs + desc.hdr_size;
    size_t ucode_size = (size_t)desc.imem_load_size + (size_t)desc.dmem_load_size;
    if (ucode_size == 0 || ucode_off > vbios_len || ucode_size > vbios_len - ucode_off) {
        fprintf(stderr, "FAIL: ucode вне границ VBIOS\n");
        free(vbios); return -1;
    }
    if (ucode_size > dma_cap) {
        fprintf(stderr, "FAIL: ucode (%zu) больше DMA-буфера (%zu)\n", ucode_size, dma_cap);
        free(vbios); return -1;
    }

    /* 3. Копируем ucode в DMA-буфер (он уже отображён в IOMMU на dma_iova). */
    memset(dma_buf, 0, dma_cap);
    memcpy(dma_buf, vbios + ucode_off, ucode_size);

    /* 4. Патч: FRTS-команда. */
    if (nv_fwsec_patch_frts(dma_buf, dma_cap, &desc, frts_addr, frts_size) != NV_FWSEC_OK) {
        fprintf(stderr, "FAIL: патч FRTS\n");
        free(vbios); return -1;
    }

    /* 4b. Патч подписи (по fuse-версии). */
    if (desc.signature_count != 0) {
        uint32_t fuse_ver = 0;
        if (nv_falcon_signature_fuse_version_ga102(io, desc.engine_id_mask,
                                                   desc.ucode_id, &fuse_ver) != NV_OK) {
            fprintf(stderr, "FAIL: чтение fuse-версии\n");
            free(vbios); return -1;
        }
        uint32_t sig_idx = 0;
        if (nv_fwsec_select_signature_index(&desc, fuse_ver, &sig_idx) != NV_FWSEC_OK) {
            fprintf(stderr, "FAIL: нет подходящей подписи (fuse=%u, versions=0x%x)\n",
                    fuse_ver, desc.signature_versions);
            free(vbios); return -1;
        }
        size_t sigs_base = loc.desc_abs + FALCON_DESC_V3_SIZE;
        size_t sig_at    = sigs_base + (size_t)sig_idx * NV_BCRT30_RSA3K_SIG_SIZE;
        if (sig_at + NV_BCRT30_RSA3K_SIG_SIZE > vbios_len) {
            fprintf(stderr, "FAIL: подпись вне границ VBIOS\n");
            free(vbios); return -1;
        }
        printf("signature: fuse_ver=%u idx=%u @ vbios+0x%zx\n", fuse_ver, sig_idx, sig_at);
        if (nv_fwsec_patch_signature(dma_buf, dma_cap, &desc, vbios + sig_at) != NV_FWSEC_OK) {
            fprintf(stderr, "FAIL: вставка подписи\n");
            free(vbios); return -1;
        }
    } else {
        printf("signature: signature_count=0 — патч подписи не требуется\n");
    }
    free(vbios);

    /* 5. reset -> dma_load -> boot. */
    if (nv_falcon_reset_ga102(io, NV_PGSP_FALCON_BASE, NV_PGSP_FALCON2_BASE,
                              boot0, FWSEC_TIMEOUT_US) != NV_OK) {
        fprintf(stderr, "FAIL: reset Falcon (HWCFG2=0x%08x)\n",
                bar_rd(io->ctx, NV_PGSP_FALCON_HWCFG2));
        return -1;
    }
    printf("Falcon reset OK\n");

    nv_dma_load_target_t imem_sec = { 0, desc.imem_phys_base, desc.imem_load_size };
    nv_dma_load_target_t dmem     = { desc.imem_load_size, desc.dmem_phys_base, desc.dmem_load_size };
    if (nv_falcon_dma_load_ga102(io, NV_PGSP_FALCON_BASE, NV_PGSP_FALCON2_BASE,
                                 dma_iova, &imem_sec, &dmem,
                                 desc.pkc_data_offset, desc.engine_id_mask,
                                 desc.ucode_id, 0 /*boot_addr*/, FWSEC_TIMEOUT_US) != NV_OK) {
        fprintf(stderr, "FAIL: dma_load (DMATRFCMD=0x%08x)\n",
                bar_rd(io->ctx, NV_PGSP_FALCON_BASE + NV_PFALCON_FALCON_DMATRFCMD_OFF));
        return -1;
    }
    printf("Falcon dma_load OK\n");

    uint32_t mbox0 = 0xFFFFFFFFu;
    int brc = nv_falcon_boot(io, NV_PGSP_FALCON_BASE, 1, 0, 0, 0, FWSEC_TIMEOUT_US, &mbox0);
    uint32_t mbox1 = nv_falcon_read_mailbox1(io, NV_PGSP_FALCON_BASE);
    if (brc != NV_OK) {
        fprintf(stderr, "FAIL: boot не дошёл до halted (timeout); mbox0=0x%08x mbox1=0x%08x\n",
                mbox0, mbox1);
        return -1;
    }

    /* 6. Проверка результата. */
    uint64_t wpr_lo = 0, wpr_hi = 0;
    int wpr_set = nv_wpr2_is_set(io, &wpr_lo, &wpr_hi);
    printf("FWSEC done: mbox0=0x%08x mbox1=0x%08x WPR2 set=%d [0x%llx..0x%llx]\n",
           mbox0, mbox1, wpr_set,
           (unsigned long long)wpr_lo, (unsigned long long)wpr_hi);

    if (mbox0 == 0 && wpr_set) {
        printf("*** FWSEC-FRTS OK, WPR2 создан ***\n");
        return 0;
    }
    fprintf(stderr, "FAIL: FWSEC вернул ошибку или WPR2 не выставлен\n");
    return -1;
}

/* ===================== авто-детект BDF карты NVIDIA ===================== */

/* Найти первую PCI-функцию NVIDIA (vendor 0x10de) класса дисплея (0x03xxxx).
   Пишет BDF в out (напр. "0000:01:00.0"). Возвращает 0 при успехе. */
static int find_nvidia_bdf(char *out, size_t outlen)
{
    DIR *d = opendir("/sys/bus/pci/devices");
    if (!d) return -1;
    struct dirent *e;
    char best[32] = {0};
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char path[300]; char buf[32];
        FILE *f;
        unsigned vendor = 0, class_code = 0;

        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor", e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (fgets(buf, sizeof(buf), f)) vendor = (unsigned)strtoul(buf, NULL, 16);
        fclose(f);
        if (vendor != 0x10de) continue;

        snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/class", e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        if (fgets(buf, sizeof(buf), f)) class_code = (unsigned)strtoul(buf, NULL, 16);
        fclose(f);

        /* класс дисплея: 0x0300xx (VGA) или 0x0302xx (3D). Берём VGA приоритетно. */
        if ((class_code >> 16) == 0x03) {
            if ((class_code >> 8) == 0x0300) { /* VGA — лучший кандидат */
                snprintf(out, outlen, "%s", e->d_name);
                closedir(d);
                return 0;
            }
            if (best[0] == 0) snprintf(best, sizeof(best), "%s", e->d_name);
        }
    }
    closedir(d);
    if (best[0]) { snprintf(out, outlen, "%s", best); return 0; }
    return -1;
}

int main(int argc, char **argv)
{
    char auto_bdf[32];
    const char *bdf = (argc >= 2) ? argv[1] : NULL;
    if (!bdf) {
        if (find_nvidia_bdf(auto_bdf, sizeof(auto_bdf)) == 0) {
            bdf = auto_bdf;
            printf("BDF (авто-детект): %s\n", bdf);
        } else {
            fprintf(stderr, "usage: %s [pci-bdf]  (NVIDIA не найдена для авто-детекта)\n", argv[0]);
            return 2;
        }
    }

    struct vfio_ctx v;
    if (vfio_open(&v, bdf) != 0) { vfio_close(&v); return 1; }
    vfio_enable_busmaster(&v);

    /* DMA-буфер под ucode (1 MiB с запасом, кратно 256, выровнен страницей). */
    const size_t dma_cap = 0x100000;
    void *dma_buf = mmap(NULL, dma_cap, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (dma_buf == MAP_FAILED) { perror("mmap dma_buf"); vfio_close(&v); return 1; }
    if (vfio_map_dma(&v, dma_buf, DMA_IOVA, dma_cap) != 0) {
        munmap(dma_buf, dma_cap); vfio_close(&v); return 1;
    }
    printf("DMA-буфер: VA=%p IOVA=0x%llx size=0x%zx\n",
           dma_buf, (unsigned long long)DMA_IOVA, dma_cap);

    nv_mmio_t io = { .ctx = (void *)v.bar0, .rd = bar_rd, .wr = bar_wr, .udelay = bar_udelay };

    int rc = run_fwsec(&io, DMA_IOVA, (uint8_t *)dma_buf, dma_cap);

    /* Размап DMA (не критично перед выходом, но аккуратно). */
    struct vfio_iommu_type1_dma_unmap unmap = {
        .argsz = sizeof(unmap), .iova = DMA_IOVA, .size = dma_cap,
    };
    ioctl(v.container, VFIO_IOMMU_UNMAP_DMA, &unmap);
    munmap(dma_buf, dma_cap);
    vfio_close(&v);

    printf("\n=== РЕЗУЛЬТАТ: %s ===\n", rc == 0 ? "OK (WPR2 создан)" : "FAIL (см. лог выше)");
    return rc == 0 ? 0 : 1;
}
