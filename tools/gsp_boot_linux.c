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
#include "../driver/gsp/gsp_rm.h"
#include "../driver/gsp/gmmu.h"
#include "../driver/gsp/gsp_fifo.h"
#include "../driver/gsp/gsp_disp.h"
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

/* Прочитать физ. BAR0/1/3 и PCI BDF-id из sysfs (для GspSystemInfo). */
static int read_pci_bars(const char *bdf, uint64_t *bar0, uint64_t *bar1, uint64_t *bar3, uint64_t *devid)
{
    char p[300]; snprintf(p,sizeof(p),"/sys/bus/pci/devices/%s/resource",bdf);
    FILE *f=fopen(p,"r"); if(!f) return -1;
    uint64_t s[6]={0};
    for(int i=0;i<6;i++){ unsigned long long a=0,e=0,fl=0;
        if(fscanf(f,"%llx %llx %llx",&a,&e,&fl)!=3) break; s[i]=a; }
    fclose(f);
    *bar0=s[0]; *bar1=s[1]; *bar3=s[3];
    unsigned dom=0,bus=0,dev=0,fn=0;
    if(sscanf(bdf,"%x:%x:%x.%x",&dom,&bus,&dev,&fn)==4)
        *devid=((uint64_t)bus<<8)|(uint64_t)(((dev&0x1f)<<3)|(fn&7));
    else *devid=0x100;
    return 0;
}

/* ===================== CPU-секвенсер (GSP_RUN_CPU_SEQUENCER) =====================
 * GSP-RM шлёт список I/O-команд, которые CPU должен выполнить (спец-ресет GSP-ядра
 * в середине init). Порт nouveau r535_gsp_msg_run_cpu_sequencer + rmgspseq.h. */
enum { SEQ_REG_WRITE=0, SEQ_REG_MODIFY, SEQ_REG_POLL, SEQ_DELAY_US, SEQ_REG_STORE,
       SEQ_CORE_RESET, SEQ_CORE_START, SEQ_CORE_WAIT_HALT, SEQ_CORE_RESUME };

struct seq_ctx {
    const nv_mmio_t *io; uint32_t boot0; uint64_t libos_io, meta_io, booter_io;
    uint32_t app_version; const nv_booter_desc_t *bd;
};

/* Колбэки канала двустороннего RPC (слой 3): дверной звонок GSP + задержка опроса.
   io_ctx = наш nv_mmio_t* (через него bar_wr/bar_udelay). */
static void gsp_rpc_doorbell(void *p)
{ const nv_mmio_t *io = (const nv_mmio_t *)p; bar_wr(io->ctx, NV_PGSP_FALCON_BASE + 0xc00, 0); }
static void gsp_rpc_udelay(void *p, uint32_t us) { (void)p; bar_udelay(NULL, us); }

static int seq_poll(const nv_mmio_t *io, uint32_t addr, uint32_t mask, uint32_t val, uint32_t usec)
{
    uint32_t w=0; if(!usec) usec=4000000u;
    for(;;){ if((bar_rd(io->ctx,addr)&mask)==val) return 0;
        if(w>=usec) return -1; bar_udelay(NULL,10); w+=10; }
}

/* CORE_RESUME (точно по nouveau): ресет GSP в RISC-V + mbox=libos + РЕСТАРТ уже
   загруженного SEC2 Booter (без reset/reload — WPR2 уже расширен) + ожидание
   0x1180f8 bit26 + проверки mbox0/active. */
static int seq_core_resume(struct seq_ctx *c)
{
    const nv_mmio_t *io=c->io;
    if (nv_falcon_gsp_reset_riscv(io,NV_PGSP_FALCON_BASE,NV_PGSP_FALCON2_BASE,TIMEOUT_US)!=NV_OK) return -1;
    nv_falcon_write_mailbox0(io,NV_PGSP_FALCON_BASE,(uint32_t)c->libos_io);
    nv_falcon_write_mailbox1(io,NV_PGSP_FALCON_BASE,(uint32_t)(c->libos_io>>32));
    nv_falcon_start(io, NV_PSEC_FALCON_BASE);   /* nvkm_falcon_start(&sec2->falcon) */
    if (seq_poll(io, 0x1180f8, 0x04000000, 0x04000000, 2000000)!=0){
        printf("  CORE_RESUME: 0x1180f8 bit26 таймаут\n"); return -1; }
    uint32_t mb0 = nv_falcon_read_mailbox0(io, NV_PSEC_FALCON_BASE);
    if (mb0!=0){ printf("  CORE_RESUME: SEC2 mbox0=0x%08x (ожид. 0)\n",mb0); return -1; }
    nv_falcon_write_os_version(io,NV_PGSP_FALCON_BASE,c->app_version);
    if (!nv_falcon_riscv_active(io,NV_PGSP_FALCON2_BASE)){ printf("  CORE_RESUME: RISC-V не active\n"); return -1; }
    return 0;
}

static int exec_cpu_sequencer(struct seq_ctx *c, const uint32_t *cb, uint32_t cmdIndex, uint32_t *regSave)
{
    const nv_mmio_t *io=c->io;
    uint32_t ptr=0, nrw=0,nrp=0,ncore=0;
    while (ptr < cmdIndex){
        uint32_t op=cb[ptr++];
        switch(op){
        case SEQ_REG_WRITE: { uint32_t a=cb[ptr],v=cb[ptr+1]; ptr+=2; bar_wr(io->ctx,a,v); nrw++; } break;
        case SEQ_REG_MODIFY:{ uint32_t a=cb[ptr],m=cb[ptr+1],v=cb[ptr+2]; ptr+=3;
                              bar_wr(io->ctx,a,(bar_rd(io->ctx,a)&~m)|v); } break;
        case SEQ_REG_POLL:  { uint32_t a=cb[ptr],m=cb[ptr+1],v=cb[ptr+2],to=cb[ptr+3]; ptr+=5; nrp++;
                              if(seq_poll(io,a,m,v,to)!=0){ printf("  seq REG_POLL таймаут @0x%x\n",a); return -1; } } break;
        case SEQ_DELAY_US:  { uint32_t u=cb[ptr]; ptr+=1; bar_udelay(NULL,u); } break;
        case SEQ_REG_STORE: { uint32_t a=cb[ptr],idx=cb[ptr+1]; ptr+=2; if(idx<8) regSave[idx]=bar_rd(io->ctx,a); } break;
        case SEQ_CORE_RESET:
            ncore++;
            if (nv_falcon_reset_ga102(io,NV_PGSP_FALCON_BASE,NV_PGSP_FALCON2_BASE,c->boot0,TIMEOUT_US)!=NV_OK) return -1;
            { uint32_t a=NV_PGSP_FALCON_BASE+0x624; bar_wr(io->ctx,a,(bar_rd(io->ctx,a)&~0x80u)|0x80u); }
            bar_wr(io->ctx,NV_PGSP_FALCON_BASE+0x10c,0);
            break;
        case SEQ_CORE_START:     ncore++; nv_falcon_start(io,NV_PGSP_FALCON_BASE); break;
        case SEQ_CORE_WAIT_HALT: ncore++;
            if (nv_falcon_wait_halted(io,NV_PGSP_FALCON_BASE,TIMEOUT_US)!=NV_OK){ printf("  seq CORE_WAIT_HALT таймаут\n"); return -1; }
            break;
        case SEQ_CORE_RESUME:    ncore++;
            if (seq_core_resume(c)!=0) return -1;
            break;
        default: printf("  seq неизвестный опкод %u @%u\n",op,ptr-1); return -1;
        }
    }
    printf("  seq выполнен: REG_WRITE=%u REG_POLL=%u CORE-ops=%u\n",nrw,nrp,ncore);
    return 0;
}

/* ===================== главный прогон ===================== */
static int run(const nv_mmio_t *io, struct arena *ar, const char *bdf)
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

    /* ФАКТИЧЕСКИЙ верх WPR2 после FWSEC (FWSEC клампит FRTS под VGA-workspace на ~128КБ).
       gspFwWprEnd/frtsSize должны соответствовать реальному WPR2, иначе GSP-RM адресует
       незащищённую память сверху и падает. */
    uint64_t w_lo=0, w_hi=0; nv_wpr2_is_set(io,&w_lo,&w_hi);
    uint64_t real_frts_size = (w_hi>frts_addr) ? (w_hi - frts_addr) : frts_size;
    printf("FWSEC WPR2 факт.: [0x%llx..0x%llx] → frtsSize=0x%llx (вместо 0x%llx)\n",
           (unsigned long long)w_lo,(unsigned long long)w_hi,
           (unsigned long long)real_frts_size,(unsigned long long)frts_size);

    /* ВАЖНО (проверено HW 2026-06-30): Booter ТРЕБУЕТ gspFwWprEnd = frts_addr + НОМИНАЛЬНЫЕ
       0x100000 (а не фактический верх WPR2 0x2ff8e0000, куда FWSEC клампит FRTS под VGA-WS).
       Любое иное gspFwWprEnd → Booter отвергает meta (mbox0=0x8d, RISC-V не стартует).
       Рассинхрон meta(end)↔HW-WPR2(end) на 0x20000 штатный — его создаёт сам Booter, GSP-RM
       к нему устойчив. Поэтому раскладка — по НОМИНАЛЬНОМУ frts_size. */
    (void)real_frts_size; /* только для диагностики (печать выше) */
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

    /* --- 2.5: пре-бутовые RPC в cmdq (как r535_gsp_oneinit): SET_SYSTEM_INFO + SET_REGISTRY.
       GSP-RM при ранней инициализации читает их из cmdq и блокируется без них. --- */
    {
        uint64_t bar0=0,bar1=0,bar3=0,devid=0x100;
        if (read_pci_bars(bdf,&bar0,&bar1,&bar3,&devid)!=0)
            fprintf(stderr,"WARN: не прочитал BAR из sysfs — sysinfo с нулями\n");
        uint8_t *cmdq = shm_va + shm_lay.cmdq_off;
        uint8_t sysinfo[NV_GSP_SYSINFO_SIZE];
        nv_gsp_build_sysinfo(sysinfo,bar0,bar1,bar3,devid);
        uint8_t reg[256]; uint32_t reglen = nv_gsp_build_registry(reg,sizeof(reg));
        uint32_t s0 = nv_gsp_cmdq_write(cmdq,0,0,NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO,sysinfo,NV_GSP_SYSINFO_SIZE);
        uint32_t s1 = nv_gsp_cmdq_write(cmdq,s0,1,NV_VGPU_MSG_FUNCTION_SET_REGISTRY,reg,reglen);
        nv_gsp_cmdq_set_writeptr(shm_va,&shm_lay,s0+s1);
        printf("cmdq: SET_SYSTEM_INFO(слоты 0..%u) + SET_REGISTRY(слот %u) writePtr=%u; "
               "bar0=0x%llx bar1=0x%llx bar3=0x%llx bdf=0x%llx reglen=%u\n",
               s0-1,s0,s0+s1,(unsigned long long)bar0,(unsigned long long)bar1,
               (unsigned long long)bar3,(unsigned long long)devid,reglen);
    }

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

    /* ДИАГ: расширил ли Booter WPR2 на полный регион GSP-RM? */
    {
        uint64_t wlo=0,whi=0; int ws=nv_wpr2_is_set(io,&wlo,&whi);
        printf("ДИАГ WPR2 после Booter: set=%d [0x%llx..0x%llx] (ожид. 0x%llx..0x%llx)\n",
               ws,(unsigned long long)wlo,(unsigned long long)whi,
               (unsigned long long)lay.wpr2_addr,(unsigned long long)lay.wpr2_end);
        /* RISC-V статус-регистры (поиск фолта/исключения) PFALCON2 GSP */
        printf("ДИАГ GSP PRISCV:");
        uint32_t roff[]={0x388,0x100,0x104,0x108,0x10c,0x110,0x120,0x130,0x388,0x668,0x800,0x804};
        for (unsigned k=0;k<sizeof(roff)/sizeof(roff[0]);k++)
            printf(" [+0x%x]=0x%08x", roff[k], bar_rd(io->ctx, NV_PGSP_FALCON2_BASE+roff[k]));
        printf("\n");
    }

    /* --- 5. FALCON_OS = app_version; проверка RISC-V active --- */
    nv_falcon_write_os_version(io,NV_PGSP_FALCON_BASE,bl.app_version);
    int active=0;
    for (int i=0;i<200;i++){ if (nv_falcon_riscv_active(io,NV_PGSP_FALCON2_BASE)){active=1;break;} bar_udelay(NULL,1000); }
    uint32_t rv=bar_rd(io->ctx,NV_PGSP_FALCON2_BASE+NV_PRISCV_RISCV_CPUCTL_OFF);
    printf("GSP RISC-V: active=%d (CPUCTL=0x%08x)\n",active,rv);

    /* Дверной звонок GSP (falcon+0xc00=0): уведомить, что в cmdq есть команды
       (SET_SYSTEM_INFO/SET_REGISTRY). Как nvkm_falcon_wr32(&gsp->falcon,0xc00,0). */
    bar_wr(io->ctx, NV_PGSP_FALCON_BASE + 0xc00, 0);

    /* --- задача 7: дренируем msgq до события GSP_INIT_DONE (0x1001) ---
       GSP-RM шлёт поток событий; читаем каждое, двигаем readPtr (consume), ищем INIT_DONE. */
    uint32_t rptr=0, last_fn=0, sig=0, msgs=0, len=0; int got=0;
    for (int i=0;i<8000 && active && !got;i++){
        uint32_t wptr=nv_gsp_msgq_writeptr(shm_va,&shm_lay);
        while (rptr != wptr){
            const uint8_t *e=shm_va+shm_lay.msgq_off+NV_GSP_QUEUE_ENTRYOFF+(size_t)rptr*NV_GSP_QUEUE_MSGSIZE;
            uint32_t ec=*(const volatile uint32_t*)(e+NV_GSP_MSG_ELEMCOUNT_OFF);
            if(!ec||ec>shm_lay.msg_count) ec=1;
            const uint8_t *r=e+NV_GSP_MSG_ELEM_HDR_SIZE;
            sig=r[4]|(r[5]<<8)|(r[6]<<16)|((uint32_t)r[7]<<24);
            len=r[8]|(r[9]<<8)|(r[10]<<16)|((uint32_t)r[11]<<24);
            last_fn=r[12]|(r[13]<<8)|(r[14]<<16)|((uint32_t)r[15]<<24);
            msgs++;
            printf("GSP msg #%u @slot%u: sig=0x%08x function=0x%04x len=%u ec=%u%s\n",
                   msgs,rptr,sig,last_fn,len,ec,
                   last_fn==NV_VGPU_MSG_EVENT_GSP_INIT_DONE?"  <== GSP_INIT_DONE":"");
            /* GSP_RUN_CPU_SEQUENCER (0x1002): выполнить I/O-команды и перезапустить GSP-ядро */
            if (last_fn==0x1002u){
                uint8_t *pl=(uint8_t*)e+NV_GSP_RPC_PAYLOAD_OFF;
                uint32_t cmdIndex=*(uint32_t*)(pl+4);
                uint32_t *regSave=(uint32_t*)(pl+8);
                const uint32_t *cbuf=(const uint32_t*)(pl+40);
                struct seq_ctx sc={io,boot0,libos_io,meta_io,booter_io,bl.app_version,&bd};
                printf("  → выполняю CPU-секвенсер (cmdIndex=%u)...\n",cmdIndex);
                int sr=exec_cpu_sequencer(&sc,cbuf,cmdIndex,regSave);
                printf("  CPU-секвенсер: %s\n",sr==0?"OK — GSP-RM возобновлён":"FAIL");
            }
            rptr+=ec; while(rptr>=shm_lay.msg_count) rptr-=shm_lay.msg_count;
            *(volatile uint32_t*)(shm_va+shm_lay.msgq_off+NV_GSP_MSGQ_RXHDROFF)=rptr; /* consume */
            if (last_fn==NV_VGPU_MSG_EVENT_GSP_INIT_DONE){ got=1; break; }
        }
        if((i%200)==199) bar_wr(io->ctx, NV_PGSP_FALCON_BASE + 0xc00, 0); /* периодич. звонок */
        bar_udelay(NULL,5000);
    }
    uint32_t fn=last_fn;
    if (got) printf("GSP RPC: ★ GSP_INIT_DONE получен после %u сообщений ★\n",msgs);
    else if (msgs) printf("GSP RPC: GSP жив (%u сообщений, последнее function=0x%04x sig=0x%08x), INIT_DONE НЕ пришёл\n",msgs,last_fn,sig);
    else printf("GSP RPC: msgq пуст — GSP не записал сообщение\n");

    /* ===================== СЛОЙ 3 (проход A): двусторонний RPC =====================
       Канал уже поднят (INIT_DONE). Продолжаем с тех же указателей: cmdq.writePtr и
       msgq.readPtr, что оставил дренаж. Делаем первый двусторонний RPC
       GET_GSP_STATIC_INFO (карта FB-регионов), затем цепочку RM client→device→subdevice. */
    int l3_static_ok = 0, l3_chain_ok = 0, l3_ctrl_ok = 0, l3_vaspace_ok = 0;
    int l3_vram_ok = 0, l3_map_ok = 0;
    int l4_devinfo_ok = 0, l4_ce_engtype = -1, l4_chan_ok = 0, l4_bind_ok = 0, l4_sched_ok = 0;
    int l5_disp_ok = 0, l5_modeset_ok = 0, l5_scanout_ok = 0;
    int l4_ce_obj_ok = 0, l4_exec_ok = 0; uint32_t l4_ce_runlist = 0;
    if (got) {
        nv_gsp_rpc_chan ch;
        memset(&ch, 0, sizeof(ch));
        ch.shm = shm_va; ch.lay = shm_lay;
        /* cmdq.writePtr (=2 после пре-бута); пре-бут использовал seq 0..writePtr-1. */
        ch.cmdq_wptr = *(volatile uint32_t*)(shm_va + shm_lay.cmdq_off + NV_MSGQ_TX_WRITEPTR_OFF);
        ch.seq       = ch.cmdq_wptr;
        ch.msgq_rptr = rptr;                       /* финальный readPtr дренажа */
        ch.io_ctx = (void*)io; ch.ring = gsp_rpc_doorbell; ch.udelay = gsp_rpc_udelay;
        printf("\nСЛОЙ 3: RPC-канал cmdq.wptr=%u seq=%u msgq.rptr=%u\n",
               ch.cmdq_wptr, ch.seq, ch.msgq_rptr);

        /* --- метрика №1: GET_GSP_STATIC_INFO (65) → карта FB-регионов VRAM --- */
        nv_gsp_static_info si;
        int sirc = nv_gsp_get_static_info(&ch, &si);
        if (sirc == NV_GSP_RM_OK) {
            l3_static_ok = 1;
            printf("СЛОЙ 3: GET_GSP_STATIC_INFO OK — FB-регионов: %u\n", si.num_regions);
            for (uint32_t i = 0; i < si.num_regions; i++)
                printf("  FB[%u] %016llx..%016llx rsvd=%llx perf=0x%x comp=%u iso=%u prot=%u\n",
                       i, (unsigned long long)si.regions[i].base,
                       (unsigned long long)si.regions[i].limit,
                       (unsigned long long)si.regions[i].reserved,
                       si.regions[i].performance, si.regions[i].compressed,
                       si.regions[i].iso, si.regions[i].prot);
            printf("  GSP internal: hClient=0x%08x hDevice=0x%08x hSubdevice=0x%08x; bar1Pde=0x%llx bar2Pde=0x%llx\n",
                   si.h_client, si.h_device, si.h_subdevice,
                   (unsigned long long)si.bar1_pde, (unsigned long long)si.bar2_pde);
        } else {
            printf("СЛОЙ 3: GET_GSP_STATIC_INFO FAIL rc=%d\n", sirc);
        }

        /* --- метрика №2: цепочка RM-объектов client→device→subdevice (RM_ALLOC=103) --- */
        uint32_t hcli=0, hdev=0, hsub=0, st0=0xffffffff, st1=0xffffffff, st2=0xffffffff;
        int rc0 = nv_gsp_rm_client_ctor(&ch, &hcli, &st0);
        printf("СЛОЙ 3: RM client  rc=%d status=0x%x handle=0x%08x\n", rc0, st0, hcli);
        int rc1 = -1, rc2 = -1;
        if (rc0 == NV_GSP_RM_OK && st0 == 0) {
            rc1 = nv_gsp_rm_device_ctor(&ch, hcli, &hdev, &st1);
            printf("СЛОЙ 3: RM device  rc=%d status=0x%x handle=0x%08x\n", rc1, st1, hdev);
            if (rc1 == NV_GSP_RM_OK && st1 == 0) {
                rc2 = nv_gsp_rm_subdevice_ctor(&ch, hcli, hdev, &hsub, &st2);
                printf("СЛОЙ 3: RM subdev  rc=%d status=0x%x handle=0x%08x\n", rc2, st2, hsub);
            }
        }
        l3_chain_ok = (rc0==NV_GSP_RM_OK && st0==0 && rc1==NV_GSP_RM_OK && st1==0 &&
                       rc2==NV_GSP_RM_OK && st2==0);

        /* ===== ПРОХОД B: работа с памятью через RPC ===== */
        if (l3_chain_ok) {
            /* --- СЛОЙ 4 A0: таблица движков (FIFO_GET_DEVICE_INFO_TABLE) — до канала ---
               Читаем список движков GPU, берём engineType CE0 для канала (A2).
               Порт r535_fifo_runl_ctor. */
            {
                nv_gsp_fifo_devinfo di;
                uint32_t dst = 0xffffffffu;
                int drc = nv_gsp_fifo_get_device_info(&ch, hcli, hsub, &di, &dst);
                if (drc == NV_GSP_RM_OK && dst == 0) {
                    l4_devinfo_ok = 1;
                    printf("СЛОЙ 4 A0: FIFO device-info OK — движков: %u\n", di.count);
                    for (uint32_t i = 0; i < di.count; i++) {
                        uint32_t rt = di.engines[i].rm_engine_type;
                        const char *nm = "?";
                        if (rt == RM_ENGINE_TYPE_GR0) nm = "GR0";
                        else if (rt >= RM_ENGINE_TYPE_COPY0 && rt <= RM_ENGINE_TYPE_COPY9) nm = "COPYx";
                        else if (rt == RM_ENGINE_TYPE_SW) nm = "SW";
                        printf("  engn[%02u] rm_type=0x%02x(%s) runlist=%u pri_base=0x%x eng_desc=0x%x\n",
                               i, rt, nm, di.engines[i].runlist,
                               di.engines[i].runlist_pri_base, di.engines[i].eng_desc);
                    }
                    int ce = nv_gsp_fifo_find_engine(&di, RM_ENGINE_TYPE_COPY0);
                    if (ce >= 0) {
                        l4_ce_engtype = (int)di.engines[ce].rm_engine_type;
                        l4_ce_runlist = di.engines[ce].runlist;
                        printf("СЛОЙ 4 A0: CE0 найден — engineType=0x%x runlist=%u (для канала A2)\n",
                               l4_ce_engtype, di.engines[ce].runlist);
                    } else {
                        printf("СЛОЙ 4 A0: COPY0 в таблице не найден\n");
                    }
                } else {
                    printf("СЛОЙ 4 A0: FIFO device-info FAIL rc=%d status=0x%x\n", drc, dst);
                }
            }

            /* --- B-метрика №1: RM_CONTROL FB_GET_INFO_V2 — конфиг VRAM/heap с GSP --- */
            uint32_t idx[5] = { NV2080_CTRL_FB_INFO_INDEX_RAM_SIZE,
                                NV2080_CTRL_FB_INFO_INDEX_TOTAL_RAM_SIZE,
                                NV2080_CTRL_FB_INFO_INDEX_HEAP_SIZE,
                                NV2080_CTRL_FB_INFO_INDEX_HEAP_FREE,
                                NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE };
            uint32_t val[5] = {0,0,0,0,0}, cst = 0xffffffff;
            int crc = nv_gsp_fb_get_info(&ch, hcli, hsub, idx, val, 5, &cst);
            if (crc == NV_GSP_RM_OK && cst == 0) {
                l3_ctrl_ok = 1;
                printf("СЛОЙ 3B: FB_GET_INFO_V2 OK (KiB): RAM=%u TOTAL_RAM=%u HEAP=%u HEAP_FREE=%u BAR1=%u\n",
                       val[0], val[1], val[2], val[3], val[4]);
            } else {
                printf("СЛОЙ 3B: FB_GET_INFO_V2 FAIL rc=%d status=0x%x\n", crc, cst);
            }

            /* --- B-метрика №2: FERMI_VASPACE_A — корень GMMU (GPU VA-пространство) --- */
            uint32_t hva=0, vst=0xffffffff;
            int vrc = nv_gsp_rm_vaspace_ctor(&ch, hcli, hdev, &hva, &vst);
            l3_vaspace_ok = (vrc == NV_GSP_RM_OK && vst == 0);
            printf("СЛОЙ 3B: FERMI_VASPACE_A rc=%d status=0x%x handle=0x%08x%s\n",
                   vrc, vst, hva, l3_vaspace_ok ? "" : "  (не OK)");

            /* --- ПРОХОД C: регистрация VRAM-диапазона (NV01_MEMORY_LIST_FBMEM) ---
               В GSP-модели VRAM'ом владеет гость: выбираем физ. диапазон из usable
               FB-региона (reserved=0, не protected) и регистрируем его memlist'ом. */
            uint64_t vsize = 0x100000; /* 1 МиБ = 256 страниц */
            uint64_t vphys = 0; int have_phys = 0;
            for (uint32_t i = 0; i < si.num_regions && !have_phys; i++) {
                if (si.regions[i].reserved || si.regions[i].prot) continue;
                uint64_t base = (si.regions[i].base + 0xfffff) & ~0xfffffull;   /* 1 МиБ-выравн. */
                uint64_t cand = base + 0x10000000ull;                           /* 256 МиБ вглубь */
                if (cand + vsize - 1 <= si.regions[i].limit) { vphys = cand; have_phys = 1; }
                else if (base + vsize - 1 <= si.regions[i].limit) { vphys = base; have_phys = 1; }
            }
            if (have_phys) {
                uint32_t hmem=0, mrres=0xffffffff;
                int mrc = nv_gsp_rm_vram_memlist(&ch, hcli, hdev, vphys, vsize, &hmem, &mrres);
                l3_vram_ok = (mrc == NV_GSP_RM_OK && mrres == 0);
                printf("СЛОЙ 3C: VRAM memlist phys=0x%llx size=0x%llx rc=%d rpc_result=0x%x handle=0x%08x\n",
                       (unsigned long long)vphys, (unsigned long long)vsize, mrc, mrres, hmem);

                /* --- ПРОХОД D: memlist VRAM → GPU VA через ПРЯМОЙ GMMU ---
                   RPC-путь (MAP_MEMORY_DMA fn=14) на железе = тупик (NV_ERR_INVALID_
                   FUNCTION, gsp-layer3-rpc.md §4D.1). Правильно: КЛИЕНТ сам строит
                   page-tables во VRAM (PRAMIN), а GSP получает физ-адреса корневых
                   уровней через RM_CONTROL COPY_SERVER_RESERVED_PDES (0x90f10106).
                   Эталон — nouveau r535_mmu_promote_vmm. */
                if (l3_vram_ok && l3_vaspace_ok) {
                    /* 5 page-tables во VRAM сразу за замапленным объектом (5×4К). */
                    uint64_t pt_base = (vphys + vsize + 0xfffull) & ~0xfffull;
                    nv_gmmu_tables t;
                    t.pd3_phys = pt_base + 0ull * 0x1000ull;   /* корень (PD3) */
                    t.pd2_phys = pt_base + 1ull * 0x1000ull;
                    t.pd1_phys = pt_base + 2ull * 0x1000ull;
                    t.pd0_phys = pt_base + 3ull * 0x1000ull;
                    t.spt_phys = pt_base + 4ull * 0x1000ull;   /* лист (SPT) */

                    /* Целевой GPU VA: выровнен на pageSize=0x20000000 (512 МиБ). */
                    uint64_t va = NV90F1_COPY_PDES_PAGESIZE_DEFAULT;   /* второй слот, ≠0 */
                    uint32_t npages = (uint32_t)(vsize >> 12);         /* 256 */

                    /* Построить иерархию PD3→…→SPT для [va, va+1МиБ) → phys vphys. */
                    uint64_t win = ~0ull;                              /* инвалидировать кэш окна */
                    int grc = nv_gmmu_map_range(io, &win, &t, va, vphys, npages);
                    printf("СЛОЙ 3D: GMMU build pt_base=0x%llx va=0x%llx npages=%u rc=%d\n",
                           (unsigned long long)pt_base, (unsigned long long)va, npages, grc);

                    if (grc == 0) {
                        /* Отдать GSP корневые уровни PD3/PD2/PD1 (сверху вниз). */
                        uint64_t pd_phys[3] = { t.pd3_phys, t.pd2_phys, t.pd1_phys };
                        uint64_t va_lo = va;
                        uint64_t va_hi = va + NV90F1_COPY_PDES_PAGESIZE_DEFAULT - 1;
                        uint32_t cpst = 0xffffffffu;
                        /* hSubDevice=0, subDeviceId=0 — как r535_mmu_promote_vmm (unicast,
                           subDevice 0); GSP резолвит субустройство по нашему клиенту. */
                        int cprc = nv_gsp_rm_vaspace_copy_pdes(&ch, hcli, hva, 0, 0,
                                                               va_lo, va_hi, pd_phys, 3, &cpst);
                        l3_map_ok = (cprc == NV_GSP_RM_OK && cpst == 0);
                        printf("СЛОЙ 3D: COPY_SERVER_RESERVED_PDES rc=%d status=0x%x%s\n",
                               cprc, cpst, l3_map_ok ? "" : "  (не OK)");

                        /* Bonus: read-back записанной листовой PTE через PRAMIN. */
                        uint64_t pte = nv_gmmu_read_pte(io, &win, t.spt_phys, va);
                        uint64_t want = nv_gmmu_make_pte_vram(vphys, 0, 0);
                        printf("СЛОЙ 3D: read-back PTE[va]=0x%016llx (ожид. 0x%016llx) valid=%d %s\n",
                               (unsigned long long)pte, (unsigned long long)want,
                               (int)(pte & NV_GMMU_PTE_VALID),
                               (pte == want) ? "MATCH" : "MISMATCH");

                        /* ============ СЛОЙ 4 A1+A2: канал GPFIFO (CE0) ============
                           A1: буферы во VRAM (instance/RAMFC, USERD, method-buffer) —
                           физ-адреса за page-tables; кольцо GPFIFO — в уже замапленном
                           VA-регионе (va→vphys, проход D), gpFifoOffset=va.
                           A2: channel_alloc(AMPERE_CHANNEL_GPFIFO_A) + BIND + SCHEDULE.
                           Порт r535_chan_ramfc_write. */
                        if (l3_map_ok && l4_ce_engtype >= 0) {
                            uint64_t buf_base = pt_base + 0x10000ull;      /* за 5 таблицами */
                            uint64_t inst_phys   = buf_base + 0x0000ull;   /* instance+RAMFC */
                            uint64_t userd_phys  = buf_base + 0x1000ull;
                            uint64_t mthd_phys   = buf_base + 0x2000ull;   /* method-buffer */
                            uint64_t inst_size = 0x1000, userd_size = 0x200, mthd_size = 0x5000;
                            /* userdMem.size=0x200 (один канал, gv100_chan_userd),
                               instance=0x1000 (gf100_chan_inst), mthdbuf≈0x5000 —
                               как r535_chan_ramfc_write; TODO: verify HW. */
                            /* Обнулить буферы во VRAM через PRAMIN (win из прохода D). */
                            nv_pramin_fill(io, &win, inst_phys,  (uint32_t)inst_size,  0);
                            nv_pramin_fill(io, &win, userd_phys, (uint32_t)userd_size, 0);
                            nv_pramin_fill(io, &win, mthd_phys,  (uint32_t)mthd_size,  0);
                            /* Кольцо GPFIFO — начало замапленного региона (phys vphys, VA va). */
                            uint32_t gpfifo_entries = 0x100;               /* 256 записей = 2 КиБ */
                            nv_pramin_fill(io, &win, vphys, gpfifo_entries * 8u, 0);

                            nv_gsp_chan_cfg cfg; memset(&cfg, 0, sizeof(cfg));
                            cfg.hClient = hcli; cfg.hDevice = hdev; cfg.hVASpace = hva;
                            cfg.chid = 0; cfg.engineType = (uint32_t)l4_ce_engtype;
                            cfg.gpfifo_va = va; cfg.gpfifo_entries = gpfifo_entries;
                            cfg.inst_phys = inst_phys;   cfg.inst_size = inst_size;
                            cfg.userd_phys = userd_phys; cfg.userd_size = userd_size;
                            cfg.ramfc_size = 0x200;
                            cfg.mthdbuf_phys = mthd_phys; cfg.mthdbuf_size = mthd_size;
                            cfg.mthdbuf_sysmem = 0; cfg.priv = 1;

                            uint32_t hchan = 0, chst = 0xffffffffu;
                            int chrc = nv_gsp_rm_channel_alloc(&ch, &cfg, &hchan, &chst);
                            l4_chan_ok = (chrc == NV_GSP_RM_OK && chst == 0);
                            printf("СЛОЙ 4 A2: channel_alloc rc=%d status=0x%x handle=0x%08x engineType=0x%x%s\n",
                                   chrc, chst, hchan, cfg.engineType, l4_chan_ok ? "" : "  (не OK)");

                            if (l4_chan_ok) {
                                uint32_t bst = 0xffffffffu;
                                int brc4 = nv_gsp_rm_channel_bind(&ch, hcli, hchan,
                                                                  cfg.engineType, &bst);
                                l4_bind_ok = (brc4 == NV_GSP_RM_OK && bst == 0);
                                printf("СЛОЙ 4 A2: BIND rc=%d status=0x%x%s\n",
                                       brc4, bst, l4_bind_ok ? "" : "  (не OK)");

                                uint32_t sst = 0xffffffffu;
                                int src4 = nv_gsp_rm_channel_schedule(&ch, hcli, hchan, 1, &sst);
                                l4_sched_ok = (src4 == NV_GSP_RM_OK && sst == 0);
                                printf("СЛОЙ 4 A2: GPFIFO_SCHEDULE(enable) rc=%d status=0x%x%s\n",
                                       src4, sst, l4_sched_ok ? "" : "  (не OK)");

                                /* --- ПРОХОД B: объект copy-engine (AMPERE_DMA_COPY_B) на канале ---
                                   NVB0B5_ALLOCATION_PARAMETERS{version=1, engineType=CE0}. */
                                if (l4_sched_ok) {
                                    uint32_t hce = NV_GSP_RM_CE_OBJ_HANDLE | 0u, cest = 0xffffffffu;
                                    int cerc = nv_gsp_rm_ce_obj_alloc(&ch, hcli, hchan, hce,
                                                                      AMPERE_DMA_COPY_B,
                                                                      cfg.engineType, &cest);
                                    l4_ce_obj_ok = (cerc == NV_GSP_RM_OK && cest == 0);
                                    printf("СЛОЙ 4 B: CE-объект (AMPERE_DMA_COPY_B) rc=%d status=0x%x handle=0x%08x%s\n",
                                           cerc, cest, hce, l4_ce_obj_ok ? "" : "  (не OK)");
                                }

                                /* ============ ПРОХОД C: исполнить команду GPU ============
                                   Host-релиз семафора через pushbuffer. Разметка в замапленном
                                   регионе (va→vphys): GPFIFO ring @+0, pushbuffer @+0x1000,
                                   семафор @+0x2000. Пишем PB/entry/GP_PUT через PRAMIN, звоним
                                   в doorbell, ждём семафор. Порт clc56f.h (NVC56F host sem). */
                                if (l4_sched_ok) {
                                    uint64_t pb_va   = va    + 0x1000ull, pb_phys  = vphys + 0x1000ull;
                                    uint64_t sem_va  = va    + 0x2000ull, sem_phys = vphys + 0x2000ull;
                                    uint32_t payload = 0xcafe0001u;

                                    nv_pramin_wr32(io, &win, sem_phys, 0u);          /* семафор=0 */
                                    uint32_t pb[8];
                                    uint32_t nd = nv_gsp_fifo_build_sem_release(pb, sem_va, payload);
                                    for (uint32_t i = 0; i < nd; i++)
                                        nv_pramin_wr32(io, &win, pb_phys + i * 4u, pb[i]);
                                    /* GPFIFO entry[0] → pushbuffer. */
                                    uint32_t e0 = 0, e1 = 0;
                                    nv_gsp_fifo_gpfifo_entry(pb_va, nd, &e0, &e1);
                                    nv_pramin_wr32(io, &win, vphys + 0u, e0);
                                    nv_pramin_wr32(io, &win, vphys + 4u, e1);
                                    /* USERD GP_PUT = 1 (одна запись доступна). */
                                    nv_pramin_wr32(io, &win, userd_phys + NV_USERD_GP_PUT_OFF, 1u);
                                    /* Doorbell: token = (runlistId<<16)|chid. */
                                    uint32_t token = (l4_ce_runlist << 16) | cfg.chid;
                                    io->wr(io->ctx, NV_VFN_DOORBELL_ADDR, token);
                                    /* Ждём релиз семафора (до 2с). */
                                    uint32_t got = 0;
                                    for (int i = 0; i < 2000; i++) {
                                        got = nv_pramin_rd32(io, &win, sem_phys);
                                        if (got == payload) { l4_exec_ok = 1; break; }
                                        bar_udelay(NULL, 1000);
                                    }
                                    printf("СЛОЙ 4 C: sem-release payload=0x%x got=0x%x doorbell(0x%x)=0x%x %s\n",
                                           payload, got, NV_VFN_DOORBELL_ADDR, token,
                                           l4_exec_ok ? "★ ИСПОЛНЕНО ★" : "(нет релиза)");
                                }
                            }
                        }
                    }
                }
            } else {
                printf("СЛОЙ 3C: не нашёл usable FB-региона под 1 МиБ\n");
            }
        }

        printf("СЛОЙ 3: итог — static_info=%s, RM-цепочка=%s, FB-control=%s, vaspace=%s, VRAM-alloc=%s, VRAM-map=%s\n",
               l3_static_ok?"OK":"нет", l3_chain_ok?"OK":"нет",
               l3_ctrl_ok?"OK":"нет", l3_vaspace_ok?"OK":"нет",
               l3_vram_ok?"OK":"нет", l3_map_ok?"OK":"нет");

        /* ===================== СЛОЙ 5 (A0): энумерация дисплея =====================
           NV04_DISPLAY_COMMON (0x0073) под device → контролы NV0073_*: число heads и
           маска поддерживаемых дисплеев. Порт r535_disp_oneinit. Headless (без монитора). */
        if (l3_chain_ok) {
            uint32_t hdisp = 0, dst = 0xffffffffu;
            int drc = nv_gsp_disp_common_alloc(&ch, hcli, hdev, &hdisp, &dst);
            printf("СЛОЙ 5 A0: NV04_DISPLAY_COMMON rc=%d status=0x%x handle=0x%08x%s\n",
                   drc, dst, hdisp, (drc==NV_GSP_RM_OK && dst==0) ? "" : "  (не OK)");
            if (drc == NV_GSP_RM_OK && dst == 0) {
                uint32_t nheads = 0, hst = 0xffffffffu;
                int hrc = nv_gsp_disp_get_num_heads(&ch, hcli, hdisp, &nheads, &hst);
                uint32_t dmask = 0, dddc = 0, sst = 0xffffffffu;
                int src = nv_gsp_disp_get_supported(&ch, hcli, hdisp, &dmask, &dddc, &sst);
                printf("СЛОЙ 5 A0: NUM_HEADS rc=%d status=0x%x heads=%u; GET_SUPPORTED rc=%d status=0x%x displayMask=0x%x DDC=0x%x\n",
                       hrc, hst, nheads, src, sst, dmask, dddc);
                l5_disp_ok = (hrc==NV_GSP_RM_OK && hst==0 && src==NV_GSP_RM_OK && sst==0 &&
                              nheads > 0 && dmask != 0);

                /* --- СЛОЙ 5 B: коннекторы (OR_GET_INFO) + connect-state + EDID ---
                   Перебираем displayId-биты из displayMask: тип/протокол выхода (SOR
                   DP/TMDS), подключён ли монитор, EDID (если подключён). Headless:
                   OR-инфо статично; connect/EDID покажут монитор при наличии. */
                if (l5_disp_ok) {
                    uint32_t conn = 0, cst = 0xffffffffu;
                    nv_gsp_disp_get_connect_state(&ch, hcli, hdisp, dmask, &conn, &cst);
                    printf("СЛОЙ 5 B: CONNECT_STATE status=0x%x connected=0x%x\n", cst, conn);
                    /* Захват для modeset (5C.4d): предпочитаем TMDS/HDMI (без DP link training). */
                    static uint8_t md_edid[256]; int md_edid_ok = 0;
                    uint32_t md_did = 0, md_proto = 0, md_sor = ~0u;
                    for (uint32_t b = 0; b < 32; b++) {
                        uint32_t did = dmask & (1u << b);
                        if (!did) continue;
                        uint32_t ty=0, pr=0, ix=0, lo=0, ost=0xffffffffu;
                        int orc = nv_gsp_disp_or_get_info(&ch, hcli, hdisp, did, &ty, &pr, &ix, &lo, &ost);
                        const char *tn = (ty==NV0073_OR_TYPE_SOR) ? "SOR" :
                                         (ty==NV0073_OR_TYPE_DAC) ? "DAC" :
                                         (ty==NV0073_OR_TYPE_NONE) ? "NONE" : "?";
                        const char *pn = (pr==NV0073_OR_PROTOCOL_SOR_DP_A) ? "DP_A" :
                                         (pr==NV0073_OR_PROTOCOL_SOR_DP_B) ? "DP_B" :
                                         (pr==NV0073_OR_PROTOCOL_SOR_SINGLE_TMDS_A) ? "TMDS_A" :
                                         (pr==NV0073_OR_PROTOCOL_SOR_SINGLE_TMDS_B) ? "TMDS_B" :
                                         (pr==NV0073_OR_PROTOCOL_SOR_DUAL_TMDS) ? "DUAL_TMDS" : "?";
                        int plugged = (conn & did) ? 1 : 0;
                        printf("  disp 0x%04x: OR rc=%d status=0x%x type=%s proto=%s idx=%u loc=%u  %s\n",
                               did, orc, ost, tn, pn, ix, lo, plugged ? "ПОДКЛЮЧЁН" : "нет");
                        if (plugged) {
                            static uint8_t edid[256]; uint32_t esz=0, est=0xffffffffu;
                            int erc = nv_gsp_disp_get_edid(&ch, hcli, hdisp, did, edid, sizeof(edid), &esz, &est);
                            printf("    EDID rc=%d status=0x%x size=%u", erc, est, esz);
                            if (erc==NV_GSP_RM_OK && est==0 && esz>=8)
                                printf(" magic=%02x%02x%02x%02x%02x%02x%02x%02x",
                                       edid[0],edid[1],edid[2],edid[3],edid[4],edid[5],edid[6],edid[7]);
                            printf("\n");
                            /* Кандидат на modeset: первый TMDS-выход (или первый вообще, если TMDS нет). */
                            int is_tmds = (pr==NV0073_OR_PROTOCOL_SOR_SINGLE_TMDS_A ||
                                           pr==NV0073_OR_PROTOCOL_SOR_SINGLE_TMDS_B ||
                                           pr==NV0073_OR_PROTOCOL_SOR_DUAL_TMDS);
                            if (erc==NV_GSP_RM_OK && est==0 && esz>=128 &&
                                (!md_edid_ok || (is_tmds && md_proto>=8 /*был DP*/))) {
                                memcpy(md_edid, edid, sizeof(md_edid));
                                md_edid_ok = 1; md_did = did; md_proto = pr;
                            }
                        }
                    }

                    /* --- СЛОЙ 5 C.1: RAMIN дисплея + display root класс ---
                       Обнулить 64 КиБ VRAM под RAMIN (PRAMIN) → WRITE_INST_MEM на
                       внутренний subdevice GSP → alloc AD102_DISP. Порт r535_disp_init. */
                    uint64_t disp_inst = 0x13400000ull;        /* 64К-выровнено, в usable FB */
                    uint64_t win = ~0ull;
                    nv_pramin_fill(io, &win, disp_inst, NV_DISP_INST_SIZE, 0u);
                    uint32_t wist = 0xffffffffu;
                    int wirc = nv_gsp_disp_write_inst_mem(&ch, si.h_client, si.h_subdevice,
                                                          disp_inst, NV_DISP_INST_SIZE, &wist);
                    printf("СЛОЙ 5 C.1: WRITE_INST_MEM rc=%d status=0x%x inst=0x%llx\n",
                           wirc, wist, (unsigned long long)disp_inst);
                    uint32_t hroot = 0, rst = 0xffffffffu;
                    int rrc = nv_gsp_disp_root_alloc(&ch, hcli, hdev, AD102_DISP, &hroot, &rst);
                    printf("СЛОЙ 5 C.1: AD102_DISP root rc=%d status=0x%x handle=0x%08x%s\n",
                           rrc, rst, hroot,
                           (rrc==NV_GSP_RM_OK && rst==0) ? "  ★ DISPLAY ROOT ★" : "  (не OK)");

                    /* --- СЛОЙ 5 C.2: core-channel дисплея ---
                       Пушбуфер во VRAM (обнулить, ≤4К) → DISPLAY_CHANNEL_PUSHBUFFER на
                       внутр. subdevice GSP → alloc AD102_DISP_CORE_CHANNEL_DMA под root.
                       Порт r535_chan_push + r535_dmac_init. */
                    if (rrc == NV_GSP_RM_OK && rst == 0) {
                        uint64_t core_pb = 0x13410000ull;   /* пушбуфер core-channel во VRAM */
                        nv_pramin_fill(io, &win, core_pb, NV_DISP_PB_SIZE, 0u);
                        uint32_t pst = 0xffffffffu;
                        int prc = nv_gsp_disp_channel_pushbuffer(&ch, si.h_client, si.h_subdevice,
                                                                 AD102_DISP_CORE_CHANNEL_DMA, 0,
                                                                 core_pb, NV_DISP_PB_SIZE - 1, &pst);
                        printf("СЛОЙ 5 C.2: DISPLAY_CHANNEL_PUSHBUFFER rc=%d status=0x%x pb=0x%llx\n",
                               prc, pst, (unsigned long long)core_pb);
                        uint32_t hcore = 0, cst = 0xffffffffu;
                        int crc = nv_gsp_disp_core_channel_alloc(&ch, hcli, hroot,
                                                                 AD102_DISP_CORE_CHANNEL_DMA, 0,
                                                                 &hcore, &cst);
                        printf("СЛОЙ 5 C.2: CORE_CHANNEL_DMA rc=%d status=0x%x handle=0x%08x%s\n",
                               crc, cst, hcore,
                               (crc==NV_GSP_RM_OK && cst==0) ? "  ★ CORE CHANNEL ★" : "  (не OK)");

                        /* --- СЛОЙ 5 C.3: назначить SOR подключённым дисплеям ---
                           DFP_ASSIGN_SOR до modeset (и до DP link training). Порт
                           r535_outp_acquire. Перебираем подключённые (connected). */
                        if (crc == NV_GSP_RM_OK && cst == 0) {
                            for (uint32_t b = 0; b < 32; b++) {
                                uint32_t did = conn & (1u << b);
                                if (!did) continue;
                                uint32_t sor = ~0u, ast = 0xffffffffu;
                                int arc = nv_gsp_disp_assign_sor(&ch, hcli, hdisp, did, &sor, &ast);
                                printf("СЛОЙ 5 C.3: ASSIGN_SOR disp=0x%04x rc=%d status=0x%x SOR=%d%s\n",
                                       did, arc, ast, (int)sor,
                                       (arc==NV_GSP_RM_OK && ast==0 && sor!=~0u) ? "  ★ SOR ★" : "  (не OK)");
                                if (did == md_did && arc==NV_GSP_RM_OK && ast==0 && sor!=~0u)
                                    md_sor = sor;   /* SOR для нашего modeset-выхода */
                            }

                            /* --- СЛОЙ 5 C.4a: window channel (GA102_DISP_WINDOW_CHANNEL_DMA) ---
                               Тем же путём, что core (r535_dmac_init): пушбуфер + PUSHBUFFER
                               control + alloc под display root. Нужен для surface/scanout. */
                            uint64_t win_pb = 0x13412000ull;   /* пушбуфер window-channel */
                            nv_pramin_fill(io, &win, win_pb, NV_DISP_PB_SIZE, 0u);
                            uint32_t wpst = 0xffffffffu;
                            int wprc = nv_gsp_disp_channel_pushbuffer(&ch, si.h_client, si.h_subdevice,
                                                                      GA102_DISP_WINDOW_CHANNEL_DMA, 0,
                                                                      win_pb, NV_DISP_PB_SIZE - 1, &wpst);
                            printf("СЛОЙ 5 C.4a: WINDOW PUSHBUFFER rc=%d status=0x%x pb=0x%llx\n",
                                   wprc, wpst, (unsigned long long)win_pb);
                            uint32_t hwin = 0, wst = 0xffffffffu;
                            int wrc = nv_gsp_disp_core_channel_alloc(&ch, hcli, hroot,
                                                                     GA102_DISP_WINDOW_CHANNEL_DMA, 0,
                                                                     &hwin, &wst);
                            printf("СЛОЙ 5 C.4a: WINDOW_CHANNEL_DMA rc=%d status=0x%x handle=0x%08x%s\n",
                                   wrc, wst, hwin,
                                   (wrc==NV_GSP_RM_OK && wst==0) ? "  ★ WINDOW CHANNEL ★" : "  (не OK)");

                            /* --- СЛОЙ 5 C.4b: framebuffer во VRAM + тест-паттерн ---
                               Выделяем FB во VRAM (1920x1080 BGRA), заливаем 3 полосы
                               R/G/B через PRAMIN, читаем обратно. Безопасно (вывод не
                               трогаем). FB — под surface для scanout (5C.4c-d). */
                            {
                                uint64_t fb_phys = 0x14000000ull;   /* FB во VRAM (usable) */
                                uint32_t fb_w = 1920, fb_h = 1080;
                                uint32_t pitch = fb_w * 4u;         /* BGRA8888 */
                                uint64_t fb_size = (uint64_t)pitch * fb_h;
                                uint32_t band = fb_h / 3u;
                                uint64_t band_bytes = (uint64_t)pitch * band;
                                /* BGRA (little-endian u32 0xAARRGGBB): красный/зелёный/синий */
                                nv_pramin_fill(io, &win, fb_phys + 0*band_bytes, (uint32_t)band_bytes, 0x00ff0000u);
                                nv_pramin_fill(io, &win, fb_phys + 1*band_bytes, (uint32_t)band_bytes, 0x0000ff00u);
                                nv_pramin_fill(io, &win, fb_phys + 2*band_bytes,
                                               (uint32_t)(fb_size - 2*band_bytes), 0x000000ffu);
                                uint32_t p0 = nv_pramin_rd32(io, &win, fb_phys + 0*band_bytes);
                                uint32_t p1 = nv_pramin_rd32(io, &win, fb_phys + 1*band_bytes);
                                uint32_t p2 = nv_pramin_rd32(io, &win, fb_phys + 2*band_bytes);
                                int fb_ok = (p0==0x00ff0000u && p1==0x0000ff00u && p2==0x000000ffu);
                                printf("СЛОЙ 5 C.4b: FB=0x%llx %ux%u pitch=%u size=0x%llx read-back R=0x%08x G=0x%08x B=0x%08x %s\n",
                                       (unsigned long long)fb_phys, fb_w, fb_h, pitch,
                                       (unsigned long long)fb_size, p0, p1, p2,
                                       fb_ok ? "  ★ ПАТТЕРН В VRAM ★" : "  (mismatch)");
                            }

                            /* --- СЛОЙ 5 C.4d: сабмит core-channel modeset (raster+pclk+SOR) ---
                               Программируем тайминг дисплея и SOR через core-channel: пишем
                               поток методов в пушбуфер (PRAMIN) → бампаем PUT в user-регионе
                               BAR0 → ждём GET==PUT. БЕЗ surface/ctx-dma (notifier выкл,
                               interlock окон=0) — проверяем сам механизм сабмита + программу
                               таймингов. Пиксели (window+ctx-dma) — 5C.4e. Порт corec37d_*+
                               headc37d_mode. PUT/GET: NV507C @user+0x0/+0x4, PTR[11:2]=байт-offset. */
                            if (wrc==NV_GSP_RM_OK && wst==0 && md_edid_ok && md_sor != ~0u) {
                                nv_edid_timing mt;
                                int mprc = nv_edid_parse_dtd(md_edid, sizeof(md_edid), &mt);
                                printf("СЛОЙ 5 C.4d: EDID DTD parse rc=%d %ux%u@%uкГц sync h%s v%s did=0x%04x proto=%u SOR=%d\n",
                                       mprc, mt.hact, mt.vact, mt.pclk_khz,
                                       mt.hsync_pos?"+":"-", mt.vsync_pos?"+":"-",
                                       md_did, md_proto, (int)md_sor);
                                if (mprc == 0) {
                                    uint32_t head = 0;
                                    uint32_t wnd = 0;                 /* window-инстанс (owner head0) */
                                    uint32_t wnd_bit = 1u << wnd;     /* бит окна в SET_WINDOW_INTERLOCK_FLAGS */
                                    uint64_t fb2 = 0x14000000ull;
                                    uint32_t w = mt.hact, h = mt.vact, pit = w * 4u;

                                    /* 1) FB под нативное разрешение (из EDID) — R/G/B полосы (X8R8G8B8). */
                                    uint64_t fbsz = (uint64_t)pit * h;
                                    uint32_t bnd = h / 3u; uint64_t bb = (uint64_t)pit * bnd;
                                    nv_pramin_fill(io, &win, fb2 + 0*bb, (uint32_t)bb, 0x00ff0000u);
                                    nv_pramin_fill(io, &win, fb2 + 1*bb, (uint32_t)bb, 0x0000ff00u);
                                    nv_pramin_fill(io, &win, fb2 + 2*bb, (uint32_t)(fbsz - 2*bb), 0x000000ffu);

                                    /* 2) ctx-dma NV_DMA_IN_MEMORY (весь VRAM, RDWR) — 24б дескриптор в
                                       inst-mem дисплея @disp_inst+0x1000 (после RAMHT). */
                                    uint32_t desc_off = 0x1000u;
                                    uint64_t desc_phys = disp_inst + desc_off;
                                    uint8_t desc[NV_CTXDMA_DESC_SIZE];
                                    nv_gsp_disp_build_ctxdma_desc(desc, 0, 0x2ffffffffull);
                                    for (unsigned i = 0; i < NV_CTXDMA_DESC_SIZE; i += 4)
                                        nv_pramin_wr32(io, &win, desc_phys + i,
                                                       (uint32_t)desc[i] | ((uint32_t)desc[i+1]<<8) |
                                                       ((uint32_t)desc[i+2]<<16) | ((uint32_t)desc[i+3]<<24));
                                    /* 3) RAMHT-запись {handle,context} для window-канала (chid=window0). */
                                    uint32_t rslot = 0, rctx = 0;
                                    nv_gsp_disp_ramht_entry(NV_DISP_CHID_WINDOW(head), NV_DISP_HANDLE_VRAM,
                                                            hcli, desc_off, &rslot, &rctx);
                                    uint64_t ent_phys = disp_inst + (uint64_t)rslot * NV_DISP_RAMHT_ENTRY;
                                    nv_pramin_wr32(io, &win, ent_phys + 0, NV_DISP_HANDLE_VRAM);
                                    nv_pramin_wr32(io, &win, ent_phys + 4, rctx);
                                    printf("СЛОЙ 5 C.4e: ctx-dma desc@0x%llx flags0=0x%x; RAMHT slot=%u ent@0x%llx ctx=0x%x (rb h=0x%x c=0x%x)\n",
                                           (unsigned long long)desc_phys,
                                           (uint32_t)desc[0]|((uint32_t)desc[1]<<8)|((uint32_t)desc[2]<<16)|((uint32_t)desc[3]<<24),
                                           rslot, (unsigned long long)ent_phys, rctx,
                                           nv_pramin_rd32(io,&win,ent_phys+0), nv_pramin_rd32(io,&win,ent_phys+4));

                                    uint32_t core_user = NVC77D_CORE_USER_BASE;                 /* 0x680000 */
                                    uint32_t win_user  = NVC37E_WINDOW_USER_BASE + head*0x1000u;/* 0x690000 */
                                    static uint8_t cs[1024]; uint32_t coff = 0;

                                    /* --- ФАЗА 1: core init + update (назначить окно голове, assign_windows).
                                       В nouveau corec37d_init — ОТДЕЛЬНЫЙ PUSH_KICK ДО modeset. interlock=0. */
                                    nv_gsp_disp_build_core_init(cs, &coff, 0u /*notifier disabled*/);
                                    nv_gsp_disp_build_core_update(cs, &coff, 0u);
                                    for (uint32_t i = 0; i < coff; i += 4)
                                        nv_pramin_wr32(io, &win, core_pb + i,
                                                       (uint32_t)cs[i] | ((uint32_t)cs[i+1]<<8) |
                                                       ((uint32_t)cs[i+2]<<16) | ((uint32_t)cs[i+3]<<24));
                                    io->wr(io->ctx, core_user + 0x0, coff);
                                    uint32_t cget = 0; int p1done = 0;
                                    for (int it = 0; it < 500; it++) {
                                        cget = io->rd(io->ctx, core_user + 0x4);
                                        if ((cget & 0xffcu) == (coff & 0xffcu)) { p1done = 1; break; }
                                        bar_udelay(NULL, 1000);
                                    }
                                    printf("СЛОЙ 5 C.4d-ф1: core init+update %u байт (%u мет) PUT=0x%x GET=0x%x %s\n",
                                           coff, coff/8u, coff, cget, p1done ? "GET==PUT (окна назначены)" : "GET!=PUT");
                                    l5_modeset_ok = p1done;

                                    /* --- ФАЗА 2: modeset (core) + image (window), UPDATE'ы СЦЕПЛЕНЫ (interlock).
                                       Дописываем в те же пушбуферы (core с offset coff). core UPDATE ждёт окно0,
                                       window UPDATE ждёт core → защёлкиваются вместе (атомарный modeset+flip). */
                                    uint32_t c2 = coff;               /* старт фазы 2 в core-пушбуфере */
                                    nv_gsp_disp_build_core_modeset(cs, &coff, &mt, head, md_sor, md_proto);
                                    nv_gsp_disp_build_core_update(cs, &coff, wnd_bit /*интерлок с окном0*/);
                                    for (uint32_t i = c2; i < coff; i += 4)
                                        nv_pramin_wr32(io, &win, core_pb + i,
                                                       (uint32_t)cs[i] | ((uint32_t)cs[i+1]<<8) |
                                                       ((uint32_t)cs[i+2]<<16) | ((uint32_t)cs[i+3]<<24));
                                    static uint8_t ws[256]; uint32_t woff = 0;
                                    nv_gsp_disp_build_window_image(ws, &woff, NVC37E_PARAMS_FORMAT_X8R8G8B8,
                                                                   w, h, pit, fb2, NV_DISP_HANDLE_VRAM);
                                    nv_gsp_disp_build_window_update(ws, &woff, 1 /*интерлок с core*/);
                                    for (uint32_t i = 0; i < woff; i += 4)
                                        nv_pramin_wr32(io, &win, win_pb + i,
                                                       (uint32_t)ws[i] | ((uint32_t)ws[i+1]<<8) |
                                                       ((uint32_t)ws[i+2]<<16) | ((uint32_t)ws[i+3]<<24));
                                    /* Бампнуть window PUT (встанет на UPDATE, ждёт core), затем core PUT. */
                                    io->wr(io->ctx, win_user  + 0x0, woff);
                                    io->wr(io->ctx, core_user + 0x0, coff);
                                    uint32_t wget = 0; int cdone = 0, wdone = 0;
                                    for (int it = 0; it < 1000; it++) {
                                        cget = io->rd(io->ctx, core_user + 0x4);
                                        wget = io->rd(io->ctx, win_user  + 0x4);
                                        cdone = ((cget & 0xffcu) == (coff & 0xffcu));
                                        wdone = ((wget & 0xffcu) == (woff & 0xffcu));
                                        if (cdone && wdone) break;
                                        bar_udelay(NULL, 1000);
                                    }
                                    printf("СЛОЙ 5 C.4e-ф2: core modeset PUT=0x%x GET=0x%x %s; window %ux%u pit=%u поток=%u байт PUT=0x%x GET=0x%x %s\n",
                                           coff, cget, cdone ? "GET==PUT" : "GET!=PUT",
                                           w, h, pit, woff, woff, wget, wdone ? "GET==PUT" : "GET!=PUT");
                                    if (cdone && wdone)
                                        printf("СЛОЙ 5 C.4e: ★ INTERLOCKED MODESET+FLIP (оба GET==PUT) — ПИКСЕЛИ НА МОНИТОРЕ ★\n");
                                    l5_scanout_ok = (cdone && wdone);

                                    /* Дать монитору просинхронизироваться + показать кадр (видно на HDMI). */
                                    bar_udelay(NULL, 5000000);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

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
        const char *files[3] = {"/tmp/gsp-loginit.bin","/tmp/gsp-logintr.bin","/tmp/gsp-logrm.bin"};
        uint8_t *logs[3] = {loginit_va, logintr_va, logrm_va};
        uint64_t put0[3];
        for (int k = 0; k < 3; k++) put0[k] = *(volatile uint64_t*)logs[k];
        bar_udelay(NULL, 2000000); /* 2с: растёт ли лог? (фолт-луп vs заморозка) */
        for (int k = 0; k < 3; k++) {
            uint64_t put = *(volatile uint64_t*)logs[k]; /* put-указатель @0 */
            printf("ДИАГ: %-8s put=0x%llx (за 2с Δ=%lld)\n", names[k],
                   (unsigned long long)put, (long long)(put - put0[k]));
            FILE *df = fopen(files[k], "wb");
            if (df) { fwrite(logs[k], 1, NV_GSP_LIBOS_LOG_SIZE, df); fclose(df); }
        }
        /* Дамп msgq-региона целиком (вдруг GSP что-то записал не туда). */
        FILE *mf = fopen("/tmp/gsp-shm.bin","wb");
        if (mf){ fwrite(shm_va,1,shm_lay.total_size,mf); fclose(mf); }
        printf("ДИАГ: дампы логов и shm → /tmp/gsp-*.bin\n");
    }

    int rpc_ok = (got && sig==NV_GSP_RPC_SIGNATURE);
    if (brc==NV_OK && mb0==0 && active && rpc_ok){
        printf("\n*** GSP-RM ОТВЕТИЛ ПО RPC (function=0x%08x) — СЛОЙ 2 ЗАВЕРШЁН ***\n",fn);
        if (l3_static_ok && l3_chain_ok)
            printf("*** СЛОЙ 3 (проход A): двусторонний RPC + RM client/device/subdevice — OK ***\n");
        else if (l3_static_ok)
            printf("*** СЛОЙ 3: GET_GSP_STATIC_INFO OK; RM-цепочка НЕ завершена ***\n");
        else
            printf("*** СЛОЙ 3: двусторонний RPC пока не подтверждён ***\n");
        if (l3_ctrl_ok || l3_vaspace_ok)
            printf("*** СЛОЙ 3 (проход B): память через RPC — FB-control=%s, VA-пространство(GMMU)=%s ***\n",
                   l3_ctrl_ok?"OK":"нет", l3_vaspace_ok?"OK":"нет");
        if (l3_vram_ok)
            printf("*** СЛОЙ 3 (проход C): регистрация VRAM-объекта (NV01_MEMORY_LIST_FBMEM) — OK ***\n");
        if (l3_map_ok)
            printf("*** СЛОЙ 3 (проход D): прямой GMMU — page-tables во VRAM + COPY_SERVER_RESERVED_PDES (GSP прошил PDB) ***\n");
        if (l4_devinfo_ok)
            printf("*** СЛОЙ 4 (проход A0): FIFO device-info прочитан — движки GPU перечислены%s ***\n",
                   (l4_ce_engtype >= 0) ? ", CE0 найден" : "");
        if (l5_disp_ok)
            printf("*** СЛОЙ 5 (A0): дисплейный движок перечислен — NV04_DISPLAY_COMMON + heads + displayMask ***\n");
        if (l5_modeset_ok)
            printf("*** СЛОЙ 5 (C.4d): core-channel modeset САБМИТ проглочен (GET==PUT) — тайминг+SOR запрограммированы ***\n");
        if (l5_scanout_ok)
            printf("*** СЛОЙ 5 (C.4e): window flip САБМИТ проглочен (GET==PUT) — surface на scanout, ПИКСЕЛИ на мониторе ***\n");
        if (l4_ce_obj_ok)
            printf("*** СЛОЙ 4 (проход B): объект copy-engine (AMPERE_DMA_COPY_B) на канале — OK ***\n");
        if (l4_exec_ok)
            printf("*** СЛОЙ 4 (проход C): pushbuffer исполнен — host-семафор released ПЕРВАЯ КОМАНДА GPU ***\n");
        if (l4_sched_ok)
            printf("*** СЛОЙ 4 (проход A): канал GPFIFO создан+bind+schedule (CE0) — ПЕРВЫЙ КАНАЛ НА ЖЕЛЕЗЕ ***\n");
        else if (l4_chan_ok)
            printf("*** СЛОЙ 4 (проход A): channel_alloc OK; bind=%s schedule=%s ***\n",
                   l4_bind_ok?"OK":"нет", l4_sched_ok?"OK":"нет");
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

    /* Заглушка на низкие IOVA [0..16МиБ]: под нативным nouveau стоит iommu=pt (passthrough),
       и GSP-RM безвредно читает нулевые/низкие адреса; под VFIO (строгий IOMMU) это DMAR-фолт.
       Маппим зануленный регион, чтобы такие чтения не падали. */
    void *zbuf=mmap(NULL,0x1000000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if (zbuf!=MAP_FAILED){ memset(zbuf,0,0x1000000);
        if (vfio_map(&v,zbuf,0x0,0x1000000)==0) printf("Заглушка IOVA[0..0x1000000] смаплена\n");
        else printf("WARN: не удалось замапить IOVA 0 (зарезервирован?)\n"); }
    struct arena ar={.va=abuf,.iova=ARENA_IOVA,.off=0,.cap=ARENA_SIZE};
    nv_mmio_t io={.ctx=(void*)v.bar0,.rd=bar_rd,.wr=bar_wr,.udelay=bar_udelay};
    int rc=run(&io,&ar,bdf);
    struct vfio_iommu_type1_dma_unmap u={.argsz=sizeof(u),.iova=ARENA_IOVA,.size=ARENA_SIZE};
    ioctl(v.container,VFIO_IOMMU_UNMAP_DMA,&u); munmap(abuf,ARENA_SIZE); vfio_close(&v);
    printf("\n=== РЕЗУЛЬТАТ: %s ===\n", rc==0?"OK (GSP-RM загружен, RISC-V active)":"FAIL (см. лог)");
    return rc==0?0:1;
}
