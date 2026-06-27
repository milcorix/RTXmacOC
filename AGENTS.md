# AGENTS.md — инструкция для агентов и контрибьюторов

Этот файл читают AI-агенты и люди перед работой над репозиторием. Коротко: что
за проект, как устроен, как собирать, какие правила. Держи его в актуальном виде.

## Что за проект

**RTXmacOC** — open-source драйвер NVIDIA RTX (Ada, AD104, RTX 4070 Super,
`10DE:2783`) для **x86 Hackintosh под OpenCore**, где RTX — основная и
единственная видеокарта. Конечная цель — полноценный вывод изображения (и дальше
ускорение). На современных macOS драйвера NVIDIA для RTX не существует — это и
есть стена, которую бьём.

Подробности стратегии — `docs/ARCHITECTURE.md`. План — `docs/ROADMAP_MONTH1.md`.

### Не-цели
- Не eGPU на настоящем Маке, не Apple Silicon.
- Не покупка второй видеокарты как часть продукта (RTX = единственная карта).

## Архитектура по слоям (где мы)

1. **PCIe bring-up** — найти карту, смапить BAR0, прочитать `PMC_BOOT_0`. ✅ сделано.
2. **GSP bring-up** — поднять GPU через GSP, наладить RPC. 🔨 текущий фокус.
   План: `docs/gsp-bringup-notes.md`.
3. Memory management (GMMU/VRAM).
4. Command submission (каналы).
5. Display / modeset — **вывод картинки**. Конечная видимая цель.
6. 3D/compute (Metal) — самое дальнее.

Важно: слой 5 (дисплей) невозможен без 2+3+4. Не прыгать вперёд — бить по слою 2.

## Структура репозитория

```
pcie_probe.c            слой 1: user-space разведка PCIe (IOKit)
ada_regs.h              определения регистров NVIDIA Ada (общие для всех)
driver/RTXProbe/        kext (основной канал доставки через OpenCore)
driver/RTXProbeDext/    dext (альтернатива, PCIDriverKit)
tools/                  вспомогательные утилиты (nv_mmio_linux, vbios_dump, CI-скрипты)
docs/                   архитектура, роадмап, конспекты (GSP), стенд, дампы
.github/workflows/      CI сборки на macOS-раннере
```

## Сборка и CI

- **Проба / утилиты (user-space):** `make probe` (нужна macOS для IOKit) или
  напрямую `clang`. Портируемые утилиты (`tools/vbios_dump.c`) собираются любым cc.
- **Драйверы (kext/dext):** только Apple-тулчейн (Xcode). Локально или в CI.
- **CI:** `.github/workflows/build-macos.yml` собирает на macOS-раннере
  (`tools/ci_build_macos.sh`): pcie_probe + kext + dext (compile-check). Пуш в
  любую ветку → запуск. dext-шаг сейчас non-blocking (iig на Xcode 26.5 допиливаем).
- **macOS под рукой нет?** Сборку гонять через CI (GitHub Actions, бесплатно для
  public). VPS с macOS в KVM — см. `docs/vps-macos-build.md`.

## Как проверять без железа (важно)

Машина разработки часто без доступной RTX (и без второй GPU для вывода). Поэтому:
- **Компиляция** — через CI на macOS-раннере.
- **Логика регистров** — сверять с открытыми исходниками (см. ниже) + читать с
  реального железа headless: Linux live-USB (`tools/nv_mmio_linux.c`), или
  RW-Everything на Windows (`docs/read-pmc-boot0-windows.md`).
- **Bring-up слоёв 2–5** — проверяется чтением регистров и mailbox/статусов, БЕЗ
  монитора. Это снимает требование второй видеокарты на dev-машине.

## Правила работы (соблюдать)

1. **Регистры и битовые поля — только с подтверждением по источнику.** Любое
   новое определение в `ada_regs.h` сопровождать ссылкой на открытый источник
   (open-gpu-kernel-modules / nouveau / nova-core). Непроверенные раскладки
   помечать `TODO: verify`. Пример: `PMC_BOOT_0` сверен с nova-core (AD104=0x194).
2. **Прошивки NVIDIA (Booter, GSP-RM) — грузить как есть, не модифицировать**
   (подписаны). FWSEC берётся из VBIOS самой карты. Вопрос распространения, не
   реверса — протокол открыт, блоб чёрный ящик.
3. **Атрибуция источников.** Пересказывать, не копировать дословно (<30 слов),
   давать ссылку. Помечать «пересказано для соответствия лицензии».
4. **Язык.** Комментарии и доки — на русском (как в проекте). Идентификаторы в коде — английские.
5. **Стиль кода.** Чисто, с комментариями «зачем», а не «что». C/C++ как в
   существующих файлах. Не тащить лишние зависимости — только системные фреймворки.
6. **Не ломать сборку.** После правок драйверов — прогнать CI (пуш) и убедиться,
   что зелёное (kext обязателен).
7. **Безопасность.** Не коммитить секреты/пароли. Не лить чужие kernel-блобы без
   понимания лицензии.

## Целевое железо (эталонный стенд)

- GPU: RTX 4070 Super, AD104, `10DE:2783`, rev `0xA1`, subsystem `1569:F302`.
- BAR0 (регистры) = 16 MB; на этой машине из Windows виден как `0x52000000`.
- Хост: i5-12400F (без iGPU!), Gigabyte B760M DS3H DDR4, 32 ГБ.
  Нет iGPU → во время macOS вывод с RTX, дисплей-драйвера пока нет → чёрный экран
  ожидаем до слоя 5; разработка идёт headless.

Детали — `docs/testbed.md`, `docs/hw-dumps/`.

## Текущий статус (обновлять)

- Слой 1: ✅ код (`driver/RTXProbe`) + декодер сверен с ядром. CI зелёный.
- Слой 2: 🔨 план в `docs/gsp-bringup-notes.md`. Шаг 1 (VBIOS reader) ✅ —
  `tools/vbios_dump.c` локализует/извлекает FWSEC ucode. Шаг 2 (Falcon engine) ✅ —
  `driver/gsp/falcon.{h,c}` + `falcon_regs.h`: reset(GA102)/select_core/program_brom/
  DMA-load IMEM+DMEM/start/boot/mailbox/WPR2/GFW, сверено с nova-core. Осталось:
  патч FWSEC-образа (DMEMMAPPER init_cmd=FRTS + подпись) + kernel DMA-буфер в kext,
  затем запуск FWSEC-FRTS → Booter → GSP-RM → очереди RPC.
- Дальше по `docs/gsp-bringup-notes.md` §7.

## Ключевые источники (референс-база)

- NVIDIA open-gpu-kernel-modules — https://github.com/NVIDIA/open-gpu-kernel-modules
- nova-core (Rust, чистый разбор) — https://docs.kernel.org/gpu/nova/core/
- drm/nouveau (GSP/RPC) — https://docs.kernel.org/gpu/nouveau.html
