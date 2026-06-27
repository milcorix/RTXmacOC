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

Драйвер строится слоями. Картинку на экране (слой 5) нельзя сделать без
фундамента (слои 2–4), поэтому идём снизу вверх.

| # | Слой | Статус |
|---|------|--------|
| 1 | **PCIe bring-up** — найти карту, смапить BAR0, прочитать chip ID (`PMC_BOOT_0`) | ✅ готово |
| 2 | **GSP bring-up** — поднять GPU через GSP, наладить RPC | 🔨 в работе |
| 3 | Memory management (GMMU/VRAM) | ⏳ |
| 4 | Command submission (каналы) | ⏳ |
| 5 | **Display / modeset** — вывод изображения | ⏳ цель |
| 6 | 3D / compute (Metal) | ⏳ |

### Что уже работает

**Слой 1 — ✅**
- `pcie_probe` находит карту на PCIe и читает config space (vendor/device/BAR/...).
- Драйверы `RTXProbe` (**kext**) и `RTXProbeDext` (**dext**) матчатся на `10DE:2783`,
  мапят BAR0 и читают/декодят `PMC_BOOT_0` → распознают **Ada AD104**.
- Декодер регистра сверен с исходниками ядра Linux (nova-core): chipset `0x194`.
- Сборка kext/dext проверяется в CI на macOS-раннере (бейдж выше).

**Слой 2 — 🔨 в работе**
- `tools/vbios_dump` — читает VBIOS карты, проходит образы PCI ROM и извлекает
  **FWSEC** ucode (первый кирпич GSP-инициализации). Структуры из nova-core.
- `driver/gsp/falcon.{h,c}` + `falcon_regs.h` — примитивы микроконтроллера
  **Falcon GSP**: reset, ожидание скраба памяти, start/boot, mailbox, проверка
  WPR2 и прогресса GFW boot.
- Дальше: DMA-загрузка ucode + BROM → запуск FWSEC-FRTS → Booter → GSP-RM →
  очереди RPC. План атаки: [docs/gsp-bringup-notes.md](docs/gsp-bringup-notes.md).

> Метрика, к которой идём в слое 2: **GSP отвечает на первый RPC**. После этого
> память/каналы/дисплей становятся последовательностью RPC к живому GSP-RM.

---

## 🗂️ Структура

```
pcie_probe.c            слой 1: user-space разведка PCIe (IOKit)
ada_regs.h              регистры NVIDIA Ada (PMC_BOOT_0, декод чипа)
falcon_regs.h           регистры микроконтроллера Falcon (GSP)
driver/RTXProbe/        kext: матчинг + маппинг BAR0 + чтение PMC_BOOT_0
driver/RTXProbeDext/    dext (PCIDriverKit): то же через MemoryRead32
driver/gsp/falcon.*     примитивы Falcon (reset/boot/mailbox) — слой 2
tools/vbios_dump.c      чтение VBIOS + извлечение FWSEC — слой 2
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
- [Bring-up GSP (слой 2)](docs/gsp-bringup-notes.md) — план атаки по открытым исходникам.
- [Дорожная карта, Месяц 1](docs/ROADMAP_MONTH1.md).
- [AGENTS.md](AGENTS.md) — правила для контрибьюторов и AI-агентов.

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
