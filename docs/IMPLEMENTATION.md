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
| 2 | GSP bring-up (FWSEC-FRTS→WPR2) | 🟢 HW 2026-06-29 (Linux/VFIO: mbox0=0, WPR2 set) для `driver/gsp/*`; macOS-kext-шим `FwsecRun.cpp` — 🟡 CI | `tools/{vbios_dump,fwsec_run_linux}.c`, `falcon_regs.h`, `driver/gsp/*`, `driver/RTXProbe/FwsecRun.*` |
| 3–6 | память/каналы/дисплей/Metal | ⏳ (дисплей — заблокирован Apple, см. graphics-stack) | — |

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
     `driver/gsp/booter.{h,c}` (`nv_booter_parse`). Офлайн-проверено на реальных
     `booter_load/booter_unload-535.113.01`: контейнер разобран, инварианты выполнены
     (`make booter-parse-test`). Смещения — `PORTING-MAP.md`.
   - ⏳ НЕ сделано (фазы 3-6): regs SEC2 + запуск Booter на SEC2; `GspFwWprMeta`;
     radix3-маппинг GSP-RM; libos; оркестрация (boot GSP→Booter→RISC-V active).
5. **GSP-RM + очереди RPC** (`open-gpu-kernel-modules` `message_queue_priv.h`,
   nouveau `r535.c`) → первый RPC. ← **метрика слоя 2** (задача 7).

---

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
