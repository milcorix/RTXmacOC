# PORTING-MAP.md — соответствие наш код ↔ исходники

> Чтобы реализация оставалась верной, здесь зафиксировано, **из какого
> upstream-файла портирован каждый наш модуль**, какие структуры/последовательности
> взяты и проверены. Перед правкой/добавлением кода слоя 2 — сверяйся отсюда.
>
> Upstream (всё публичное):
> - **OGK** = NVIDIA/open-gpu-kernel-modules (GitHub)
> - **nova** = ядро Linux, `drivers/gpu/nova-core/` (Rust, чистый разбор)
> - **nouveau** = ядро Linux, `drivers/gpu/drm/nouveau/` (открытая реализация GSP-RM)
>
> Содержимое источников пересказано/сведено для соответствия лицензии.

---

## Слой 1

| Наш код | Upstream | Что взято | Сверено |
|---|---|---|---|
| `ada_regs.h` `NV_PMC_BOOT_0` декод | nova `regs.rs` (`NV_PMC_BOOT_0`, `NV_PMC_BOOT_42`) | поля arch[28:24]/impl[23:20], chipset=(raw>>20)&0x1ff | 🟢 HW: `0x194000A1`→AD104 (RW-Everything) |
| `driver/RTXProbe*` матчинг | IOKit/PCIDriverKit (Apple) | `IOPCIPrimaryMatch 0x278310de`, маппинг BAR0 | ✅ собирается в CI |

## Слой 2 — Шаг 1 (VBIOS/FWSEC), `tools/vbios_dump.c`

| Наш код | Upstream (nova `vbios.rs`/`firmware.rs`) | Структура/смещения |
|---|---|---|
| PciRomHeader | `PciRomHeader` | sig u16@0 (`0xAA55`), pci_data_ptr u16@0x18 |
| PCIR | `PcirStruct` | sig@0, vendor@4, device@6, len u16@0x0A, image_len@0x10, code_type@0x14, last@0x15 |
| NPDE | `NpdeStruct` | @ `(pcir+len+0xF)&~0xF`; sig "NPDE", subimage_len@0x08, last@0x0A |
| BIT header | `BitHeader` | id `0xB8FF`, sig "BIT\0", header_size@8, token_size@9, token_entries@10 |
| BIT token | `BitToken` | id@0, data_ver@1, data_size@2, data_offset@4; FALCON_DATA id=`0x70` |
| falcon_data_off | `falcon_data_offset()` | `ptr - sizeof(PciAt image)` |
| PmuLookupTable | `PmuLookupTableHeader`/`Entry` | hdr: ver,header_len,entry_len,entry_count (по 1б); entry(packed): app_id@0,target@1,data u32@2; FWSEC PROD=`0x85` |
| falcon_ucode_off | `FwSecBiosImage::new` | `entry.data - sizeof(PciAt image)` |
| FalconUCodeDescV3 (44б) | `firmware.rs FalconUCodeDescV3` | hdr@0, pkc_data_offset@8, interface_offset@12, imem_phys@16, **imem_load@20**, imem_virt@24, dmem_phys@28, **dmem_load@32**, engine_id_mask u16@36, ucode_id@38, signature_count@39, signature_versions u16@40 |
| FalconUCodeDescV2 (60б) | `firmware.rs FalconUCodeDescV2` | imem_load@24, dmem_load@48 |
| desc.size() | `FalconUCodeDescriptor::size` | `(hdr>>16)&0xffff` |
| Bcrt30Rsa3kSignature | `fwsec.rs` | 384 байта |

## Слой 2 — Шаг 2 (Falcon), `falcon_regs.h` + `driver/gsp/falcon.{h,c}`

| Наш код | Upstream | Что взято | Сверено |
|---|---|---|---|
| `falcon_regs.h` PFALCON/PFALCON2 | nova `regs.rs` | смещения IRQSCLR/MAILBOX/HWCFG2/CPUCTL/DMACTL/DMATRF*/IMEMC/DMEMC/ENGINE/FBIF; PFALCON2 MOD_SEL/BROM*/PRISCV | ✅ |
| база GSP 0x110000/0x111000 | nova `falcon/gsp.rs` (`RegisterBase`) | `PFalconBase=0x110000`, `PFalcon2Base=0x111000` | ✅ |
| WPR2 lo/hi, GFW boot | nova `regs.rs` | `0x1fa824/0x1fa828`, `0x118234` (0xff=done) | 🟢 HW: WPR2-адреса читаемы (LO=0x1FFFFE00, HI=0 → не задан в простое); GFW не снят |
| `nv_falcon_start`/`boot`/mailbox | nova `falcon.rs` start/boot | CPUCTL alias→ALIAS.startcpu иначе CPUCTL.startcpu; wait halted | ✅ |
| `nv_falcon_reset_ga102` | nova `falcon.rs reset` + `hal/ga102.rs reset_eng` | reset_eng(reset_engine+scrub)→select_core→scrub(повторно)→FALCON_RM=boot0 | ✅ сверено 2026-06-28: порядок исправлен (добавлен 2-й wait_mem_scrubbing после select_core) |
| `nv_falcon_select_core_ga102` | `hal/ga102.rs select_core_ga102` | BCR_CTRL core_select=Falcon, ждать valid | ✅ |
| `nv_falcon_program_brom_ga102` | `hal/ga102.rs program_brom_ga102` | BROM_PARAADDR/ENGIDMASK/CURR_UCODE_ID, MOD_SEL=Rsa3k(1) | ✅ |
| `nv_falcon_dma_wr`/`dma_load` | nova `falcon.rs dma_wr/dma_load` | DMATRFBASE=(dma>>8), BASE1=(dma>>40)&0x1ff, cmd size 256B, imem/sec биты, poll idle; FBIF coherent sysmem/physical | ✅ |
| `nv_falcon_signature_fuse_version_ga102` | `hal/ga102.rs signature_reg_fuse_version` | fuse SEC2/NVDEC/GSP по engine_id_mask, idx=ucode_id-1, версия=старший бит | ✅ |

## Слой 2 — Шаг 3 (патч FWSEC), `driver/gsp/fwsec_patch.{h,c}`

| Наш код | Upstream (nova `fwsec.rs`) | Что взято | Сверено |
|---|---|---|---|
| `nv_fwsec_desc_parse` | `firmware.rs` V2/V3 | поля imem/dmem_load, interface_offset, pkc_data_offset, engine_id_mask, ucode_id, signature_count/versions | ✅ |
| `nv_fwsec_patch_frts` | `fwsec.rs new_fwsec` | AppifHdrV1@(imem_load+interface_offset); AppifV1(id,dmem_base); DMEMMAPPER init_cmd@44=FRTS(0x15); FrtsCmd@(imem_load+cmd_in_off): ReadVbios{1,24,0,0,2}+FrtsRegion{1,20,addr>>12,size>>12,FB=2} | ✅ |
| `nv_fwsec_select_signature_index` | `fwsec.rs FwsecFirmware::new` | bit=1<<fuse_ver; требуется (sig_versions&bit)!=0; idx=popcount(sig_versions&(bit-1)) | ✅ |
| `nv_fwsec_patch_signature` | `fwsec.rs` patch_signature | копия 384б подписи в imem_load+pkc_data_offset | ✅ |

## Слой 2 — Шаг 5 (kext DMA + оркестрация FWSEC), `driver/gsp/fwsec_locate.*` + `driver/RTXProbe/FwsecRun.*`

| Наш код | Upstream | Что взято | Сверено |
|---|---|---|---|
| `nv_fwsec_locate` (fwsec_locate.c) | nova `vbios.rs` | переиспользуемый локатор FWSEC (PCIR/NPDE/BIT/PmuLookupTable), без I/O | ✅ |
| `FwsecRun.cpp` (kext) | nova `fwsec.rs::new`+`run`, `falcon.rs` | VBIOS из BAR0 ROM shadow (0x300000) → locate → DMA-буфер (IOBufferMemoryDescriptor) → patch FRTS+signature → reset_ga102 → dma_load → boot → проверка mbox0/WPR2 | 🟡 CI (kext compile-check), на железе не исполнялось |
| `tools/fwsec_run_linux.c` (Linux-хост) | те же `driver/gsp/*` + VFIO | mmap BAR0 как `nv_mmio_t`, `VFIO_IOMMU_MAP_DMA`→IOVA в `dma_load`; оркестрация 1:1 с FwsecRun.cpp. Для HW-верификации слоя 2 headless без macOS | 📄 SRC, на железе не исполнялось (см. `docs/bench-test-fwsec-linux.md`) |
| frts_addr/frts_size (регион WPR2) | nova `fb.rs FbLayout` + `fb/hal/ga102.rs`/`tu102.rs` | `driver/gsp/fb_layout.*`: vidmem=NV_USABLE_FB_SIZE_IN_MB×1MiB; vga_workspace из NV_PDISP_VGA_WORKSPACE_BASE; frts_base=align_down(vga,128K)-1MiB; frts_size=1MiB | 🟡 CI / 📄 SRC |

## Слой 2 — ещё НЕ портировано (следующие шаги)

| Будущий модуль | Upstream | Что портировать |
|---|---|---|
| kernel DMA (kext) | Apple `IOBufferMemoryDescriptor` | coherent-буфер, физ. адрес для `dma_load` |
| `driver/gsp/booter.*` | nova `firmware/booter.rs` | загрузка Booter → GSP-RM в WPR2, старт RISC-V |
| `driver/gsp/rpc.*` | OGK `message_queue_priv.h`, nouveau `r535.c` | command/status очереди в sysmem, формат RPC, первый handshake |

## Конкретные upstream-ссылки (raw, ветка master ядра)

- nova vbios:   `drivers/gpu/nova-core/vbios.rs`
- nova firmware:`drivers/gpu/nova-core/firmware.rs`, `firmware/fwsec.rs`
- nova falcon:  `drivers/gpu/nova-core/falcon.rs`, `falcon/gsp.rs`, `falcon/hal/ga102.rs`
- nova regs:    `drivers/gpu/nova-core/regs.rs`
- nouveau GSP:  `drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c`
- OGK queues:   `src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h`

## Аудит против nova-core (2026-06-28)

Полная построчная сверка FWSEC-FRTS-цепочки с актуальным master ядра
(`falcon.rs`, `falcon/hal/ga102.rs`, `firmware.rs`, `firmware/fwsec.rs`,
`vbios.rs`, `fb.rs`, `fb/hal/{ga102,tu102}.rs`, `regs.rs`). Скачаны raw-исходники,
сверены смещения структур, битовые поля, порядок шагов.

**Подтверждено верным (без правок):** раскладки `FalconUCodeDescV2/V3`; FRTS-патч
(`init_cmd@44`, `cmd_in_buffer_offset@8`, `ReadVbios`/`FrtsRegion`); выбор подписи
(`popcount(versions & (1<<fuse - 1))`); различие смещений ucode (`desc_abs+hdr_size`)
и подписей (`desc_abs+sizeof(V3)=44`); все битовые поля regs.rs (DMATRFCMD, FBIF,
WPR2-decode `>>4<<12`/`hi_val!=0`, BCR_CTRL, BROM, fuse, VGA_WORKSPACE); fb_layout
(`vidmem_size` из `NV_USABLE_FB_SIZE_IN_MB` — верно для Ada/ga102; `frts_size=1MiB`;
`align_down(vga,128K)-1MiB`); локатор FWSEC.

**Найдено и исправлено:**
- **D1** (`falcon.c` `nv_falcon_reset_ga102`): пропущено повторное
  `reset_wait_mem_scrubbing` после `select_core` — добавлено (nova вызывает scrub
  дважды: внутри `reset_eng` и затем в `reset()`).
- **D2** (`FwsecRun.cpp`): добавлен явный отказ для дескриптора != V3 (путь
  DMA-таргетов и база подписей `+44` рассчитаны только на V3; AD104 FWSEC = V3).

**Верификационная заметка (на стенде):**
- **D3** (`fwsec_locate.c`): IFR-заголовок (`NVGI`) не разбирается; на Ada PROM-смещение
  обычно уже применено → скан с 0 корректен. Проверить по дампу ROM из BAR0+0x300000:
  первый word должен быть `0xAA55`, а не `NVGI`. Помечено `TODO: verify` в коде.
