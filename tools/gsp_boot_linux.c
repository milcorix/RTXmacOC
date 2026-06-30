/*
 * gsp_boot_linux.c — оркестратор загрузки GSP-RM на железе (задача 6, фаза 6).
 *
 * Полная цепочка слоя 2 на реальной RTX через VFIO (Linux, headless):
 *   1. FWSEC-FRTS → создаёт WPR2 (как fwsec_run_linux);
 *   2. staging GSP-RM в sysmem: .fwimage + radix3 + bootloader + signature + WprMeta + libos;
 *   3. reset GSP-фалкона в RISC-V, GSP mailbox = libos-адрес;
 *   4. SEC2 Booter с mbox = WprMeta-адрес → грузит GSP-RM в WPR2 и стартует RISC-V;
 *   5. FALCON_OS = app_version; проверка RISC-V active.
 *
 * Метрика (задача 6): Booter mbox0==0 И GSP RISC-V active. (Первый RPC = задача 7.)
 * Сборка: make gsp-boot-linux ; прогон без ребута: tools/run-gsp-boot-detached.sh
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
#include "../driver/gsp/fwsec_locate.h"
#include "../driver/gsp/fwsec_patch.h"
#include "../driver/gsp/fb_layout.h"
#include "../driver/gsp/booter.h"
#include "../driver/gsp/gsp_fw.h"
#include "../driver/gsp/gsp_rpc.h"
#include "../driver/gsp/fw_blob.h"

#define TIMEOUT_US (2u * 1000u * 1000u)
#define ARENA_IOVA 0x10000000ULL
#define ARENA_SIZE (64u * 1024u * 1024u)   /* 64 МиБ под всё (fwimage ~36 МиБ) */
#define FALCON_DESC_V3_SIZE 44u

static uint32_t bar_rd(void *ctx, uint32_t off)
{ return *(volatile uint32_t *)((volatile uint8_t *)ctx + off); }
static void bar_wr(void *ctx, uint32_t off, uint32_t val)
{ *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = val; }
static void bar_udelay(void *ctx, uint32_t usec)
{ (void)ctx; struct timespec ts = { usec/1000000u, (long)(usec%1000000u)*1000L }; nanosleep(&ts, NULL); }

/* ===================== VFIO ===================== */
struct vfio_ctx { int container, group, device; volatile void *bar0; size_t bar0_size; int cfgv; uint64_t cfg; };

static int read_iommu_group(const char *bdf, char *out, size_t n)
{
    char link[256], tgt[256];
    snprintf(link, sizeof(link), "/sys/bus/pci/devices/%s/iommu_group", bdf);
    ssize_t r = readlink(link, tgt, sizeof(tgt)-1);
    if (r < 0) { perror("readlink iommu_group"); return -1; }
    tgt[r]='\0'; const char *b = strrchr(tgt,'/'); b=b?b+1:tgt; snprintf(out,n,"%s",b); return 0;
}
static int vfio_open(struct vfio_ctx *v, const char *bdf)
{
    memset(v,0,sizeof(*v)); v->container=v->group=v->device=-1;
    char g[64]; if (read_iommu_group(bdf,g,sizeof(g))) return -1;
    printf("VFIO: %s, группа %s\n", bdf, g);
    v->container=open("/dev/vfio/vfio",O_RDWR); if(v->container<0){perror("vfio");return -1;}
    if (ioctl(v->container,VFIO_GET_API_VERSION)!=VFIO_API_VERSION){fprintf(stderr,"API\n");return -1;}
    if (!ioctl(v->container,VFIO_CHECK_EXTENSION,VFIO_TYPE1_IOMMU)){fprintf(stderr,"нет TYPE1\n");return -1;}
    char gp[80]; snprintf(gp,sizeof(gp),"/dev/vfio/%s",g);
    v->group=open(gp,O_RDWR); if(v->group<0){perror("group");return -1;}
    struct vfio_group_status gs={.argsz=sizeof(gs)};
    if (ioctl(v->group,VFIO_GROUP_GET_STATUS,&gs)){perror("STATUS");return -1;}
    if (!(gs.flags&VFIO_GROUP_FLAGS_VIABLE)){fprintf(stderr,"не viable\n");return -1;}
    if (ioctl(v->group,VFIO_GROUP_SET_CONTAINER,&v->container)){perror("SET_CONT");return -1;}
    if (ioctl(v->container,VFIO_SET_IOMMU,VFIO_TYPE1_IOMMU)){perror("SET_IOMMU");return -1;}
    v->device=ioctl(v->group,VFIO_GROUP_GET_DEVICE_FD,bdf); if(v->device<0){perror("DEV_FD");return -1;}
    struct vfio_region_info reg={.argsz=sizeof(reg),.index=VFIO_PCI_BAR0_REGION_INDEX};
    if (ioctl(v->device,VFIO_DEVICE_GET_REGION_INFO,&reg)){perror("BAR0");return -1;}
    v->bar0_size=(size_t)reg.size;
    v->bar0=mmap(NULL,reg.size,PROT_READ|PROT_WRITE,MAP_SHARED,v->device,reg.offset);
    if (v->bar0==MAP_FAILED){perror("mmap BAR0");v->bar0=NULL;return -1;}
    printf("VFIO: BAR0 size=0x%zx\n",v->bar0_size);
    struct vfio_region_info c={.argsz=sizeof(c),.index=VFIO_PCI_CONFIG_REGION_INDEX};
    if (ioctl(v->device,VFIO_DEVICE_GET_REGION_INFO,&c)==0){v->cfg=c.offset;v->cfgv=1;}
    return 0;
}
static void vfio_busmaster(struct vfio_ctx *v)
{
    if (!v->cfgv) return; uint16_t cmd=0;
    if (pread(v->device,&cmd,2,v->cfg+0x04)==2){ cmd|=0x0006;
        if (pwrite(v->device,&cmd,2,v->cfg+0x04)==2) printf("VFIO: BusMaster (CMD=0x%04x)\n",cmd); }
}
static int vfio_map(struct vfio_ctx *v, void *va, uint64_t iova, size_t sz)
{
    struct vfio_iommu_type1_dma_map m={.argsz=sizeof(m),
        .flags=VFIO_DMA_MAP_FLAG_READ|VFIO_DMA_MAP_FLAG_WRITE,
        .vaddr=(uint64_t)(uintptr_t)va,.iova=iova,.size=sz};
    if (ioctl(v->container,VFIO_IOMMU_MAP_DMA,&m)){perror("MAP_DMA");return -1;}
    return 0;
}
static void vfio_close(struct vfio_ctx *v){ if(v->bar0)munmap((void*)v->bar0,v->bar0_size);
    if(v->device>=0)close(v->device); if(v->group>=0)close(v->group); if(v->container>=0)close(v->container); }

/* ===================== бамп-аллокатор арены ===================== */
struct arena { uint8_t *va; uint64_t iova; uint64_t off, cap; };
static uint64_t arena_alloc(struct arena *a, uint64_t sz, uint8_t **va, uint64_t *iova)
{
    uint64_t o = a->off;
    a->off = (a->off + sz + 0xFFFu) & ~0xFFFull; /* выравнивание 4K */
    if (a->off > a->cap) { fprintf(stderr,"FAIL: арена переполнена\n"); return (uint64_t)-1; }
    if (va) *va = a->va + o;
    if (iova) *iova = a->iova + o;
    return o;
}

/* ===================== шаг FWSEC-FRTS (как fwsec_run_linux) ===================== */
static int do_fwsec_frts(const nv_mmio_t *io, uint64_t frts_addr, uint64_t frts_size,
                         uint64_t ucode_iova, uint8_t *ucode_va, size_t ucode_cap, uint32_t boot0)
{
    uint8_t *vbios = malloc(NV_VBIOS_SCAN_MAX);
    if (!vbios) return -1;
    for (uint32_t i=0;i<NV_VBIOS_SCAN_MAX;i+=4){ uint32_t w=bar_rd(io->ctx,NV_ROM_SHADOW_BASE+i);
        vbios[i]=w; vbios[i+1]=w>>8; vbios[i+2]=w>>16; vbios[i+3]=w>>24; }
    nv_fwsec_location_t loc; nv_fwsec_desc_t d;
    if (nv_fwsec_locate(vbios,NV_VBIOS_SCAN_MAX,&loc)!=NV_FWSEC_LOC_OK){fprintf(stderr,"FWSEC locate\n");free(vbios);return -1;}
    if (nv_fwsec_desc_parse(vbios+loc.desc_abs,NV_VBIOS_SCAN_MAX-loc.desc_abs,&d)!=NV_FWSEC_OK||d.version!=3){
        fprintf(stderr,"FWSEC desc\n");free(vbios);return -1;}
    size_t uoff=loc.desc_abs+d.hdr_size, usz=(size_t)d.imem_load_size+d.dmem_load_size;
    if (usz>ucode_cap){fprintf(stderr,"FWSEC ucode>буфер\n");free(vbios);return -1;}
    memset(ucode_va,0,ucode_cap); memcpy(ucode_va,vbios+uoff,usz);
    if (nv_fwsec_patch_frts(ucode_va,ucode_cap,&d,frts_addr,frts_size)!=NV_FWSEC_OK){fprintf(stderr,"FRTS patch\n");free(vbios);return -1;}
    if (d.signature_count){ uint32_t fv=0;
        if (nv_falcon_signature_fuse_version_ga102(io,d.engine_id_mask,d.ucode_id,&fv)!=NV_OK){free(vbios);return -1;}
        uint32_t si=0; if (nv_fwsec_select_signature_index(&d,fv,&si)!=NV_FWSEC_OK){fprintf(stderr,"FWSEC sig idx\n");free(vbios);return -1;}
        size_t sat=loc.desc_abs+FALCON_DESC_V3_SIZE+(size_t)si*NV_BCRT30_RSA3K_SIG_SIZE;
        if (nv_fwsec_patch_signature(ucode_va,ucode_cap,&d,vbios+sat)!=NV_FWSEC_OK){free(vbios);return -1;}
    }
    free(vbios);
    if (nv_falcon_reset_ga102(io,NV_PGSP_FALCON_BASE,NV_PGSP_FALCON2_BASE,boot0,TIMEOUT_US)!=NV_OK){fprintf(stderr,"FWSEC reset\n");return -1;}
    nv_dma_load_target_t im={0,d.imem_phys_base,d.imem_load_size}, dm={d.imem_load_size,d.dmem_phys_base,d.dmem_load_size};
    if (nv_falcon_dma_load_ga102(io,NV_PGSP_FALCON_BASE,NV_PGSP_FALCON2_BASE,ucode_iova,&im,&dm,
                                 d.pkc_data_offset,d.engine_id_mask,d.ucode_id,0,TIMEOUT_US)!=NV_OK){fprintf(stderr,"FWSEC dma_load\n");return -1;}
    uint32_t mb=0xFFFFFFFFu;
    nv_falcon_boot(io,NV_PGSP_FALCON_BASE,1,0,0,0,TIMEOUT_US,&mb);
    uint64_t lo=0,hi=0; int set=nv_wpr2_is_set(io,&lo,&hi);
    printf("FWSEC-FRTS: mbox0=0x%08x WPR2 set=%d [0x%llx..0x%llx]\n",mb,set,(unsigned long long)lo,(unsigned long long)hi);
    return (mb==0 && set) ? 0 : -1;
}

/* ===================== главный прогон ===================== */
static int run(const nv_mmio_t *io, struct arena *ar)
{
    uint32_t boot0 = bar_rd(io->ctx,0x0);
    printf("PMC_BOOT_0 = 0x%08x\n", boot0);
    if (nv_wait_gfw_boot_completed(io,4u*1000000u)!=NV_OK){fprintf(stderr,"FAIL: GFW boot\n");return -1;}
    uint64_t vram = nv_fb_vidmem_size(io);
    uint64_t frts_addr=0, frts_size=0;
    if (nv_fb_compute_frts(io,&frts_addr,&frts_size)!=0){fprintf(stderr,"FAIL: FRTS region\n");return -1;}
    printf("VRAM=%llu МиБ; FRTS=0x%llx sz=0x%llx\n",(unsigned long long)(vram>>20),
           (unsigned long long)frts_addr,(unsigned long long)frts_size);

    /* --- арена: выделить регионы --- */
    uint8_t *fwsec_va,*booter_va,*fwimg_va,*lvl2_va,*lvl1_va,*lvl0_va,*bl_va,*sig_va,*meta_va,*libos_va,
            *loginit_va,*logintr_va,*logrm_va,*rmargs_va,*shm_va;
    uint64_t fwsec_io,booter_io,fwimg_io,lvl2_io,lvl1_io,lvl0_io,bl_io,sig_io,meta_io,libos_io,
             loginit_io,logintr_io,logrm_io,rmargs_io,shm_io;
    nv_gsp_shm_layout_t shm_lay; nv_gsp_shm_compute(&shm_lay);

    /* GSP-RM ELF + bootloader (для размеров). */
    uint8_t *gsp=NULL; size_t gsp_len=0;
    if (nv_fw_blob_get("gsp",&gsp,&gsp_len)!=NV_FW_BLOB_OK){fprintf(stderr,"FAIL: gsp блоб\n");return -1;}
    nv_gsp_sections_t sec;
    if (nv_gsp_rm_sections(gsp,gsp_len,".fwsignature_ad10x",&sec)!=NV_GSP_OK){fprintf(stderr,"FAIL: ELF секции\n");nv_fw_blob_free(gsp);return -1;}
    uint8_t *blb=NULL; size_t blb_len=0;
    if (nv_fw_blob_get("bootloader",&blb,&blb_len)!=NV_FW_BLOB_OK){fprintf(stderr,"FAIL: bootloader блоб\n");nv_fw_blob_free(gsp);return -1;}
    nv_gsp_bootloader_t bl;
    if (nv_gsp_bootloader_parse(blb,blb_len,&bl)!=NV_GSP_OK){fprintf(stderr,"FAIL: bootloader desc\n");nv_fw_blob_free(gsp);nv_fw_blob_free(blb);return -1;}
    uint32_t n2=0,l2p=0;
    if (nv_gsp_radix3_levels(sec.fwimage_size,&n2,&l2p)!=NV_GSP_OK){fprintf(stderr,"FAIL: radix3 levels\n");nv_fw_blob_free(gsp);nv_fw_blob_free(blb);return -1;}

    if (arena_alloc(ar,0x100000,&fwsec_va,&fwsec_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,0x100000,&booter_va,&booter_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,sec.fwimage_size,&fwimg_va,&fwimg_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,(uint64_t)l2p*0x1000,&lvl2_va,&lvl2_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,0x1000,&lvl1_va,&lvl1_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,0x1000,&lvl0_va,&lvl0_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,bl.data_size,&bl_va,&bl_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,sec.sig_size,&sig_va,&sig_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,0x1000,&meta_va,&meta_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,0x1000,&libos_va,&libos_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,NV_GSP_LIBOS_LOG_SIZE,&loginit_va,&loginit_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,NV_GSP_LIBOS_LOG_SIZE,&logintr_va,&logintr_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,NV_GSP_LIBOS_LOG_SIZE,&logrm_va,&logrm_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,0x1000,&rmargs_va,&rmargs_io)==(uint64_t)-1) goto oom;
    if (arena_alloc(ar,shm_lay.total_size,&shm_va,&shm_io)==(uint64_t)-1) goto oom;

    /* --- 1. FWSEC-FRTS → WPR2 --- */
    if (do_fwsec_frts(io,frts_addr,frts_size,fwsec_io,fwsec_va,0x100000,boot0)!=0){
        fprintf(stderr,"FAIL: FWSEC-FRTS не создал WPR2\n"); nv_fw_blob_free(gsp); nv_fw_blob_free(blb); return -1; }

    /* --- 2. staging GSP-RM в sysmem --- */
    memcpy(fwimg_va, gsp+sec.fwimage_off, sec.fwimage_size);
    memcpy(sig_va,   gsp+sec.sig_off,     sec.sig_size);
    memcpy(bl_va,    blb+bl.data_abs,     bl.data_size);
    nv_fw_blob_free(gsp); nv_fw_blob_free(blb);
    memset(lvl0_va,0,0x1000); memset(lvl1_va,0,0x1000); memset(lvl2_va,0,(size_t)l2p*0x1000);
    if (nv_gsp_radix3_fill(sec.fwimage_size,lvl0_va,lvl1_va,lvl2_va,fwimg_io,lvl2_io,lvl1_io)!=NV_GSP_OK){
        fprintf(stderr,"FAIL: radix3 fill\n"); return -1; }

    nv_gsp_fb_layout_t lay;
    if (nv_gsp_fb_layout(vram,frts_addr,frts_size,bl.data_size,sec.fwimage_size,&lay)!=NV_GSP_OK){
        fprintf(stderr,"FAIL: fb_layout\n"); return -1; }
    nv_gsp_wpr_meta_src_t src = {
        .radix3_lvl0_dma=lvl0_io, .radix3_elf_size=sec.fwimage_size,
        .bootloader_dma=bl_io, .bootloader_size=bl.data_size,
        .boot_code_offset=bl.code_offset, .boot_data_offset=bl.data_offset, .boot_manifest_offset=bl.manifest_offset,
        .signature_dma=sig_io, .signature_size=sec.sig_size,
        .vga_workspace_addr=frts_addr+frts_size, .vga_workspace_size=0,
    };
    memset(meta_va,0,0x1000);
    if (nv_gsp_wpr_meta_build(meta_va,0x1000,&lay,&src)!=NV_GSP_OK){fprintf(stderr,"FAIL: wpr_meta\n");return -1;}

    nv_gsp_libos_region_t regs[4]={
        {"LOGINIT",loginit_io,NV_GSP_LIBOS_LOG_SIZE},{"LOGINTR",logintr_io,NV_GSP_LIBOS_LOG_SIZE},
        {"LOGRM",logrm_io,NV_GSP_LIBOS_LOG_SIZE},{"RMARGS",rmargs_io,0x1000}};
    size_t libos_sz=0; memset(libos_va,0,0x1000);
    if (nv_gsp_libos_build_args(libos_va,0x1000,regs,4,&libos_sz)!=NV_GSP_OK){fprintf(stderr,"FAIL: libos\n");return -1;}
    memset(loginit_va,0,NV_GSP_LIBOS_LOG_SIZE); memset(logintr_va,0,NV_GSP_LIBOS_LOG_SIZE);
    memset(logrm_va,0,NV_GSP_LIBOS_LOG_SIZE); memset(rmargs_va,0,0x1000);
    nv_gsp_pte_array_fill(loginit_va,NV_GSP_LIBOS_LOG_SIZE,8,loginit_io,NV_GSP_LIBOS_LOG_SIZE);
    nv_gsp_pte_array_fill(logintr_va,NV_GSP_LIBOS_LOG_SIZE,8,logintr_io,NV_GSP_LIBOS_LOG_SIZE);
    nv_gsp_pte_array_fill(logrm_va,NV_GSP_LIBOS_LOG_SIZE,8,logrm_io,NV_GSP_LIBOS_LOG_SIZE);

    /* Очереди RPC (shared mem) + rmargs (GSP_ARGUMENTS_CACHED → RMARGS-регион). */
    if (nv_gsp_shm_init(shm_va,shm_lay.total_size,shm_io,&shm_lay)!=NV_GSP_RPC_OK){fprintf(stderr,"FAIL: shm_init\n");return -1;}
    if (nv_gsp_rmargs_build(rmargs_va,0x1000,shm_io,&shm_lay)!=NV_GSP_RPC_OK){fprintf(stderr,"FAIL: rmargs\n");return -1;}
    printf("RPC shm@0x%llx (cmdq@0x%llx msgq@0x%llx ptes=%u msgCount=%u); rmargs@0x%llx\n",
           (unsigned long long)shm_io,(unsigned long long)(shm_io+shm_lay.cmdq_off),
           (unsigned long long)(shm_io+shm_lay.msgq_off),shm_lay.ptes_nr,shm_lay.msg_count,
           (unsigned long long)rmargs_io);
    printf("staging: fwimage@0x%llx radix3-lvl0@0x%llx bl@0x%llx sig@0x%llx meta@0x%llx libos@0x%llx\n",
           (unsigned long long)fwimg_io,(unsigned long long)lvl0_io,(unsigned long long)bl_io,
           (unsigned long long)sig_io,(unsigned long long)meta_io,(unsigned long long)libos_io);
    printf("WPR2: start=0x%llx end=0x%llx heap=0x%llx elf=0x%llx boot=0x%llx\n",
           (unsigned long long)lay.wpr2_addr,(unsigned long long)lay.wpr2_end,
           (unsigned long long)lay.heap_addr,(unsigned long long)lay.elf_addr,(unsigned long long)lay.boot_addr);

    /* --- 3. reset GSP в RISC-V, GSP mailbox = libos --- */
    if (nv_falcon_gsp_reset_riscv(io,NV_PGSP_FALCON_BASE,NV_PGSP_FALCON2_BASE,TIMEOUT_US)!=NV_OK){
        fprintf(stderr,"FAIL: GSP reset(RISC-V)\n"); return -1; }
    nv_falcon_write_mailbox0(io,NV_PGSP_FALCON_BASE,(uint32_t)libos_io);
    nv_falcon_write_mailbox1(io,NV_PGSP_FALCON_BASE,(uint32_t)(libos_io>>32));
    printf("GSP reset(RISC-V) OK; GSP mbox=libos 0x%llx\n",(unsigned long long)libos_io);

    /* --- 4. SEC2 Booter с mbox = WprMeta --- */
    uint8_t *bb=NULL; size_t bb_len=0;
    if (nv_fw_blob_get("booter_load",&bb,&bb_len)!=NV_FW_BLOB_OK){fprintf(stderr,"FAIL: booter_load\n");return -1;}
    nv_booter_desc_t bd;
    if (nv_booter_parse(bb,bb_len,&bd)!=NV_BOOTER_OK){fprintf(stderr,"FAIL: booter parse\n");nv_fw_blob_free(bb);return -1;}
    memset(booter_va,0,0x100000); memcpy(booter_va,bb+bd.data_abs,bd.data_size);
    uint32_t rf=0; if (nv_falcon_signature_fuse_version_ga102(io,(uint16_t)bd.engine_id_mask,(uint8_t)bd.ucode_id,&rf)!=NV_OK){nv_fw_blob_free(bb);return -1;}
    uint32_t bi=0; if (nv_booter_select_signature(&bd,rf,&bi)!=NV_BOOTER_OK){fprintf(stderr,"FAIL: booter sig\n");nv_fw_blob_free(bb);return -1;}
    size_t bsat=(size_t)bd.sig_prod_abs+(size_t)bi*NV_BOOTER_SIG_SIZE;
    memcpy(booter_va+bd.patch_loc, bb+bsat, NV_BOOTER_SIG_SIZE);
    nv_fw_blob_free(bb);

    if (nv_falcon_reset_ga102(io,NV_PSEC_FALCON_BASE,NV_PSEC_FALCON2_BASE,boot0,TIMEOUT_US)!=NV_OK){fprintf(stderr,"FAIL: SEC2 reset\n");return -1;}
    nv_dma_load_target_t bim={bd.app[0].code_offset,0,bd.app[0].code_size}, bdm={bd.os_data_offset,0,bd.os_data_size};
    if (nv_falcon_dma_load_ga102(io,NV_PSEC_FALCON_BASE,NV_PSEC_FALCON2_BASE,booter_io,&bim,&bdm,
                                 bd.pkc_data_offset,(uint16_t)bd.engine_id_mask,(uint8_t)bd.ucode_id,
                                 bd.app[0].code_offset,TIMEOUT_US)!=NV_OK){fprintf(stderr,"FAIL: SEC2 dma_load\n");return -1;}
    uint32_t mb0=0xFFFFFFFFu;
    int brc=nv_falcon_boot(io,NV_PSEC_FALCON_BASE,1,(uint32_t)meta_io,1,(uint32_t)(meta_io>>32),TIMEOUT_US,&mb0);
    uint32_t mb1=nv_falcon_read_mailbox1(io,NV_PSEC_FALCON_BASE);
    printf("Booter: rc=%d mbox0=0x%08x mbox1=0x%08x (WprMeta=0x%llx)\n",brc,mb0,mb1,(unsigned long long)meta_io);

    /* --- 5. FALCON_OS = app_version; проверка RISC-V active --- */
    nv_falcon_write_os_version(io,NV_PGSP_FALCON_BASE,bl.app_version);
    int active=0;
    for (int i=0;i<200;i++){ if (nv_falcon_riscv_active(io,NV_PGSP_FALCON2_BASE)){active=1;break;} bar_udelay(NULL,1000); }
    uint32_t rv=bar_rd(io->ctx,NV_PGSP_FALCON2_BASE+NV_PRISCV_RISCV_CPUCTL_OFF);
    printf("GSP RISC-V: active=%d (CPUCTL=0x%08x)\n",active,rv);

    /* --- задача 7: ждём ответа GSP-RM в status-очереди (событие GSP_INIT_DONE) --- */
    uint32_t wptr=0; int got=0;
    for (int i=0;i<2000 && active;i++){ wptr=nv_gsp_msgq_writeptr(shm_va,&shm_lay); if(wptr){got=1;break;} bar_udelay(NULL,5000); }
    uint32_t sig=0,fn=0,len=0;
    if (got){
        const uint8_t *e=shm_va+shm_lay.msgq_off+NV_GSP_QUEUE_ENTRYOFF; /* запись [0] */
        const uint8_t *r=e+NV_GSP_MSG_ELEM_HDR_SIZE;
        sig=r[4]|(r[5]<<8)|(r[6]<<16)|((uint32_t)r[7]<<24);
        len=r[8]|(r[9]<<8)|(r[10]<<16)|((uint32_t)r[11]<<24);
        fn =r[12]|(r[13]<<8)|(r[14]<<16)|((uint32_t)r[15]<<24);
        printf("GSP RPC: msgq.writePtr=%u rpc.signature=0x%08x len=%u function=0x%08x\n",wptr,sig,len,fn);
    } else printf("GSP RPC: msgq.writePtr=0 — GSP не записал сообщение за ~10с\n");

    /* --- диагностика: тронул ли GSP очереди/логи --- */
    {
        const uint8_t *cq = shm_va + shm_lay.cmdq_off, *mq = shm_va + shm_lay.msgq_off;
        uint32_t cmd_rx = *(volatile uint32_t*)(cq + NV_GSP_MSGQ_RXHDROFF); /* GSP читает cmdq? */
        uint32_t msg_wp = *(volatile uint32_t*)(mq + NV_MSGQ_TX_WRITEPTR_OFF);
        uint32_t gmb0 = nv_falcon_read_mailbox0(io, NV_PGSP_FALCON_BASE);
        uint32_t gmb1 = nv_falcon_read_mailbox1(io, NV_PGSP_FALCON_BASE);
        printf("ДИАГ: cmdq.rx.readPtr=%u msgq.tx.writePtr=%u GSPmbox0=0x%08x mbox1=0x%08x\n",
               cmd_rx, msg_wp, gmb0, gmb1);
        const char *names[3] = {"LOGINIT","LOGINTR","LOGRM"};
        uint8_t *logs[3] = {loginit_va, logintr_va, logrm_va};
        for (int k = 0; k < 3; k++) {
            uint64_t put = *(volatile uint64_t*)logs[k]; /* put-указатель @0 */
            int nz = 0; for (int j = 0x1000; j < 0x1400; j++) if (logs[k][j]) { nz = 1; break; }
            printf("ДИАГ: %-8s put=0x%llx данные@0x1000:%s первые: %02x %02x %02x %02x %02x %02x %02x %02x\n",
                   names[k], (unsigned long long)put, nz?"ЕСТЬ":"нет",
                   logs[k][0x1000],logs[k][0x1001],logs[k][0x1002],logs[k][0x1003],
                   logs[k][0x1004],logs[k][0x1005],logs[k][0x1006],logs[k][0x1007]);
        }
    }

    int rpc_ok = (got && sig==NV_GSP_RPC_SIGNATURE);
    if (brc==NV_OK && mb0==0 && active && rpc_ok){
        printf("\n*** GSP-RM ОТВЕТИЛ ПО RPC (function=0x%08x) — СЛОЙ 2 ЗАВЕРШЁН ***\n",fn);
        return 0;
    }
    if (brc==NV_OK && mb0==0 && active){
        printf("\n*** GSP-RM ЗАГРУЖЕН (задача 6: mbox0=0, RISC-V active), но RPC-ответа нет (задача 7) ***\n");
        return 1; /* частичный успех */
    }
    fprintf(stderr,"\nНЕ достигнуто: mbox0=0x%08x active=%d\n",mb0,active);
    return -1;
oom:
    nv_fw_blob_free(gsp); nv_fw_blob_free(blb); return -1;
}

/* ===================== авто-детект + main ===================== */
static int find_bdf(char *out, size_t n)
{
    DIR *d=opendir("/sys/bus/pci/devices"); if(!d)return -1; struct dirent *e; char best[40]={0};
    while ((e=readdir(d))){ if(e->d_name[0]=='.')continue; char p[300],b[32]; FILE*f; unsigned ven=0,cl=0;
        snprintf(p,sizeof(p),"/sys/bus/pci/devices/%s/vendor",e->d_name); if(!(f=fopen(p,"r")))continue;
        if(fgets(b,sizeof(b),f))ven=strtoul(b,0,16); fclose(f); if(ven!=0x10de)continue;
        snprintf(p,sizeof(p),"/sys/bus/pci/devices/%s/class",e->d_name); if(!(f=fopen(p,"r")))continue;
        if(fgets(b,sizeof(b),f))cl=strtoul(b,0,16); fclose(f);
        if((cl>>16)==0x03){ if((cl>>8)==0x0300){snprintf(out,n,"%s",e->d_name);closedir(d);return 0;}
            if(!best[0])snprintf(best,sizeof(best),"%s",e->d_name);} }
    closedir(d); if(best[0]){snprintf(out,n,"%s",best);return 0;} return -1;
}

int main(int argc, char **argv)
{
    char ab[40]; const char *bdf=(argc>=2)?argv[1]:NULL;
    if (!bdf){ if(find_bdf(ab,sizeof(ab))==0){bdf=ab;printf("BDF: %s\n",bdf);} else {fprintf(stderr,"NVIDIA не найдена\n");return 2;} }
    struct vfio_ctx v; if (vfio_open(&v,bdf)){vfio_close(&v);return 1;} vfio_busmaster(&v);
    void *abuf=mmap(NULL,ARENA_SIZE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (abuf==MAP_FAILED){perror("mmap arena");vfio_close(&v);return 1;}
    if (vfio_map(&v,abuf,ARENA_IOVA,ARENA_SIZE)){munmap(abuf,ARENA_SIZE);vfio_close(&v);return 1;}
    printf("DMA-арена: VA=%p IOVA=0x%llx size=0x%x\n",abuf,(unsigned long long)ARENA_IOVA,ARENA_SIZE);
    struct arena ar={.va=abuf,.iova=ARENA_IOVA,.off=0,.cap=ARENA_SIZE};
    nv_mmio_t io={.ctx=(void*)v.bar0,.rd=bar_rd,.wr=bar_wr,.udelay=bar_udelay};
    int rc=run(&io,&ar);
    struct vfio_iommu_type1_dma_unmap u={.argsz=sizeof(u),.iova=ARENA_IOVA,.size=ARENA_SIZE};
    ioctl(v.container,VFIO_IOMMU_UNMAP_DMA,&u); munmap(abuf,ARENA_SIZE); vfio_close(&v);
    printf("\n=== РЕЗУЛЬТАТ: %s ===\n", rc==0?"OK (GSP-RM загружен, RISC-V active)":"FAIL (см. лог)");
    return rc==0?0:1;
}
