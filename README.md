# RTXmacOC

[![build-macos](https://github.com/prd1324/RTXmacOC/actions/workflows/build-macos.yml/badge.svg)](https://github.com/prd1324/RTXmacOC/actions/workflows/build-macos.yml)

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
RTX 4070 Super подтверждены: декод `PMC_BOOT_0` (слой 1) и **полный путь
FWSEC-FRTS слоя 2** — Falcon-сброс, DMA-загрузка ucode, boot и создание региона
WPR2 в VRAM (`mbox0=0`, `WPR2 set=1`, лог в [docs/hw-dumps/](docs/hw-dumps/)).
Остальной код компилируется в CI и сверен с nova-core, но на железе ещё не исполнялся.

> ⚠️ **Блокер вывода изображения.** В современном macOS (Big Sur+) **нет публичной
> точки расширения** для стороннего kext/dext как полноценного GPU-акселератора
> для WindowServer: мешают library validation, приватные Metal/IOAccelerator-
> интерфейсы и то, что DriverKit не поддерживает графику. То есть слой 5 (вывод
> картинки) сторонним драйвером на актуальной macOS **заблокирован политикой
> Apple**, а не нашим кодом. Разбор и источники — [docs/macos-graphics-stack.md](docs/macos-graphics-stack.md).
> Направление проекта в свете этого пересматривается.

Легенда: 🟡 компилируется (CI) · 📄 совпадает с исходником · 🟢 проверено на железе.

| # | Слой | Статус |
|---|------|--------|
| 1 | **PCIe bring-up** — найти карту, смапить BAR0, прочитать chip ID (`PMC_BOOT_0`) | 🟢 декод подтверждён на железе (kext-загрузка ждёт macOS) |
| 2 | **GSP bring-up** — поднять GPU через GSP, наладить RPC | 🟢 FWSEC-FRTS→WPR2 подтверждён на железе (Linux/VFIO); Booter+RPC ещё не начаты |
| 3 | Memory management (GMMU/VRAM) | ⏳ |
| 4 | Command submission (каналы) | ⏳ |
| 5 | **Display / modeset** — вывод изображения | ⛔ заблокирован моделью Apple (см. выше) |
| 6 | 3D / compute (Metal) | ⛔ закрытый интерфейс |

### Что есть в коде (но НЕ проверено на железе)

**Слой 1**
- `pcie_probe` — чтение config space/BAR из IORegistry (компилируется).
- kext `RTXProbe` / dext `RTXProbeDext` — матчинг `10DE:2783`, маппинг BAR0,
  чтение/декод `PMC_BOOT_0`. Компилируются в CI; **не загружались на macOS**.
- Декодер регистра **подтверждён на реальной RTX 4070 Super**: `PMC_BOOT_0 =
  0x194000A1` → Ada AD104, chipset `0x194`, rev `0xA1` (Windows/RW-Everything,
  [docs/hw-dumps/](docs/hw-dumps/)). Это первый результат, проверенный железом.

**Слой 2** — 🟢 **подтверждён на железе** (RTX 4070 Super, Linux/VFIO, 2026-06-29):
FWSEC-FRTS отработал, `mbox0=0`, `WPR2 set=1 [0x2ff800000..0x2ff8e0000]`
([docs/hw-dumps/20260629-rtx4070s-fwsec-frts-linux-OK.log](docs/hw-dumps/)).
- `driver/gsp/{falcon,fwsec_patch,fwsec_locate,fb_layout}.*`, `falcon_regs.h` —
  примитивы Falcon (reset/DMA-load/boot), локатор и патч FWSEC, поиск региона FRTS.
  Сверены с nova-core, **проверены на железе**.
- `tools/fwsec_run_linux.c` — Linux/VFIO-хост тех же модулей (тот код, что и kext).
  `tools/run-fwsec-detached.sh` — прогон на единственной GPU без ребута (возвращает GUI).
- macOS-kext-шим `driver/RTXProbe/FwsecRun.*` (IOPCIDevice/IOBufferMemoryDescriptor) —
  всё ещё 🟡: те же модули, но на самой macOS не прогонялся.
- Дальше: Booter → GSP-RM → очереди RPC. План: [docs/gsp-bringup-notes.md](docs/gsp-bringup-notes.md).

---

## 🗂️ Структура

```
pcie_probe.c            слой 1: user-space разведка PCIe (IOKit)
ada_regs.h              регистры NVIDIA Ada (PMC_BOOT_0, декод чипа)
falcon_regs.h           регистры микроконтроллера Falcon (GSP)
driver/RTXProbe/        kext: матчинг + маппинг BAR0 + чтение PMC_BOOT_0
driver/RTXProbeDext/    dext (PCIDriverKit): то же через MemoryRead32
driver/gsp/*            слой 2: Falcon (reset/boot/DMA), локатор+патч FWSEC, расчёт FRTS
driver/RTXProbe/FwsecRun.*  kext-оркестратор FWSEC-FRTS (те же gsp-модули)
tools/vbios_dump.c      чтение VBIOS + извлечение FWSEC — слой 2
tools/fwsec_run_linux.c Linux/VFIO-прогон FWSEC-FRTS на железе (слой 2, 🟢)
tools/run-fwsec-detached.sh  прогон на единственной GPU без ребута (возврат GUI)
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
- [Bring-up GSP (слой 2)](docs/gsp-bringup-notes.md) — план атаки по открытым исходникам.
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
