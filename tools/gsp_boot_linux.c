/*
 * gsp_boot_linux.c — Linux-ПЛАТФОРМА для переносимого оркестратора GSP-RM.
 *
 * Вся логика bring-up (слои 2–5) вынесена в driver/gsp/gsp_bringup.c и общая с
 * macOS-kext. Здесь остаётся только Linux-специфика:
 *   - VFIO: открыть устройство, замапить BAR0, настроить IOMMU-арену;
 *   - DMA-арена через mmap(MAP_ANONYMOUS) + VFIO_IOMMU_MAP_DMA (IOVA=phys для GSP);
 *   - реализации колбэков nv_mmio_t (bar_rd/wr/udelay + log=printf) и
 *     nv_gsp_debug_t (дамп логов/shm в /tmp/*.bin);
 *   - чтение физ. BAR/PCI-id из sysfs, авто-детект NVIDIA.
 * Затем — один вызов nv_gsp_bringup(io, arena, pci, dbg).
 *
 * Сборка: make gsp-boot-linux ; прогон без ребута: tools/run-gsp-boot-detached.sh
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
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
#include "../driver/gsp/nv_dma.h"
#include "../driver/gsp/gsp_bringup.h"

#define ARENA_IOVA 0x10000000ULL
#define ARENA_SIZE NV_DMA_ARENA_SIZE   /* 64 МиБ под всё (fwimage ~36 МиБ) */

/* ===================== колбэки nv_mmio_t ===================== */
static uint32_t bar_rd(void *ctx, uint32_t off)
{ return *(volatile uint32_t *)((volatile uint8_t *)ctx + off); }
static void bar_wr(void *ctx, uint32_t off, uint32_t val)
{ *(volatile uint32_t *)((volatile uint8_t *)ctx + off) = val; }
static void bar_udelay(void *ctx, uint32_t usec)
{ (void)ctx; struct timespec ts = { usec/1000000u, (long)(usec%1000000u)*1000L }; nanosleep(&ts, NULL); }
static void bar_log(void *ctx, const char *fmt, ...)
{ (void)ctx; va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }

/* Дамп libos-логов и shm в /tmp/*.bin (диагностика стенда). */
static void dbg_dump(void *ctx, const char *name, const uint8_t *data, uint32_t len)
{
    (void)ctx;
    char path[64];
    snprintf(path, sizeof(path), "/tmp/gsp-%s.bin", name);
    for (char *p = path; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32; /* нижний регистр */
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
}

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

/* Прочитать физ. BAR0/1/3 и PCI BDF-id из sysfs (для GspSystemInfo). */
static int read_pci_bars(const char *bdf, nv_gsp_pci_info_t *pci)
{
    char p[300]; snprintf(p,sizeof(p),"/sys/bus/pci/devices/%s/resource",bdf);
    FILE *f=fopen(p,"r"); if(!f) return -1;
    uint64_t s[6]={0};
    for(int i=0;i<6;i++){ unsigned long long a=0,e=0,fl=0;
        if(fscanf(f,"%llx %llx %llx",&a,&e,&fl)!=3) break; s[i]=a; }
    fclose(f);
    pci->bar0=s[0]; pci->bar1=s[1]; pci->bar3=s[3];
    unsigned dom=0,bus=0,dev=0,fn=0;
    if(sscanf(bdf,"%x:%x:%x.%x",&dom,&bus,&dev,&fn)==4)
        pci->devid=((uint64_t)bus<<8)|(uint64_t)(((dev&0x1f)<<3)|(fn&7));
    else pci->devid=0x100;
    return 0;
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

    nv_dma_arena_t ar; nv_dma_arena_init(&ar,(uint8_t*)abuf,ARENA_IOVA,ARENA_SIZE);
    nv_mmio_t io={.ctx=(void*)v.bar0,.rd=bar_rd,.wr=bar_wr,.udelay=bar_udelay,.log=bar_log};
    nv_gsp_pci_info_t pci={0};
    if (read_pci_bars(bdf,&pci)!=0) fprintf(stderr,"WARN: не прочитал BAR из sysfs — sysinfo с нулями\n");
    nv_gsp_debug_t dbg={.ctx=NULL,.dump=dbg_dump};

    /* Linux-стенд — диагностический прогон: dbg!=NULL включает дампы и длинные
       паузы «разглядеть монитор». scan не нужен (стенд не публикует апертуру),
       провайдер FB не нужен (FB берётся во VRAM и заливается через PRAMIN). */
    int rc=nv_gsp_bringup(&io,&ar,&pci,&dbg,NULL,NULL,NULL);

    struct vfio_iommu_type1_dma_unmap u={.argsz=sizeof(u),.iova=ARENA_IOVA,.size=ARENA_SIZE};
    ioctl(v.container,VFIO_IOMMU_UNMAP_DMA,&u); munmap(abuf,ARENA_SIZE); vfio_close(&v);
    printf("\n=== РЕЗУЛЬТАТ: %s ===\n", rc==0?"OK (GSP-RM загружен, RISC-V active)":"FAIL (см. лог)");
    return rc==0?0:1;
}
