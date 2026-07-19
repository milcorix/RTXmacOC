# RTXmacOC

[![build-macos](https://github.com/milcorix/RTXmacOC/actions/workflows/build-macos.yml/badge.svg)](https://github.com/milcorix/RTXmacOC/actions/workflows/build-macos.yml)

**Open-source драйвер NVIDIA RTX для macOS (Hackintosh под OpenCore).**

Беремся за то, до чего никто так и не добрался — заставить современные карты
NVIDIA RTX (Ada Lovelace, например RTX 4070 Super) по-настоящему работать на
актуальных macOS, где официального драйвера NVIDIA нет с High Sierra.

Целевая платформа: **x86 Hackintosh под OpenCore**, RTX как основная карта.
Не про eGPU и не про Apple Silicon — см. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

---

## 📊 Прогресс

Драйвер строится слоями. **Важно (честно):** статус слоя помечен по доказательству —
🟢 ставится только при наличии лога с реального железа. На сегодня на реальной
RTX 4070 Super подтверждены: декод `PMC_BOOT_0` (слой 1), **весь слой 2 целиком** —
FWSEC-FRTS→WPR2, SEC2 Booter грузит GSP-RM, GSP RISC-V стартует, и GSP-RM по RPC
завершает инициализацию (**`GSP_INIT_DONE`**), а также **слой 3 (проходы A+B+C)** —
двусторонний RPC к живому GSP-RM: `GET_GSP_STATIC_INFO` возвращает карту FB-регионов
VRAM, `GSP_RM_ALLOC` создаёт цепочку RM client→device→subdevice, `RM_CONTROL`/
`FB_GET_INFO_V2` читает конфиг VRAM (RAM=12282 МиБ), `FERMI_VASPACE_A` создаёт
GPU-виртуальное адресное пространство (корень GMMU), `ALLOC_MEMORY`/`NV01_MEMORY_LIST_FBMEM`
регистрирует физический VRAM-диапазон, а **проход D (VRAM → GPU VA)** — через
**прямой GMMU-маппинг**: клиент сам строит page-tables во VRAM (PRAMIN) и отдаёт GSP
корневые PDE контролом `COPY_SERVER_RESERVED_PDES` (`status=NV_OK`, read-back PTE
совпал). Всё `NV_OK` (логи в [docs/hw-dumps/](docs/hw-dumps/)). Ранний RPC-заход
`MAP_MEMORY_DMA` был отвергнут GSP (`NV_ERR_INVALID_FUNCTION` — путь vGPU) и оставлен
как задокументированный отрицательный результат.

> **Статус вывода изображения (2026-07-19).** Слой 5 полностью замкнут на железе:
> GSP-modeset + window flip = **пиксели на мониторе** (RTX 4070S, 1280×1024@HDMI).
> Блокер был аппаратный (мусорный бит SET_STORAGE для класса C67E), а не
> политический. Путь в macOS: IOFramebuffer kext (`MilcorixFB`) + тот же GSP-core,
> среда = OpenCore + SIP off + AMFIPass (Tahoe Root Patch). Metal/ускорение —
> отдельный трек, требует обход Library Validation. Разбор замков:
> [docs/macos-graphics-stack.md](docs/macos-graphics-stack.md).

Легенда: 🟡 компилируется (CI) · 📄 совпадает с исходником · 🟢 проверено на железе.

| # | Слой | Статус |
|---|------|--------|
| 1 | **PCIe bring-up** — найти карту, смапить BAR0, прочитать chip ID (`PMC_BOOT_0`) | 🟢 декод подтверждён на железе (kext-загрузка ждёт macOS) |
| 2 | **GSP bring-up** — поднять GPU через GSP, наладить RPC | 🟢 **ЗАВЕРШЁН на железе** (Linux/VFIO): FWSEC-FRTS→WPR2→Booter→GSP RISC-V active → **`GSP_INIT_DONE` по RPC** |
| 3 | **Memory management (GMMU/VRAM)** через RPC | 🟢 **A+B+C+D на железе**: RPC/RM-цепочка; FB-control + VASPACE; VRAM memlist; **D — прямой GMMU** (page-tables во VRAM + `COPY_SERVER_RESERVED_PDES`, `status=NV_OK`). RPC `MAP_MEMORY_DMA` — тупик (vGPU-путь) |
| 4 | Command submission (каналы) | 🟢 **ЗАМКНУТ на железе**: канал `AMPERE_CHANNEL_GPFIFO_A` (A) + объект CE `AMPERE_DMA_COPY_B` (B) + **pushbuffer исполнен, host-семафор `0xcafe0001` (C) — первая команда GPU**. Весь путь submission работает |
| 5 | **Display / modeset** — вывод изображения | 🟢 **ЗАМКНУТ НА ЖЕЛЕЗЕ (2026-07-19)**: слои 5A→5C.4e все пройдены, **window flip + scanout = пиксели на мониторе** (1280×1024@HDMI, RTX 4070S). Фикс: SET_STORAGE бит MEMORY_LAYOUT убран для C67E (Ampere/Ada). Диагностика: SOR ARM active, vline растёт, SUPERVISOR=0. Осталось: интеграция в macOS kext (`MilcorixFB`). |
| 6 | 3D / compute (Metal) | ⛔ закрытый интерфейс |

### Подробнее по слоям

**Слой 1**
- `pcie_probe` — чтение config space/BAR из IORegistry (компилируется).
- kext `RTXProbe` / dext `RTXProbeDext` — матчинг `10DE:2783`, маппинг BAR0,
  чтение/декод `PMC_BOOT_0`. Компилируются в CI; **не загружались на macOS**.
- Декодер регистра **подтверждён на реальной RTX 4070 Super**: `PMC_BOOT_0 =
  0x194000A1` → Ada AD104, chipset `0x194`, rev `0xA1` (Windows/RW-Everything,
  [docs/hw-dumps/](docs/hw-dumps/)). Это первый результат, проверенный железом.

**Слой 2** — 🟢 **ЗАВЕРШЁН на железе** (RTX 4070 Super, Linux/VFIO, 2026-06-30):
полная цепочка `tools/gsp_boot_linux.c` за прогон — FWSEC-FRTS→WPR2, SEC2 Booter `mbox0=0`,
GSP RISC-V active, затем RPC-handshake: `SET_SYSTEM_INFO`+`SET_REGISTRY` в cmdq →
дренаж msgq с исполнением `GSP_RUN_CPU_SEQUENCER` (mid-init ресет GSP-ядра) →
**`GSP_INIT_DONE` получен** ([docs/hw-dumps/20260630-rtx4070s-gsp-INIT_DONE-OK.log](docs/hw-dumps/)).
Раньше (2026-06-29) подтверждён FWSEC-FRTS отдельно (`...-fwsec-frts-linux-OK.log`).
- `driver/gsp/{falcon,fwsec_patch,fwsec_locate,fb_layout}.*`, `falcon_regs.h` —
  примитивы Falcon (reset/DMA-load/boot), локатор и патч FWSEC, поиск региона FRTS.
  Сверены с nova-core, **проверены на железе**.
- `tools/fwsec_run_linux.c` — Linux/VFIO-хост тех же модулей (тот код, что и kext).
  `tools/run-fwsec-detached.sh` — прогон на единственной GPU без ребута (возвращает GUI).
- `driver/gsp/{booter,gsp_fw,gsp_rpc,elf64}.*` — парсер Booter (HsFirmwareV2),
  staging GSP-RM (radix3, `GspFwWprMeta`, libos-args), очереди RPC (cmdq/msgq,
  rmargs), `GspSystemInfo`/`SET_REGISTRY` и интерпретатор `GSP_RUN_CPU_SEQUENCER`.
  Сверены с nouveau `r535.c`, **проверены на железе**.
- `tools/gsp_boot_linux.c` + `tools/run-gsp-boot-detached.sh` — оркестратор всего
  слоя 2 за один прогон на единственной GPU без ребута (возвращает GUI).
- macOS-kext-шим `driver/RTXProbe/FwsecRun.*` (IOPCIDevice/IOBufferMemoryDescriptor) —
  всё ещё 🟡: те же модули, но на самой macOS не прогонялся.
- Слой 3 закрыт полностью (A+B+C+D): проход D сделан **прямым GMMU** (page-tables
  PDE3→PTE во VRAM + `COPY_SERVER_RESERVED_PDES`), т.к. `MAP_MEMORY_DMA` оказался
  тупиком (GSP отвергает fn=14, HW 2026-07-14). Дальше — **слой 4** (каналы).

---

## 🗂️ Структура

```
pcie_probe.c            слой 1: user-space разведка PCIe (IOKit)
ada_regs.h              регистры NVIDIA Ada (PMC_BOOT_0, декод чипа)
falcon_regs.h           регистры микроконтроллера Falcon (GSP)
driver/RTXProbe/        kext: матчинг + маппинг BAR0 + чтение PMC_BOOT_0
driver/RTXProbeDext/    dext (PCIDriverKit): то же через MemoryRead32
driver/gsp/*            слой 2: Falcon, FWSEC, FRTS, Booter, staging GSP-RM,
                        очереди RPC (cmdq/msgq), GspSystemInfo, CPU-секвенсер
driver/RTXProbe/FwsecRun.*  kext-оркестратор FWSEC-FRTS (те же gsp-модули)
tools/vbios_dump.c      чтение VBIOS + извлечение FWSEC — слой 2
tools/fwsec_run_linux.c Linux/VFIO-прогон FWSEC-FRTS на железе (слой 2, 🟢)
tools/gsp_boot_linux.c  Linux/VFIO-оркестратор всего слоя 2 → GSP_INIT_DONE (🟢)
tools/run-*-detached.sh  прогон на единственной GPU без ребута (возврат GUI)
tools/nv_mmio_linux.c   проверка PMC_BOOT_0 с реального железа из Linux
docs/                   архитектура, роадмап, конспект GSP, стенд
.github/workflows/      CI сборки на macOS
```

## 🔧 Сборка

Утилиты (user-space) и портируемые модули:
```sh
make probe          # pcie_probe (нужна macOS)
make vbios-dump     # tools/vbios_dump (любая ОС)
make mmio-linux     # tools/nv_mmio_linux (Linux)
```
Драйверы (kext/dext) собираются Apple-тулчейном (Xcode) — локально или в CI.
Нет macOS под рукой? Сборку гоняет [GitHub Actions](.github/workflows/build-macos.yml).

## 🧪 Целевой стенд

RTX 4070 Super (AD104, `10DE:2783`) на i5-12400F + Gigabyte B760M, без iGPU.
До готовности слоя 5 вывод на RTX в macOS — чёрный экран (драйвера дисплея ещё
нет), разработка идёт headless. Подробности — [docs/testbed.md](docs/testbed.md).

---

## 📚 Документы

- [Архитектура и стратегия](docs/ARCHITECTURE.md) — где стена и куда бьём (путь через GSP).
- [Графический стек macOS и блокер вывода](docs/macos-graphics-stack.md) — почему вывод картинки сторонним драйвером на Big Sur+ закрыт.
- [Bring-up GSP — слой 2 РЕШЁН](docs/gsp-bringup-layer2.md) — полная тех-запись: как GSP-RM доходит до `GSP_INIT_DONE` (смещения структур, опкоды секвенсера, грабли).
- [Слой 3 — двусторонний RPC (проходы A+B+C+D)](docs/gsp-layer3-rpc.md) — RM-цепочка, `FB_GET_INFO_V2`, `FERMI_VASPACE_A`, VRAM memlist и `MAP_MEMORY_DMA`.
- [Bring-up GSP (план)](docs/gsp-bringup-notes.md) — исходный план атаки по открытым исходникам.
- [Состояние реализации](docs/IMPLEMENTATION.md) — что и как уже сделано, следующие шаги.
- [Карта портирования](docs/PORTING-MAP.md) — соответствие наш код ↔ исходники nova-core.
- [Дорожная карта, Месяц 1](docs/ROADMAP_MONTH1.md).
- [AGENTS.md](AGENTS.md) — правила для контрибьюторов и AI-агентов.
- [HANDOFF](docs/HANDOFF.md) — сводка для нового агента/контрибьютора: где мы, что дальше.

## 🤝 Как присоединиться

Нужны люди с интересом к низкоуровневой разработке: C/C++/ObjC/Swift, PCIe/MMIO,
macOS internals (IOKit/DriverKit), архитектуры NVIDIA. Необязательно знать всё —
достаточно желания разбираться.

1. Зайди в [Discord](https://discord.gg/vxcgbx2R7) или [Telegram](https://t.me/pRIVETYA12).
2. Прочитай [AGENTS.md](AGENTS.md) и [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
3. Забирай задачу из роадмапа и врывайся.

## ⚠️ Дисклеймер

Проект исследовательский и некоммерческий. NVIDIA, RTX, macOS — товарные знаки
правообладателей; проект с ними не связан. Все сведения о регистрах и
последовательностях взяты из **публичных** источников (NVIDIA open-gpu-kernel-modules,
nouveau, nova-core) и пересказаны. Используешь на свой страх и риск.
