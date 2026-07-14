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

## Слой 4 — каналы GPFIFO — 🟢 ПРОХОД A НА ЖЕЛЕЗЕ 2026-07-14

`driver/gsp/gsp_fifo.{c,h}` (порт nouveau `fifo/r535.c`), офлайн-тест
`tools/gsp_fifo_test.c`. Тех-запись: `docs/gsp-layer4-fifo.md`. Доказательство:
`docs/hw-dumps/20260714-rtx4070s-layer4-passA-chan-OK.log`.

| Наш код | Upstream | Что взято | Статус |
|---|---|---|---|
| классы канала/движка | nouveau `nvif/class.h` + `fifo/ga102.c` | `AMPERE_CHANNEL_GPFIFO_A`=0xC56F, `KEPLER_CHANNEL_GROUP_A`=0xA06C, `AMPERE_USERMODE_A`=0xC561, `AMPERE_DMA_COPY_B`=0xC7B5, `ADA_A`=0xC997, `ADA_COMPUTE_A`=0xC9C0 | 📄 сверено |
| `nv_gsp_rm_channel_alloc` | `r535_chan_ramfc_write` + `alloc/alloc_channel.h` | `NV_CHANNEL_ALLOC_PARAMS` (360б, compile-probe): gpFifoOffset@8/entries@16/flags@20/hVASpace@28/engineType@128 + instance/userd/ramfc/mthdbuf `NV_MEMORY_DESC_PARAMS`(24б, as=VIDMEM 2); flags PRIV(bit5)\|PAGE_FIXED(bit21) | 🟢 HW 2026-07-14 (status=NV_OK) |
| `nv_gsp_rm_channel_bind` | `ctrla06fgpfifo.h` `NVA06F_CTRL_CMD_BIND` | RM_CONTROL cmd=0xa06f0104, {engineType}(4б) | 🟢 HW 2026-07-14 |
| `nv_gsp_rm_channel_schedule` | `ctrla06fgpfifo.h` `NVA06F_CTRL_CMD_GPFIFO_SCHEDULE` | RM_CONTROL cmd=0xa06f0103, {bEnable,bSkipSubmit}=**ровно 2б** (4б→`INVALID_PARAM_STRUCT 0x3a`) | 🟢 HW 2026-07-14 |
| `nv_gsp_rm_engine_obj_alloc`/`nv_gsp_rm_ce_obj_alloc` | `r535` + OGK `clb0b5sw.h` | GSP_RM_ALLOC hParent=канал, класс `AMPERE_DMA_COPY_B (0xC7B5)`, `NVB0B5_ALLOCATION_PARAMETERS{version=1, engineType=CE0}` | 🟢 HW 2026-07-14 (проход B, status=NV_OK) |

| `nv_gsp_fifo_get_device_info` | `r535_fifo_runl_ctor` + `ctrl2080fifo.h` `FIFO_GET_DEVICE_INFO_TABLE` | cmd=0x20801112, params 3212б; парс engineData[16] по ENGINE_INFO_TYPE (ENG_DESC=0/RM_ENGINE_TYPE=2/RUNLIST=3/PRI_BASE=11) | 🟢 HW 2026-07-14 (11 движков, CE0=0x9 runl0) |

`engineType` (NV2080_ENGINE_TYPE_* == RM_ENGINE_TYPE) взят с железа через
`FIFO_GET_DEVICE_INFO_TABLE` (A0 🟢, CE0=0x9). **Проход A (A0+A1+A2) на железе 🟢**:
канал `AMPERE_CHANNEL_GPFIFO_A` создан+bind+schedule, всё `NV_OK`. Пруф:
`docs/hw-dumps/20260714-rtx4070s-layer4-passA-chan-OK.log`. **Проход B 🟢** (объект
CE `AMPERE_DMA_COPY_B` на канале, `docs/hw-dumps/20260714-rtx4070s-layer4-passB-ceobj-OK.log`).
**Проход C 🟢 HW** — pushbuffer с CE `SEMAPHORE_RELEASE` + GPFIFO-запись + USERD GP_PUT +
doorbell (`AMPERE_USERMODE_A 0xC561`) → host-семафор `0xcafe0001`, read-back MATCH.
Первая команда GPU исполнена; слой 4 замкнут. Пруф:
`docs/hw-dumps/20260714-rtx4070s-layer4-passC-exec-OK.log`.

## Слой 5 — дисплей (энумерация + EDID) — 🟢 5A+5B НА ЖЕЛЕЗЕ 2026-07-14

`driver/gsp/gsp_disp.{c,h}` (порт nouveau `disp/r535.c`), офлайн-тест
`tools/gsp_disp_test.c`. Тех-запись: `docs/gsp-layer5-display.md`. Пруф:
`docs/hw-dumps/20260714-rtx4070s-layer5-A0-disp-OK.log`.

| Наш код | Upstream | Что взято | Статус |
|---|---|---|---|
| `nv_gsp_disp_common_alloc` | `r535_disp_oneinit` + `nvif/class.h` | `NV04_DISPLAY_COMMON`=0x0073, hObject=0x00730000, paramsSize=0 | 🟢 HW (status=NV_OK) |
| `nv_gsp_disp_get_num_heads` | `ctrl0073system.h` `GET_NUM_HEADS` | cmd=0x730102, `{subDeviceInstance,flags,numHeads}` (12б, OUT@8) | 🟢 HW (heads=4) |
| `nv_gsp_disp_get_supported` | `ctrl0073system.h` `GET_SUPPORTED` | cmd=0x730120, `{subDeviceInstance,displayMask,displayMaskDDC}` (12б, OUT@4/@8) | 🟢 HW (0x7f00) |
| `nv_gsp_disp_or_get_info` | `ctrl0073specific.h` `OR_GET_INFO` + `r535_outp_new` | cmd=0x73028b (56б): displayId@4 index@8 type@12 protocol@16 location@28; SOR=2, DP_A=8/DP_B=9/TMDS_A=1/TMDS_B=2 | 🟢 HW (7 OR: SOR TMDS/DP) |
| `nv_gsp_disp_get_connect_state` | `ctrl0073system.h` `GET_CONNECT_STATE` + `r535_outp_detect` | cmd=0x730122 (16б): flags@4 displayMask@8(IN/OUT) | 🟢 HW (connected=0x300) |
| `nv_gsp_disp_get_edid` | `ctrl0073specific.h` `GET_EDID_V2` + `r535_tmds_edid_get` | cmd=0x730245 (2064б): displayId@4 bufferSize@8 flags@12 edidBuffer[2048]@16 | 🟢 HW (size=384, magic ok) |

Дальше: 5C (display root `AD102_DISP 0xC770` + inst-mem + core/wndw каналы +
framebuffer + link training + modeset/scanout). Display root класс — `g_allclasses.h`/`nvif/class.h`.

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

## libos init-args (задача 6, фаза 5) — ✅ офлайн 2026-06-30

`driver/gsp/gsp_fw.{h,c}`: `nv_gsp_libos_id8`, `nv_gsp_libos_build_args`,
`nv_gsp_pte_array_fill`. Проверено офлайн (`make gsp-stage-test`).

| Наш код | Upstream | Что | Статус |
|---|---|---|---|
| `LibosMemoryRegionInitArgument` | OGK `libos_init_args.h` | id8@0,pa@8,size@16,kind@24(u8),loc@25(u8); 32б | ✅ |
| kind/loc | OGK enum | CONTIGUOUS=1, LOC_SYSMEM=1 | ✅ |
| `nv_gsp_libos_id8` | nouveau `r535_gsp_libos_id8` | id=(id<<8)\|c, до 8 символов | ✅ id8(LOGINIT)=0x4c4f47494e4954 |
| `nv_gsp_libos_build_args` | nouveau `r535_gsp_libos_init` | записи LOGINIT/LOGINTR/LOGRM(0x10000)+RMARGS, CONTIGUOUS/SYSMEM | ✅ |
| `nv_gsp_pte_array_fill` | nouveau `create_pte_array` | u64 на 4K-страницу: base+i·4K (put-указатель@0, PTE@8) | ✅ |

Все офлайн-кирпичи фазы 4-5 готовы. Остаётся фаза 6 (HW): оркестратор — выделить
sysmem (VFIO) под radix3(lvl0/1/2)+bootloader+signature+WprMeta+libos+логи, разместить,
boot GSP-фалкона с libos-handle, SEC2 Booter с WPR-handle=WprMeta DMA → mbox0==0 → RISC-V active.

## Полная загрузка GSP-RM (задача 6, фаза 6) — ✅ HW 2026-06-30 — МЕТРИКА ДОСТИГНУТА

Оркестратор `tools/gsp_boot_linux.c` на реальной RTX 4070S (Linux/VFIO). Полная цепочка
слоя 2 за один прогон. Доказательство: `docs/hw-dumps/20260630-rtx4070s-gsp-rm-boot-OK.log`.

Последовательность (порт nouveau r535_gsp_oneinit + r535_gsp_init, путь SEC2/Ada):
1. FWSEC-FRTS → WPR2 (reuse) — `mbox0=0`, WPR2 set;
2. staging в sysmem (DMA-арена): .fwimage + radix3(lvl0/1/2) + bootloader + signature(.fwsignature_ad10x) + GspFwWprMeta + libos(+LOGINIT/INTR/RM, RMARGS);
3. `nv_falcon_gsp_reset_riscv` (reset_eng + BCR_CTRL|=0x111 RISC-V) → GSP mailbox0/1 = libos-адрес;
4. SEC2 Booter с mbox0/1 = WprMeta-адрес → Booter грузит GSP-RM в WPR2 и стартует RISC-V;
5. FALCON_OS=app_version; проверка RISC-V active.

| Наш код | Upstream | Что | Статус |
|---|---|---|---|
| `nv_falcon_gsp_reset_riscv` | nouveau `ga102_gsp_reset` | reset_eng + BCR_CTRL(0x111668)\|=0x111 (RISC-V core) | ✅ HW |
| `nv_falcon_riscv_active` | nouveau `riscv_active` | CPUCTL(PFALCON2+0x388) active_stat=bit7 | ✅ HW (CPUCTL=0x80) |
| GSP mbox=libos | nouveau wr 0x040/0x044 | MAILBOX0/1 GSP = libos DMA addr | ✅ HW |
| Booter mbox=WprMeta | nouveau `r535_gsp_booter_load` | SEC2 mbox0/1 = wpr_meta.addr | ✅ HW (mbox0=0!) |
| `nv_falcon_write_os_version` | nouveau wr 0x080 | FALCON_OS = app_version | ✅ HW |

**Результат HW:** FWSEC `mbox0=0`/WPR2 set; Booter `mbox0=0` (с реальным WprMeta — vs 0x89 на
dummy в фазе 3); GSP RISC-V `active=1`. Метрика задачи 6 (Booter mbox0==0 И RISC-V active)
ДОСТИГНУТА. Осталось: задача 7 (очереди RPC sysmem + первый handshake = «GSP ответил»).

## Очереди RPC GSP-RM (задача 7, часть 1) — ✅ офлайн 2026-06-30

`driver/gsp/gsp_rpc.{h,c}` + `tools/gsp_rpc_test.c` (`make gsp-rpc-test`).

| Наш код | Upstream | Что | Статус |
|---|---|---|---|
| `msgqTxHeader`/`RxHeader` | OGK `msgq_priv.h` | tx(32б): version/size/msgSize/msgCount/writePtr/flags/rxHdrOff/entryOff; rx: readPtr | ✅ |
| `nv_gsp_shm_init` | nouveau `r535_gsp_shared_init` | [PTE][cmdq 0x40000][msgq 0x40000]; entryOff=4K, msgSize=4K, msgCount=63, flags=1, rxHdrOff=32; PTE маппят весь регион (129 страниц) | ✅ офлайн |
| GSP_MSG_QUEUE_ELEMENT | OGK `message_queue_priv.h` | authTag[16]+aad[16]+checkSum+seqNum+elemCount, rpc@48 (HDR=48) | 📄 (константы) |
| rpc-заголовок | nouveau `r535_gsp_rpc_get` | header_version=0x03000000, signature=0x43505256 ('VRPC') | 📄 |

Остаётся (задача 7 ч.2, HW): `GSP_ARGUMENTS_CACHED` (rmargs: messageQueueInitArguments —
sharedMemPhysAddr/pageTableEntryCount/cmd+statQueueOffset) + интеграция в оркестратор +
поллинг msgq на событие `GSP_INIT_DONE`. Вероятны HW-итерации (молчаливые сбои GSP).

## rmargs + RPC handshake (задача 7, часть 2) — 🔶 ЧАСТИЧНО (HW 2026-06-30)

`gsp_rpc.{h,c}` (`nv_gsp_rmargs_build`, `nv_gsp_msgq_writeptr`) + интеграция в
`tools/gsp_boot_linux.c` (shared-регион очередей + rmargs в RMARGS-регионе libos +
поллинг msgq). Доказательство-диагностика: `docs/hw-dumps/20260630-rtx4070s-gsp-rpc-partial.log`.

| Наш код | Upstream | Что | Статус |
|---|---|---|---|
| `MESSAGE_QUEUE_INIT_ARGUMENTS` | OGK `gsp_init_args.h` | sharedMemPhysAddr@0(u64), pageTableEntryCount@8(u32), cmdQueueOffset@16(u64), statQueueOffset@24(u64), lockless@32/40 | ✅ офсеты |
| `GSP_ARGUMENTS_CACHED` (rmargs) | OGK `gsp_init_args.h` + nouveau `r535_gsp_rmargs_init` | mq@0(48)+srInit@48(12)+gpuInstance@60+profilerArgs@64; всего 80б | 🔶 заполнен, GSP не дошёл до INIT_DONE |

**HW-результат (частичный):** задача 6 держится (Booter mbox0=0, RISC-V active). GSP-RM
**исполняет init и пишет логи** (LOGINIT put=0x3e32 ~16КБ — значит libos-args+rmargs читаются
GSP), но `msgq.writePtr=0` (нет GSP_INIT_DONE), LOGRM едва тронут (put=0x185) → GSP-RM падает
рано в RM-init. Нужен декодер libos-логов или побайтовая сверка rmargs/msgq для диагноза.

## Слой 3 — двусторонний RPC + RM-объекты (проходы A–D)

`driver/gsp/gsp_rm.{c,h}` (порт nouveau `r535.c`), офлайн-тест `tools/gsp_rm_test.c`,
блок «СЛОЙ 3» в `tools/gsp_boot_linux.c`. Доказательство:
`docs/hw-dumps/20260630-rtx4070s-layer3-rpc-OK.log`.

| Наш код | Upstream (nouveau r535.c / nvrm 535.113.01) | Что взято | Статус |
|---|---|---|---|
| `nv_gsp_rpc_call` | `r535_gsp_cmdq_push` + `r535_gsp_msgq_wait/recv` + `r535_gsp_msg_recv` | send в cmdq (checksum, elem_count, звонок 0xc00) + poll msgq до function==fn, пропуск async, consume по readPtr | 🟢 HW |
| номера функций | `rpc_global_enums.h` | ALLOC_MEMORY=4, MAP_MEMORY_DMA=14, GET_GSP_STATIC_INFO=65, GSP_RM_CONTROL=76, GSP_RM_ALLOC=103 | 🟢 HW (4,65,76,103); 14 🔴 GSP отверг (INVALID_FUNCTION) |
| `rpc_gsp_rm_alloc_v03_00` | `g_rpc-structures.h` | hClient@0 hParent@4 hObject@8 hClass@12 status@16 paramsSize@20 flags@24 reserved[4]@28 params@32 | 🟢 HW |
| `nv_gsp_get_static_info` | `r535_gsp_rpc_get_gsp_static_info` | req=2168 нулей; парс fbRegionInfoParams@840 + barPde@2088/2096 + hInternal@2152/2156/2160 | 🟢 HW |
| `GspStaticConfigInfo` смещения | `gsp_static_config.h` (compile-probe) | sizeof=2168; fbRegion 48б/запись (base@0/limit@8/reserved@16/perf@24/comp@28/iso@29/prot@30), MAX=16 | 🟢 HW (+ самопроверка) |
| `nv_gsp_rm_client_ctor` | `r535_gsp_client_ctor` | NV01_ROOT(0x0), h=0xc1d00000, NV0000{hClient,processID=~0,name[100]}(108) | 🟢 HW |
| `nv_gsp_rm_device_ctor` | `r535_gsp_device_ctor` | NV01_DEVICE_0(0x80), h=0xde1d0000, NV0080{hClientShare,…}(56) | 🟢 HW |
| `nv_gsp_rm_subdevice_ctor` | `r535_gsp_subdevice_ctor` | NV20_SUBDEVICE_0(0x2080), h=0x5d1d0000, NV2080{subDeviceId=0}(4) | 🟢 HW |
| `nv_gsp_rm_control` | `r535_gsp_rpc_rm_ctrl_get/push` | rpc_gsp_rm_control_v03_00 (24б): hClient@0 hObject@4 cmd@8 status@12 paramsSize@16 flags@20 params@24; params IN/OUT | 🟢 HW |
| `nv_gsp_fb_get_info` | OGK `ctrl2080fb.h` FB_GET_INFO_V2 | cmd=0x20801303; params fbInfoListSize@0 + fbInfoList[54] {index,data}(8б); индексы RAM=0x07/HEAP=0x09/HEAP_FREE=0x16/BAR1=0x05 | 🟢 HW (RAM=12282 МиБ) |
| `nv_gsp_rm_vaspace_ctor` | `cl90f1.h` + `nvos.h` | FERMI_VASPACE_A(0x90f1), h=0x90f10000, NV_VASPACE_ALLOCATION_PARAMETERS(48б): index=GPU_NEW(0) | 🟢 HW |
| `nv_gsp_rm_vram_memlist` | `instmem/r535.c` `fbsr_memlist` + `g_rpc-structures.h`/`sdk-structures.h`/`cl84a0.h` | ALLOC_MEMORY(4), NV01_MEMORY_LIST_FBMEM(0x82), rpc_alloc_memory_v13_01 (pteDesc@44, pte[]@48), flags=0x40000200, format=6, pte[i]=(phys>>12)+i | 🟢 HW (phys=0x13100000, 1 МиБ) |
| `nv_gsp_rm_vmem_ctor` | OGK `virtual_mem.c` `virtmemConstruct` + `cl0070.h` | NV01_MEMORY_VIRTUAL(0x70), h=0x00700001, NV_MEMORY_VIRTUAL_ALLOCATION_PARAMS(24б): offset@0=0, limit@8=0, hVASpace@16 | 🟢 HW (status=0, объект создан) |
| `nv_gsp_rm_map_memory_dma` | OGK `rpc.c` `rpcMapMemoryDma_v03_00` + `g_rpc-structures.h`/`g_sdk-structures.h`/`nvos.h` | MAP_MEMORY_DMA(14), NVOS46(56б): hDma=NV01_MEMORY_VIRTUAL, hMemory=memlist, offset=0, length=1 МиБ, flags=0, dmaOffset IN=0/OUT@40, status@48 | 🔴 HW: GSP отверг `rpc_result=0x2a NV_ERR_INVALID_FUNCTION` (fn=14 не диспетчеризуется в GSP-offload; путь vGPU) |
| `driver/gsp/gmmu.{c,h}` PRAMIN | nouveau `instmem/nv50.c` + OGK `bar2_walk.c` | `NV_PBUS_BAR0_WINDOW`=0x1700 (_BASE 23:0=phys>>16, _TARGET 25:24), окно данных `BAR0+0x700000` (1 МиБ); аим+rd/wr/fill/wr64 | 🟢 HW (read-back PTE через PRAMIN совпал) |
| `nv_gmmu_make_pte/pde` + `map_range` | nouveau `vmmgp100.c` (`gp100_vmm_pgt_pte`/`pde`/`valid`, `desc_12`) | формат Ada=ga10x: PTE=(phys>>4)\|VALID, aper VIDMEM=0, kind=0; PDE=(pt>>4)\|APER_VRAM(1<<1); иерархия PD3[48:47]/PD2[46:38]/PD1[37:29]/PD0[28:21 dual16б]/SPT[20:12] | 🟢 HW (PTE `0x1310001` MATCH) |
| `nv_gsp_rm_vaspace_copy_pdes` | nouveau `mmu/r535.c` `r535_mmu_promote_vmm` + OGK `ctrl90f1.h` | RM_CONTROL `COPY_SERVER_RESERVED_PDES`(0x90f10106) на vaspace; PARAMS(184б, compile-probe): pageSize=0x20000000, virtAddrLo/Hi, numLevelsToCopy=3, levels[6]{physAddress@0,size@8,aperture@16,pageShift@20}(24б); L0 PD3{0x20,1,0x2f} L1 PD2{0x1000,1,0x26} L2 PD1{0x1000,1,0x1d} | 🟢 HW (status=NV_OK) |

**Полная тех-запись:** `docs/gsp-layer3-rpc.md` (проходы A+B+C+D). Проход D закрыт
на железе через **прямой GMMU-маппинг** (host-side page-tables во VRAM + `RM_CONTROL
COPY_SERVER_RESERVED_PDES`, как nouveau `vmm`/`r535_mmu_promote_vmm`): `status=NV_OK`,
read-back PTE совпал (`docs/hw-dumps/20260714-rtx4070s-layer3-passD-gmmu-OK.log`).
Слой 3 закрыт полностью (A+B+C+D) → дальше слой 4 (каналы: FIFO/GR).
Примечания: FB_GET_INFO_V2 отсутствовал в nouveau-выжимке — дотянут из полного
публичного OGK `ctrl2080fb.h` (535.113.01), в репо не коммитим. **Тупик 1:** VRAM НЕ
через `NV01_MEMORY_LOCAL_USER`/heap-alloc GSP (отвергается) — гость даёт физ. страницы
(memlist), см. тех-запись §4C/§5. **Тупик 2:** VRAM НЕ маппится в GPU VA через
`MAP_MEMORY_DMA (14)` RPC — GSP отвечает `NV_ERR_INVALID_FUNCTION` (это путь
vGPU/SR-IOV; в GSP-offload page-tables ведёт host RM локально). Правильно — прямой
GMMU (§4D.2/§4D.3), реализовано и подтверждено.
