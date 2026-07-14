# IMPLEMENTATION.md — состояние реализации (источник правды)

> Этот файл фиксирует, **что уже реализовано, где и как**, и **что делать дальше**,
> чтобы не сбиться с пути корректной реализации. Обновляется после каждого
> значимого шага. Парный документ — [PORTING-MAP.md](./PORTING-MAP.md) (соответствие
> наш код ↔ исходники nova-core/open-gpu-kernel-modules).
>
> Правило: **все регистры/структуры/последовательности портируются из публичных
> исходников и сверяются** (см. AGENTS.md). Ничего не выдумывать.

---

## ⚠️ Статус проверки (честно)

**Портируемая логика слоя 2 (`driver/gsp/*`) ПОДТВЕРЖДЕНА НА ЖЕЛЕЗЕ** (Linux/VFIO,
RTX 4070S, 2026-06-29): FWSEC-FRTS отработал, `mbox0=0`, `WPR2 set=1`. macOS-kext-шим
(`FwsecRun.cpp`: IOPCIDevice/IOBufferMemoryDescriptor) — всё ещё 🟡 (на самой macOS не
прогонялся). Легенда статусов:

- 🟢 **HW** — подтверждено логом с реального железа. Есть:
  - декод `PMC_BOOT_0`=`0x194000A1` (Ada AD104 rev A1) — Windows/RW-Everything + Linux/VFIO;
  - **FWSEC-FRTS: `mbox0=0`, `WPR2 set=1 [0x2ff800000..0x2ff8e0000]`** (Linux/VFIO,
    `docs/hw-dumps/20260629-rtx4070s-fwsec-frts-linux-OK.log`).
- 🟡 **CI** — только компилируется (есть CI-лог GitHub Actions); поведение на
  железе НЕ проверено.
- 📄 **SRC** — раскладка/логика совпадает с исходником nova-core при чтении кода;
  на железе НЕ исполнялось.

Реально подтверждены: (1) код компилируется (CI-лог); (2) PCI-идентификаторы карты
из **Windows** (`Get-PnpDevice`); (3) **🟢 декод `PMC_BOOT_0` на реальной RTX 4070
Super** — `0x194000A1` → Ada AD104, rev A1 (Windows/RW-Everything,
`docs/hw-dumps/`). НЕ подтверждено: загрузка/работа kext на macOS.

Дополнительно: **блокер вывода изображения** — см. `docs/macos-graphics-stack.md`.
Публичной точки расширения для стороннего GPU-акселератора в WindowServer на
Big Sur+ нет (library validation + приватные интерфейсы + DriverKit без графики).

---

## Карта слоёв и статус

| Слой | Что | Статус | Файлы |
|---|---|---|---|
| 1 | PCIe bring-up, чтение `PMC_BOOT_0` | 🟡 CI (на железе не запускался) | `pcie_probe.c`, `ada_regs.h`, `driver/RTXProbe/`, `driver/RTXProbeDext/` |
| 2 | GSP bring-up (FWSEC-FRTS→WPR2→GSP-RM→`GSP_INIT_DONE`) | 🟢 HW 2026-06-30 (Linux/VFIO) для `driver/gsp/*`; macOS-kext-шим `FwsecRun.cpp` — 🟡 CI | `tools/{vbios_dump,fwsec_run_linux,gsp_boot_linux}.c`, `falcon_regs.h`, `driver/gsp/*` |
| 3 | Память (GMMU/VRAM) | 🟢 HW **A** (RPC + RM client/device/subdevice) + **B** (RM_CONTROL/FB_GET_INFO_V2 + FERMI_VASPACE_A/GMMU) + **C** (VRAM memlist NV01_MEMORY_LIST_FBMEM) + **D** (прямой GMMU: page-tables во VRAM + COPY_SERVER_RESERVED_PDES, 2026-07-14), 2026-06-30..07-14 | `driver/gsp/{gsp_rm,gmmu}.*`, `tools/{gsp_boot_linux,gsp_rm_test,gmmu_test}.c` |
| 4 | Каналы (command submission: FIFO/GR) | 🟢 **ПРОХОДЫ A+B НА ЖЕЛЕЗЕ 2026-07-14**: A0 (таблица движков, CE0=0x9) + A1 (буферы VRAM) + A2 (channel alloc `AMPERE_CHANNEL_GPFIFO_A` + BIND + SCHEDULE) + B (объект CE `AMPERE_DMA_COPY_B` на канале), всё `NV_OK`. Дальше C (pushbuffer+семафор = исполнение команды) | `driver/gsp/gsp_fifo.*`, `tools/gsp_fifo_test.c` |
| 5–6 | дисплей/Metal | ⏳ (дисплей — заблокирован Apple, см. graphics-stack) | — |

---

## Слой 1 — статус 🟡 CI / 📄 SRC (на железе не проверено)

### `pcie_probe.c`
User-space разведка PCIe через IOKit: vendor/device/subsystem/class/BAR из
IORegistry. Без драйвера и без отключения SIP. Собирается в CI.

### `ada_regs.h`
Определения `NV_PMC_BOOT_0` (offset 0x0) и декод чипа:
- `architecture[28:24]`, `implementation[23:20]`, `chipset=(raw>>20)&0x1ff`.
- Сверено с nova-core: AD104 → chipset `0x194`, arch `0x19` (Ada).
- Имена арх/чипа, проверка правдоподобности значения.

### `driver/RTXProbe/` (kext) и `driver/RTXProbeDext/` (dext)
Матчатся на `10DE:2783` (`IOPCIPrimaryMatch`), мапят BAR0, читают `PMC_BOOT_0`,
декодят и логируют «Ada AD104». kext — основной канал (OpenCore), dext — через
PCIDriverKit `MemoryRead32`. Сборка проверяется в CI (kext обязателен).

**Метрика слоя 1 (чтение chip ID):** декодер `PMC_BOOT_0` **🟢 подтверждён на
реальной RTX 4070 Super** (значение `0x194000A1` → Ada AD104, rev A1; Windows/
RW-Everything). Чтение через НАШ kext в среде macOS — ещё ждёт стенда.

---

## Слой 2 — статус 🟡 CI / 📄 SRC (на железе не исполнялось)

Полный план — [gsp-bringup-notes.md](./gsp-bringup-notes.md). Ниже — что уже есть.

### Шаг 1 — VBIOS reader / FWSEC locator — 🟡 CI / 📄 SRC (не прогонялся на реальном VBIOS) — `tools/vbios_dump.c`
Алгоритм (портирован из nova-core `vbios.rs`):
1. Читает VBIOS (файл-дамп или Linux sysfs `rom`).
2. Идёт по образам PCI ROM (PciRomHeader `0xAA55` → PCIR/NPDS; размер из NPDE при
   наличии, иначе PCIR; выравнивание 512).
3. Находит **PciAt** (code_type `0x00`) и **FWSEC** (`0xE0`).
4. В PciAt ищет **BIT** (`FF B8 'B' 'I' 'T' 00`), берёт токен **FALCON_DATA (0x70)**,
   читает 4-байтный указатель → `falcon_data_off = ptr - sizeof(PciAt)`.
5. В FWSEC-секции по этому смещению парсит **PmuLookupTable**, ищет запись
   **app id 0x85 (FWSEC PROD)** → `falcon_ucode_off = entry.data - sizeof(PciAt)`.
6. Читает дескриптор: **`FalconUCodeDescV3`** (44 байта) или V2 (60). Печатает
   `imem_load`, `dmem_load`, `signature_count`, `engine_id_mask`, `ucode_id`,
   смещение ucode. `--extract` пишет блоб ucode (imem||dmem) в файл.

Все смещения/структуры — из `vbios.rs` и `firmware.rs` (см. PORTING-MAP).

### Шаг 2 — движок Falcon — 🟡 CI / 📄 SRC (на железе не исполнялся) — `falcon_regs.h` + `driver/gsp/falcon.{h,c}`
`falcon_regs.h`: регистры PFALCON/PFALCON2, **база GSP `0x110000`/`0x111000`**
(подтверждено `falcon/gsp.rs`), биты CPUCTL/HWCFG2/DMACTL/ENGINE/DMATRFCMD/FBIF,
WPR2-границы, GFW boot, `NV_PGSP_QUEUE_HEAD`.

`driver/gsp/falcon.{h,c}` — поверх абстрактного MMIO+DMA (`nv_mmio_t`):
- `nv_falcon_reset_ga102`: reset_ready(≤150us, необяз.) → reset движка → ожидание
  скраба → `select_core`(Falcon) → ожидание скраба (повторно) → `FALCON_RM =
  PMC_BOOT_0`. (falcon.rs reset + ga102.rs reset_eng; 2-й scrub добавлен по аудиту 2026-06-28)
- `nv_falcon_select_core_ga102`: BCR_CTRL core_select=Falcon, ждать valid.
- `nv_falcon_program_brom_ga102`: BROM_PARAADDR/ENGIDMASK/CURR_UCODE_ID, MOD_SEL=RSA3K.
- `nv_falcon_dma_reset` + `nv_falcon_dma_wr` + `nv_falcon_dma_load_ga102`:
  FBIF→coherent sysmem/physical, цикл `DMATRFCMD` (256-байт блоки), IMEM(sec)+DMEM,
  BOOTVEC. (falcon.rs dma_wr/dma_load)
- `nv_falcon_start`/`wait_halted`/`boot`/mailbox.
- `nv_wpr2_is_set`, `nv_gfw_boot_completed` — проверки результата.

Компилируется в CI (portable C).

### Что в слое 2 ещё НЕ сделано (точные следующие шаги)
1. **Патч FWSEC-образа** — 🟡 CI / 📄 SRC (не прогонялся на реальном FWSEC)
   `driver/gsp/fwsec_patch.{h,c}` (порт `fwsec.rs`):
   парсер дескриптора (V2/V3), `nv_fwsec_patch_frts` (DMEMMAPPER init_cmd=FRTS +
   FrtsCmd с регионом WPR2), `nv_fwsec_select_signature_index` +
   `nv_fwsec_patch_signature` (по fuse-версии из `nv_falcon_signature_fuse_version_ga102`).
2. **Kernel DMA в kext** — выделить coherent-буфер (`IOBufferMemoryDescriptor`),
   скопировать пропатченный ucode, получить физ. адрес → `nv_falcon_dma_load_ga102`.
3. **Запуск FWSEC-FRTS** — `reset → dma_load → boot(mbox0=0) → mbox0==0 &&
   nv_wpr2_is_set()`. ← создан WPR2.
4. **Booter Loader** (порт `firmware/booter.rs`) → грузит GSP-RM в WPR2, старт RISC-V.
   - 🟢 **фундамент (фазы 1-2) сделан 2026-06-29**: шим загрузки/распаковки блобов
     `driver/gsp/fw_blob.h` + `tools/fw_blob_linux.c` (zstd); парсер контейнера
     `driver/gsp/booter.{h,c}` (`nv_booter_parse` + `nv_booter_select_signature`).
     Офлайн-проверено на `booter_load/booter_unload-535.113.01`.
   - 🟢 **фаза 3 (SEC2-путь) подтверждена на железе 2026-06-29** (`tools/booter_run_linux.c`,
     Linux/VFIO): база SEC2 (0x840000) верна (HWCFG2=0x67b7), reset+dma_load+BROM(подпись)+boot
     на SEC2 OK — Booter дошёл до halted, `mbox0=0x89` (ожидаемая ошибка «нет WPR meta»,
     dummy-handle). Доказательство: `docs/hw-dumps/20260629-rtx4070s-booter-sec2-dryload.log`.
   - 🟢 **фаза 4 ч.1 (офлайн) 2026-06-29**: `driver/gsp/elf64.{h,c}` (секция ELF по имени),
     `driver/gsp/gsp_fw.{h,c}` — извлечение `.fwimage`/`.fwsignature_ad10x` из ELF GSP-RM,
     разбор `RM_RISCV_UCODE_DESC` (bootloader), билдер radix3 (3×4K, u64). Проверено на
     реальных gsp/bootloader-535.113.01 (`make gsp-stage-test`): секции/desc/radix3 согласованы.
   - 🟢 **фаза 4 ч.2 (офлайн) 2026-06-29**: `nv_gsp_fb_layout` (раскрой WPR2: frts→boot→elf→
     heap→wpr2→non-WPR heap) + `nv_gsp_wpr_meta_build` (256-байтная `GspFwWprMeta`).
     Проверено офлайн на FB=12282 МиБ с HW-якорем FRTS (`make gsp-stage-test`): раскрой
     внутри FB, поля meta согласованы. Heap=126 МиБ (формула 535.113.01). Сверено с r535.
   - 🟢 **фаза 5 (офлайн) 2026-06-30**: libos init-args (`nv_gsp_libos_*`) + pte-array лог-буферов
     (`make gsp-stage-test`: id8/kind/loc/pa согласованы, сверено с OGK/r535).
   - 🟢🟢 **фаза 6 ПОДТВЕРЖДЕНА НА ЖЕЛЕЗЕ 2026-06-30 — МЕТРИКА ЗАДАЧИ 6 ДОСТИГНУТА**:
     оркестратор `tools/gsp_boot_linux.c` (Linux/VFIO) за один прогон: FWSEC-FRTS→WPR2 →
     staging GSP-RM (fwimage+radix3+bootloader+signature+WprMeta+libos) → reset GSP в RISC-V +
     GSP mbox=libos → SEC2 Booter с WprMeta → **Booter mbox0=0** → FALCON_OS=app_version →
     **GSP RISC-V active=1** (CPUCTL=0x80). GSP-RM реально исполняется на карте.
     Доказательство: `docs/hw-dumps/20260630-rtx4070s-gsp-rm-boot-OK.log`. Новые falcon-помощники:
     `nv_falcon_gsp_reset_riscv`, `nv_falcon_riscv_active`, `nv_falcon_write_os_version`.
5. **GSP-RM + очереди RPC** — задача 7: 🟢🟢 **РЕШЕНА НА ЖЕЛЕЗЕ 2026-06-30 — МЕТРИКА
   СЛОЯ 2 ДОСТИГНУТА**. Тот же оркестратор `tools/gsp_boot_linux.c` после бута: очереди
   cmdq/msgq + rmargs (`GSP_ARGUMENTS_CACHED`) → **`SET_SYSTEM_INFO`+`SET_REGISTRY` в cmdq
   ДО бута** (BAR-физадреса+BDF / 2 ключа) → дренаж msgq с исполнением
   **`GSP_RUN_CPU_SEQUENCER`** (интерпретатор I/O-команд + mid-init ресет GSP-ядра, CORE_RESUME
   = рестарт SEC2 Booter) → **`GSP_INIT_DONE` (0x1001) получен по RPC**.
   Доказательство: `docs/hw-dumps/20260630-rtx4070s-gsp-INIT_DONE-OK.log`.
   Новый код: `driver/gsp/gsp_rpc.{c,h}` (`nv_gsp_build_sysinfo/registry`, `nv_gsp_cmdq_write`),
   `exec_cpu_sequencer`/`seq_core_resume` в оркестраторе.
   **Полная тех-запись со всеми деталями и граблями: `docs/gsp-bringup-layer2.md`.**

---

## Слой 3 — память/GMMU через RPC — проход A 🟢 HW 2026-06-30

После `GSP_INIT_DONE` поднят **двусторонний RPC** к живому GSP-RM (послать команду
пост-бут, дождаться типизированного ответа). Новый портируемый модуль
`driver/gsp/gsp_rm.{c,h}` (порт nouveau `r535.c`), офлайн-тест `tools/gsp_rm_test.c`
(`make gsp-rm-test`: framing/checksum/multi-page/async-skip/парс/размеры).

- **Метрика №1 🟢 HW:** `GET_GSP_STATIC_INFO (65)` — GSP вернул `rpc_result=0` и
  **карту 5 FB-регионов VRAM** (вершина = 0x2ffa00000 = 12282 МиБ), внутренние хэндлы
  GSP (`hClient/hDevice/hSubdevice`), `bar1/bar2PdeBase`. Двусторонний RPC жив.
- **Метрика №2 🟢 HW:** `GSP_RM_ALLOC (103)` — цепочка `NV01_ROOT`→`NV01_DEVICE_0`→
  `NV20_SUBDEVICE_0`, каждый `status=NV_OK(0)` (handles 0xc1d00000/0xde1d0000/0x5d1d0000).
  Наш RM-клиент создан на GPU.
- Доказательство: `docs/hw-dumps/20260630-rtx4070s-layer3-rpc-OK.log` (тот же прогон
  `tools/gsp_boot_linux.c`, блок «СЛОЙ 3»; `msgq.writePtr` 7→11 — GSP ответил 4 раза).
- Смещения `GspStaticConfigInfo` (2168б; fbRegion@840, barPde@2088/2096, hInternal@2152+)
  получены compile-probe против SDK 535.113.01 + самопроверкой FB-карты на железе.

**Проход B 🟢 HW 2026-07-01 — память через RPC:**
- **B-метрика №1:** `GSP_RM_CONTROL (76)` / `FB_GET_INFO_V2 (0x20801303)` — `status=NV_OK`,
  реальный конфиг: RAM=12576768 KiB (=12282 МиБ, совпало с PMC), HEAP=12355136,
  HEAP_FREE=57720, BAR1=262144 KiB (=256 МиБ). Третий RPC-глагол работает.
- **B-метрика №2:** `GSP_RM_ALLOC` `FERMI_VASPACE_A (0x90f1)` — `status=NV_OK`,
  handle=0x90f10000. **GPU VA-пространство (корень GMMU) создано через RPC.**
- Новый код: `nv_gsp_rm_control`, `nv_gsp_fb_get_info`, `nv_gsp_rm_vaspace_ctor`
  в `driver/gsp/gsp_rm.{c,h}`; блок «3B» в оркестраторе. Доказательство:
  `docs/hw-dumps/20260701-rtx4070s-layer3-passB-OK.log` (`msgq.writePtr` 11→13).

**Проход C 🟢 HW 2026-07-01 — регистрация VRAM:**
- `ALLOC_MEMORY (4)` / `NV01_MEMORY_LIST_FBMEM (0x82)` — `rpc_result=NV_OK`.
  Зарегистрирован физ. VRAM-диапазон (1 МиБ @ phys=0x13100000, 256 страниц). В
  GSP-модели VRAM'ом владеет ГОСТЬ: даёт список физ. страниц (не GSP из кучи).
- **Грабли:** первый заход через `NV01_MEMORY_LOCAL_USER (0x40)` + heap-alloc GSP
  отверг (`rpc_result≠0`); правильно — memlist (порт `fbsr_memlist`). Код:
  `nv_gsp_rm_vram_memlist`. Доказательство:
  `docs/hw-dumps/20260701-rtx4070s-layer3-passC-vram-OK.log`.

**Проход D 🟢 HW 2026-07-14 — VRAM → GPU VA через ПРЯМОЙ GMMU:**
- Ранний RPC-заход `MAP_MEMORY_DMA (14)` через `NV01_MEMORY_VIRTUAL (0x70)` GSP
  **отверг** (`rpc_result=0x2a NV_ERR_INVALID_FUNCTION`) — это путь vGPU/SR-IOV, в
  GSP-offload page-tables ведёт CPU-side RM локально (nouveau тоже не шлёт этот RPC).
  Отрицательный лог: `docs/hw-dumps/gsp-boot-layer3-passD-20260714.log`. Код
  `nv_gsp_rm_map_memory_dma`/`nv_gsp_rm_vmem_ctor` остаются как док. отрицательный результат.
- **Реализован прямой GMMU** (порт nouveau `vmmgp100`/`r535_mmu_promote_vmm`):
  - `driver/gsp/gmmu.{c,h}` — PRAMIN-доступ CPU→VRAM (`NV_PBUS_BAR0_WINDOW=0x1700`,
    окно `BAR0+0x700000`), кодеры PTE/PDE Ada (ga10x/gp100), построение иерархии
    PD3→PD2→PD1→PD0→SPT + `nv_gmmu_map_range`. Офлайн-тест `tools/gmmu_test.c`.
  - `nv_gsp_rm_vaspace_copy_pdes` в `gsp_rm.c` — `RM_CONTROL COPY_SERVER_RESERVED_PDES
    (0x90f10106)` на vaspace: отдаёт GSP физ-адреса корней PD3/PD2/PD1. Структура (184б)
    сверена с `ctrl90f1.h` (compile-probe), значения `levels[]` — как `r535_mmu_promote_vmm`.
- **Результат HW** (`docs/hw-dumps/20260714-rtx4070s-layer3-passD-gmmu-OK.log`):
  `COPY_SERVER_RESERVED_PDES status=NV_OK`, read-back листовой PTE `MATCH`
  (`0x1310001` = `(0x13100000>>4)|VALID`). **VRAM в GPU VA-пространстве.**
- **Полная тех-запись: `docs/gsp-layer3-rpc.md`** §4D (§4D.3 — реализация).

---

---

## Слой 4 — каналы (command submission) — 🟢 ПРОХОД A НА ЖЕЛЕЗЕ 2026-07-14

Канал GPFIFO поднят через GSP-RM (порт nouveau `r535_chan_ramfc_write`). Модуль
`driver/gsp/gsp_fifo.{c,h}` + офлайн-тест `tools/gsp_fifo_test.c` (`make gsp-fifo-test`).
Тех-запись и план проходов A/B/C — `docs/gsp-layer4-fifo.md`. **Доказательство:**
`docs/hw-dumps/20260714-rtx4070s-layer4-passA-chan-OK.log`.

- **A0**: `FIFO_GET_DEVICE_INFO_TABLE` → 11 движков (GR0, COPY0..4, NVDEC/NVENC/SEC2/OFA/SW),
  CE0 engineType=0x9 runlist=0.
- **A1**: instance+RAMFC/USERD/method-buffer во VRAM за page-tables прохода D; кольцо
  GPFIFO в замапленном GPU-VA (`0x20000000`).
- **A2**: `channel_alloc` (`AMPERE_CHANNEL_GPFIFO_A`) + `BIND` + `GPFIFO_SCHEDULE` = `NV_OK`.
  Грабли: SCHEDULE-params ровно 2 байта (иначе `NV_ERR_INVALID_PARAM_STRUCT 0x3a`).
- **B 🟢 HW**: `nv_gsp_rm_ce_obj_alloc` — объект `AMPERE_DMA_COPY_B (0xC7B5)` на канале
  (`NVB0B5_ALLOCATION_PARAMETERS{version=1, engineType=CE0}`) → `status=NV_OK`, handle=0xce00.
  Пруф: `docs/hw-dumps/20260714-rtx4070s-layer4-passB-ceobj-OK.log`. Дальше C (pushbuffer+семафор).

- `nv_gsp_rm_channel_alloc` — `GSP_RM_ALLOC` класс `AMPERE_CHANNEL_GPFIFO_A (0xC56F)`
  под device, params `NV_CHANNEL_ALLOC_PARAMS` (360б, compile-probe): GPU-VA кольца
  GPFIFO (из прямого GMMU слоя 3), `hVASpace`, `engineType`, физ-дескрипторы
  instance/USERD/RAMFC/method-buffer (`NV_MEMORY_DESC_PARAMS`, addressSpace VIDMEM=2).
- `nv_gsp_rm_channel_bind` / `nv_gsp_rm_channel_schedule` — `RM_CONTROL`
  `NVA06F_CTRL_CMD_BIND (0xa06f0104)` / `GPFIFO_SCHEDULE (0xa06f0103)`.
- `nv_gsp_rm_engine_obj_alloc` — объект движка на канале (CE `AMPERE_DMA_COPY_B 0xC7B5`).
- Классы Ada сверены с `nvif/class.h`: канал 0xC56F, CE 0xC7B5, GR `ADA_A 0xC997`,
  compute `ADA_COMPUTE_A 0xC9C0`, usermode/doorbell `AMPERE_USERMODE_A 0xC561`.

**Подпроход A0 🟢 HW 2026-07-14** — `nv_gsp_fifo_get_device_info`
(`FIFO_GET_DEVICE_INFO_TABLE`, cmd `0x20801112`, params 3212б) на нашем subdevice:
прочитаны **11 движков** RTX 4070S — GR0(0x01,runl0), SEC2(0x30), NVENC(0x26),
NVDEC0(0x1d), **COPY0..COPY4(0x09..0x0d)**, OFA/NVJPEG(0x3d), SW(0x2c). **CE0
найден: engineType=0x9, runlist=0** — снят вопрос угадывания engineType для A2.
Доказательство: `docs/hw-dumps/20260714-rtx4070s-layer4-A0-devinfo-OK.log`.

**Статус:** A0 🟢 HW; framing alloc/bind/schedule 🔧 офлайн. **HW впереди:**
A1 (буферы instance/USERD/GPFIFO во VRAM + GPFIFO в GPU-VA прямым GMMU),
A2 (alloc+bind+schedule на железе = метрика прохода A).

---

## Аудит FWSEC-FRTS против nova-core (2026-06-28) — 📄 SRC

Построчная сверка слоя 2 (`driver/gsp/*`, `FwsecRun.cpp`) с актуальным master
ядра по скачанным raw-исходникам. Подробности и список сверенного — в
`PORTING-MAP.md` (раздел «Аудит против nova-core»). Итог:

- **Расхождения исправлены:** D1 — добавлено повторное ожидание скраба памяти
  после `select_core` в `nv_falcon_reset_ga102` (`falcon.c`); D2 — `FwsecRun.cpp`
  теперь явно отвергает дескриптор != V3.
- **Подтверждено верным без правок:** раскладки дескрипторов V2/V3, FRTS-патч,
  выбор подписи, различие смещений ucode (`+hdr_size`) и подписей (`+44`), все
  битовые поля regs.rs, расчёт FRTS-региона (fb_layout), локатор FWSEC.
- **К проверке на стенде (D3):** разбор IFR-заголовка (`NVGI`) не реализован — на
  Ada ожидаемо не нужен, но проверить по дампу ROM.

Статус по-прежнему 📄 SRC / 🟡 CI: код сверен с исходником, **на железе не
исполнялся**. Следующий шаг — задача 8 (HW-верификация на стенде),
runbook: `docs/bench-test-fwsec.md`.

## Инварианты (не нарушать, иначе «сходим с пути»)

1. Регистры/структуры/смещения — только из источников, со сверкой. Непроверенное —
   `TODO: verify`.
2. Прошивки (FWSEC из VBIOS, Booter, GSP-RM) — грузить как есть, не менять (кроме
   санкционированного патча FWSEC: init_cmd + signature, как в `fwsec.rs`).
3. Код примитивов — на абстрактном интерфейсе (`nv_mmio_t`), чтобы работал в
   kext/dext/тесте. Платформенная привязка (DMA-аллокатор, маппинг BAR) — снаружи.
4. Каждый шаг — проверяем headless (регистры, mailbox, WPR2), без монитора.
5. Целевой чип — Ada AD104 (`10DE:2783`), путь GA102-HAL. Hopper/Blackwell (FSP) —
   не наш кейс сейчас.

## Где что искать
- Стратегия: `docs/ARCHITECTURE.md`
- План слоя 2: `docs/gsp-bringup-notes.md`
- Соответствие исходникам: `docs/PORTING-MAP.md`
- Правила: `AGENTS.md`
