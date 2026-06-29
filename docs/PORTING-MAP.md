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
| PciRomHeader | `PciRomHeader` | sig u16@0 (`0xAA55` ЛИБО `0x4E56`="NV" для NVIDIA-расширенных образов), pci_data_ptr u16@0x18 — ✅ сверено 2026-06-29 с nova `match {0xAA55\|0x4E56}`, подтверждено дампом (FWSEC-образ начинается с 0x4E56) |
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
  обычно уже применено → скан с 0 корректен. ✅ Подтверждено дампом 2026-06-29:
  PROM (BAR0+0x300000) начинается с `0xAA55`, `NVGI` в дампе НЕТ → IFR-разбор не нужен на AD104.

## Аудит против nova-core (2026-06-29) — HW-верификация, найдено 2 бага

Прогон FWSEC-FRTS на реальной RTX 4070S (Linux/VFIO). Метрика достигнута:
`mbox0=0`, `WPR2 set=1 [0x2ff800000..0x2ff8e0000]`. Доказательство:
`docs/hw-dumps/20260629-rtx4070s-fwsec-frts-linux-OK.log` (+ дамп PROM `...-prom-shadow.bin`).
Два бага, невидимые на CI/чтении, всплыли только на железе:

- **D4** (`falcon.c`/`falcon.h`: `nv_wait_gfw_boot_completed`, `nv_gfw_boot_raw`): после
  reset/FLR карта заново гоняет GFW boot (devinit из VBIOS). Старый код читал FB-регистры
  сразу → `NV_USABLE_FB_SIZE_IN_MB` = `0xBADF5108` (priv read-fault) → мусорный FRTS-регион.
  Добавлено ожидание `SCRATCH_GROUP_05.GFW_BOOT == 0xFF` (поллинг ≤4с, как nova при init GPU).
  Подключено в `fwsec_run_linux.c` и `FwsecRun.cpp` ПЕРЕД чтением FB.
  HW: до ожидания `0x00000001`, после `0x000003FF`; VRAM=12282 MiB.
- **D5** (`fwsec_locate.c` `walk_images`): принимал только сигнатуру образа `0xAA55`.
  NVIDIA-расширенные образы (NBSI/FWSEC, code_type 0xE0) начинаются с `0x4E56` ("NV") —
  `walk` останавливался на EFI-образе, FWSEC не находился (`rc=NOIMG`). Сверено с nova
  `vbios.rs` (`0xAA55 | 0x4E56`). На дампе: образы [0]PciAt@0 [1]EFI@0xfc00 [2]FWSEC@0x24c00
  [3]FWSEC@0x2ac00(last); desc V3 @0x45818 (GSP engine=0x400, ucode_id=9, sig_count=2).

| Наш код | Upstream | Что | Статус |
|---|---|---|---|
| `nv_wait_gfw_boot_completed` | nova `Gpu::new` ждёт GFW_BOOT | поллинг `0x118234 & 0xFF==0xFF` | ✅ HW 2026-06-29 |
| `walk_images` sig | nova `PciRomHeader` `0xAA55\|0x4E56` | обе сигнатуры образа | ✅ HW 2026-06-29 |

## Контейнер Booter (задача 6, фазы 1-2) — ✅ разобран офлайн 2026-06-29

Booter — внешний подписанный блоб (`booter_load`/`booter_unload` из linux-firmware),
формат «bin»-контейнер с Heavy-Secure firmware v2 (НЕ VBIOS-дескриптор). Парсер
`driver/gsp/booter.{h,c}`; загрузка/распаковка — шим `fw_blob` (`tools/fw_blob_linux.c`,
zstd). Смещения сверены с дампом `booter_load-535.113.01.bin` (всё file-relative, кроме
os_*/app/patch_loc — они data-relative).

| Наш код | Upstream | Структура/смещения |
|---|---|---|
| nv_bin_hdr (внутр.) | nouveau `nvfw/fw.h` `nvfw_bin_hdr` | magic u32@0 (`0x10de`), ver@4, size@8, header_offset@12, data_offset@16, data_size@20 |
| HS header v2 | nouveau `nvfw/hs.h` `nvfw_hs_header_v2` (nova `firmware/hs.rs`) | sig_prod_offset@0, sig_prod_size@4, **patch_loc_off@8**, patch_sig_off@12, meta_data_offset@16, meta_data_size@20, **num_sig_off@24**, load_hdr_off@28, header_size@32. Поля `*_off` — АБСОЛЮТНЫЕ смещения к значениям/структурам |
| load header v2 | nouveau `nvfw/hs.h` `nvfw_hs_load_header_v2` | os_code_offset@0, os_code_size@4, os_data_offset@8, os_data_size@12, num_apps@16, app[]{offset,size,data_offset,data_size}@20 (по 16б) |
| `nv_booter_parse` | nova `BinFirmware`+`HsFirmwareV2::new` | разбор контейнера, `num_sig = u32@num_sig_off`, `patch_loc = u32@patch_loc_off`, `pkc_data_offset = patch_loc - os_data_offset` |

Подтверждено дампом 535.113.01: bin magic `0x10de`, data@888 size=0xd700; sig@60
size=768 → num_sig=2; load_hdr@852: os_code(0,256)+app(256,30208)+os_data(30464,24576)=55040;
patch_loc=30480 → pkc_data_offset=16; meta_data@836 (12б)=[1,1,3] (`TODO: verify` —
вероятно engine_id_mask SEC2=1 / ucode_id=3, нужно сверить с `booter.rs HsSignatureParams`).

НЕ портировано (фазы 3-6): запуск на SEC2 (regs SEC2-базы), `GspFwWprMeta`, radix3,
libos, оркестрация boot. Источники: nova `firmware/{booter,gsp}.rs`, nouveau `r535`.

## SEC2-путь Booter (задача 6, фаза 3) — ✅ HW 2026-06-29

Пробный dry-load Booter на SEC2 на реальной RTX 4070S (Linux/VFIO), `tools/booter_run_linux.c`.
WPR-handle в mbox = 0 (dummy, GSP-RM ещё не подготовлен).
Доказательство: `docs/hw-dumps/20260629-rtx4070s-booter-sec2-dryload.log`.

| Наш код | Upstream | Что | Статус |
|---|---|---|---|
| `NV_PSEC_FALCON_BASE`=0x840000, `_FALCON2_BASE`=0x841000 | nova `falcon/sec2.rs` BASE | база SEC2 | ✅ HW (HWCFG2=0x67b7) |
| HsSignatureParams | nova `booter.rs` (meta_data) | fuse_ver@0, engine_id_mask@4, ucode_id@8 | ✅ {1, 1=SEC2, 3} |
| `nv_booter_select_signature` | nova `booter.rs` | reg_fuse==0→last; иначе idx=fuse_ver-reg_fuse | ✅ HW (reg_fuse=1→idx=0) |
| DMA-таргеты Booter (GA102+) | nova `booter.rs` FalconDmaLoadable | imem_sec{src=app0.off,dst=0,len=app0.size}; dmem{src=os_data_off,dst=0,len=os_data_size}; bootvec=app0.off | ✅ HW |
| reset/dma_load/boot на SEC2 | reuse `falcon.c` (generic, base-параметризован) | reset_ga102/dma_load_ga102/boot с базой SEC2 | ✅ HW |

Результат HW: SEC2 reset OK → dma_load OK → boot **halted**, `mbox0=0x89` (контролируемая
ошибка Booter «нет валидной WPR meta» — ожидаема при dummy-handle). Главное: **BROM
принял подпись** (HS-код исполнился) → reset+DMA+BROM(подпись)+boot на SEC2 работают.
Осталось (фазы 4-6): `GspFwWprMeta`+radix3+libos+оркестрация → реальный WPR-handle → mbox0==0.

## Подготовка GSP-RM (задача 6, фаза 4, часть 1) — ✅ офлайн 2026-06-29

Модули `driver/gsp/elf64.{h,c}` + `driver/gsp/gsp_fw.{h,c}`; офлайн-тест
`tools/gsp_stage_test.c` (`make gsp-stage-test`) — на реальных gsp/bootloader-535.113.01.

| Наш код | Upstream | Структура/смещения | Статус |
|---|---|---|---|
| `nv_gsp_rm_sections` | ELF64 + nouveau r535 | образ GSP-RM = ELF64; `.fwimage` (прошивка), `.fwsignature_ad10x` (подпись AD104) | ✅ .fwimage @0x40 size 0x2448000 (36 МБ), .fwsignature_ad10x @0x244a04b size 0x1000 |
| `nv_gsp_bootloader_parse` | nouveau `nvrm/gsp.h` `RM_RISCV_UCODE_DESC` (21×u32 от header_offset bin) | version@0, bootloaderOffset@4, …, appVersion@28, manifestOffset@32/Size@36, monitorDataOffset@40/Size@44, monitorCodeOffset@48/Size@52 | ✅ code_off=18432 data_off=2048 manifest_off=0 |
| radix3 (`nv_gsp_radix3_levels/fill`) | nouveau r535 `nvkm_gsp_radix3` | 3 уровня, 4K, u64-записи; lvl2[i]=fw+i·4K, lvl1[j]=lvl2+j·4K, lvl0[0]=lvl1; ёмкость 512·512·4K=1ГБ | ✅ n2=9288, lvl2=19 стр., lvl1=19, lvl0=1 |

### GspFwWprMeta — раскладка (nouveau `nvrm/gsp.h`, для фазы 4 части 2; ещё НЕ реализовано)
Все поля NvU64, padding до 256 байт. magic=`0xdc3aae21371a60b3`, revision=1.
Порядок: magic@0, revision@8, sysmemAddrOfRadix3Elf@16, sizeOfRadix3Elf@24,
sysmemAddrOfBootloader@32, sizeOfBootloader@40, bootloaderCodeOffset@48,
bootloaderDataOffset@56, bootloaderManifestOffset@64, [union sysmemAddrOfSignature@72,
sizeOfSignature@80], gspFwRsvdStart@88, nonWprHeapOffset@96/Size@104, gspFwWprStart@112,
gspFwHeapOffset@120/Size@128, gspFwOffset@136, bootBinOffset@144, frtsOffset@152/Size@160,
gspFwWprEnd@168, fbSize@176, vgaWorkspaceOffset@184/Size@192, bootCount@200,
[union partitionRpc…/crashReport… 8-байт-выровнен], gspFwHeapVfPartitionCount(u8)+padding[7],
verified(u64). Математика WPR2-layout (gspFwWprStart/End, heap, frts) — из FbLayout (фаза 4-2).

## WPR2-раскрой + GspFwWprMeta (задача 6, фаза 4 ч.2) — ✅ офлайн 2026-06-29

`driver/gsp/gsp_fw.{h,c}`: `nv_gsp_fb_layout` (раскрой WPR2) + `nv_gsp_wpr_meta_build`
(256-байтная структура). Проверено офлайн на FB=12282 МиБ (HW-значение) с опорой на
HW-проверенный FRTS (`nv_fb_compute_frts`): `make gsp-stage-test`.

| Наш код | Upstream | Что | Статус |
|---|---|---|---|
| `nv_gsp_fb_layout` | nouveau `r535_gsp_oneinit` | раскрой сверху вниз: frts(анкер)→boot(align 4K)→elf(64K)→heap(1M)→wpr2(1M, −sizeof meta)→non-WPR heap(1M); wpr2_end=frts_end | ✅ офлайн (wpr2=0x2f5400000..0x2ff900000, внутри 12ГБ) |
| heap.size | nouveau ad102.c + OGK gsp_fw_heap.h | os_carveout(20M)+base(8M)+align(96K·fb_gb,1M)+align(client 96M,1M), max(min 84M) | ✅ 126 МиБ при 12 ГБ FB. TODO: verify HW |
| `nv_gsp_wpr_meta_build` | nouveau `r535_gsp_wpr_meta_init` | поля GspFwWprMeta (magic@0…verified@248), FB-адреса из раскроя + sysmem-DMA radix3/bootloader/sig | ✅ офлайн (magic/wprStart/End/fbSize верны) |

Остаётся (фаза 5-6): libos init-args; оркестрация на железе (выделить sysmem под
radix3/bootloader/sig/meta/libos, boot GSP-фалкона с libos-handle, Booter с реальным
WPR-handle=meta DMA → mbox0==0 → RISC-V active).
