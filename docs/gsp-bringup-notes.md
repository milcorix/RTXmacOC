# Bring-up GSP (слой 2) — конспект и план атаки

> Цель документа: разобрать, как поднять GPU Ada до состояния «GSP жив и отвечает
> по RPC», по **публичным** исходникам (NVIDIA open-gpu-kernel-modules, драйверы
> Linux nouveau и nova-core). Это прямое продолжение слоя 1 (чтение PMC_BOOT_0).
>
> Содержимое источников пересказано и сведено для соответствия лицензиям.
> Источники указаны ссылками — это и есть наша референс-база для портирования.

---

## 0. Зачем это, и почему именно GSP

Начиная с Turing, низкоуровневую инициализацию GPU выполняет встроенный
процессор **GSP (GPU System Processor)**, исполняющий подписанную NVIDIA прошивку
(GSP-RM). Драйвер не программирует железо регистр-за-регистром — он **поднимает
GSP и дальше общается с ним по RPC**. Память, каналы команд, дисплей — всё это
становится последовательностью RPC к GSP-RM.

Стратегический вывод для нас: **после того как GSP отвечает, остальные слои (3–5,
включая вывод картинки) — это «послать правильные RPC», а не слепой реверс.**
Поэтому слой 2 — ключ ко всему.

Сам механизм GSP **не зависит от ОС**: это MMIO через BAR0 + общая память
(DMA-буферы в RAM) под очереди сообщений. Порт на macOS = переписать загрузку
Falcon + очереди + RPC поверх `IOPCIDevice` (MMIO) и DMA-дескрипторов DriverKit/
kext, вместо Linux-примитивов. Протокол тот же.

---

## 1. Что происходит ДО загрузки нашего драйвера

После сброса GPU, ещё до драйвера, на GSP в режиме **Heavy-secure (HS)** уже
исполняется **FWSEC** — приложение из VBIOS ROM карты. Оно:
- проверяет прошивки (secure boot, SB) и грузит ucode на другие микроконтроллеры;
- выполняет **devinit**;
- инициализирует память: скраббинг VRAM / ECC, VPR (Video Protected Region).

Источник: [Linux kernel — FWSEC](https://docs.kernel.org/gpu/nova/core/fwsec.html).

Важно: на потребительских картах FWSEC скраббит лишь часть VRAM, остальное —
**async-скраббинг**; до его завершения нескраббленная VRAM недоступна под
аллокации. Это надо учитывать в слое 3.

---

## 2. Микроконтроллеры Falcon — базовый примитив

GSP/SEC2/PMU — это **Falcon** (FAst Logic Controller), малые RISC-подобные ядра;
современные умеют и RISC-V. У каждого:
- раздельные **IMEM/DMEM** (память кода и данных),
- DMA-движок через **FBIF** (Frame Buffer Interface) для загрузки кода из sysmem,
- уровни привилегий: Heavy-secure / Light-secure / Non-secure.

Что драйвер обязан уметь с Falcon (это наши первые примитивы слоя 2):
1. **reset** Falcon,
2. сконфигурировать,
3. **DMA-загрузить** ucode (IMEM/DMEM) из памяти,
4. **стартовать CPU** ядра.

Источник: [Linux kernel — Falcon](https://docs.kernel.org/gpu/nova/core/falcon.html).

---

## 3. Последовательность bring-up для Ada (FWSEC-FRTS + Booter)

Для Turing/Ampere/**Ada** путь такой (Hopper/Blackwell — другой, через FSP, см. §6):

1. **Идентификация чипа** — читаем `PMC_BOOT_0` (это уже сделано, слой 1):
   подтверждаем Ada AD104, выбираем нужные прошивки.
2. **Достать VBIOS** карты и найти в нём раздел FWSEC (app id `0x85`).
   VBIOS читается из ROM-региона карты (PCI expansion ROM / PROM).
   Структура: таблица `PmuLookupTable` → запись FWSEC → `FalconUCodeDescV3`
   (заголовки + IMEM + DMEM + RSA-3K подписи).
3. **FWSEC-FRTS**: через интерфейс **DMEMMAPPER** в FWSEC выполнить команду
   **FRTS (Firmware Runtime Services)**. Она **вырезает регион WPR2**
   (Write-Protected Region 2) в VRAM — туда ляжет прошивка GSP. Доступ к WPR2
   только из HS-ucode.
4. **Booter Loader** (подписанный ucode NVIDIA) грузим на Falcon → он загружает
   **GSP-RM firmware** (`gsp_*.bin`) в WPR2 и **стартует GSP в RISC-V режиме**.
5. **GSP-RM** загрузился и исполняется.
6. **Очереди сообщений** в общей sysmem (DMA): **command queue** (SW→GSP RPC) и
   **status queue** (GSP→SW статус). Драйвер шлёт RPC в command queue, читает
   ответ из status queue.

Источники:
- FWSEC/FRTS/WPR2: [kernel FWSEC](https://docs.kernel.org/gpu/nova/core/fwsec.html),
  [патч nova-core](https://www.spinics.net/lists/dri-devel/msg551975.html)
  («FWSEC-FRTS создаёт защищённый регион WPR2, Booter Loader грузит и стартует
  прошивку GSP в RISC-V» — пересказ).
- Очереди RPC: [drm/nouveau](https://docs.kernel.org/gpu/nouveau.html),
  `open-gpu-kernel-modules` → `src/nvidia/inc/kernel/gpu/gsp/message_queue_priv.h`.

---

## 4. Какие прошивки нужны (и риск)

| Блоб | Откуда | Подписан | Заметка |
|---|---|---|---|
| **FWSEC** | из **VBIOS самой карты** (ROM) | да | уже на карте, читаем |
| **Booter Loader/Unloader** | пакет драйвера NVIDIA / linux-firmware | да | для Ampere+ |
| **GSP-RM** (`gsp_ad10x`/`ga10x`) | linux-firmware / драйвер NVIDIA | да | большой блоб |

Риск (честно): Booter и GSP-RM — подписанные NVIDIA блобы. Их надо где-то взять
(linux-firmware распространяет под лицензией NVIDIA) и **загрузить как есть** —
менять нельзя (подпись). Для нас это вопрос распространения, не реверса:
протокол открыт, прошивка — чёрный ящик, который мы просто грузим.

---

## 5. Открытые референсы для портирования

- **NVIDIA open-gpu-kernel-modules** (ветка `535`+): структуры RPC, message queue,
  путь инициализации. `src/nvidia/...`, `message_queue_priv.h`.
- **nouveau** (`drivers/gpu/drm/nouveau/nvkm/subdev/gsp/r535.c`) — **открытая**
  реализация загрузки GSP-RM. Самый ценный референс: видно реальные шаги
  (`r535_gsp_cmdq_get`, `r535_gsp_rpc_*`).
- **nova-core** (Rust, `drivers/gpu/nova-core/`) — современный чистый разбор:
  модули `falcon`, `fwsec`, `gsp`, `vbios`. Хорошо читается как спецификация.

Это не код под macOS — это **описание протокола**. Наша работа: переписать его
поверх IOKit/DriverKit.

---

## 6. Hopper/Blackwell (на будущее, не Ada)

Новейшие чипы грузятся через **FSP (Foundation Security Processor)** — корень
доверия в ROM: `FSP → FMC (Falcon Microcontroller) → GSP-RM`. Для Ada это не
актуально (у нас FWSEC-FRTS + Booter). Учтём, если проект дойдёт до Blackwell.
Источник: [kernel — FSP](https://dri.freedesktop.org/docs/drm/gpu/nova/core/fsp.html).

---

## 7. План атаки на слой 2 (наши шаги)

Каждый шаг проверяется **headless** — чтением регистров и mailbox’ов GSP, без
монитора. Это снимает проблему отсутствия второй видеокарты на dev-машине.

1. **VBIOS reader** — прочитать ROM карты, распарсить `PmuLookupTable`, найти и
   извлечь FWSEC (app id 0x85) и его `FalconUCodeDescV3`. ✅ **сделано** —
   `tools/vbios_dump.c`: образы PCI ROM → BIT (token 0x70) → falcon data ptr →
   PmuLookupTable (app 0x85) → дескриптор Falcon ucode (V2/V3), с `--extract`.
   _Проверка:_ на дампе VBIOS карты печатает imem/dmem size, signature_count,
   смещение ucode; извлекает блоб.
2. **Falcon primitives** — reset / config / DMA-load IMEM+DMEM / start, поверх
   BAR0 MMIO. ✅ **движок готов**: `driver/gsp/falcon.{h,c}` — reset (GA102/Ada:
   reset_ready→engine reset→scrub→select_core→FALCON_RM), `select_core`,
   `program_brom` (RSA3K), DMA-load IMEM(sec)+DMEM (DMATRFCMD-цикл), start/boot/
   mailbox, проверка WPR2/GFW. Регистры `falcon_regs.h`, всё сверено с nova-core.
   Осталось для запуска FWSEC: пропатчить образ (DMEMMAPPER init_cmd=FRTS +
   подпись) и выделить kernel DMA-буфер в kext.
   _Проверка:_ контролируемый ucode стартует, mailbox меняет значение.
3. **FWSEC-FRTS** — выполнить FRTS через DMEMMAPPER, получить **WPR2**.
   _Проверка:_ регистры WPR показывают выделенный регион.
4. **Booter + GSP-RM** — загрузить Booter, тот поднимает GSP-RM в WPR2, старт
   RISC-V.
   _Проверка:_ статус-регистры GSP = «running», нет fault.
5. **Message queues + первый RPC** — выделить DMA-память под command/status
   очереди, инициализировать, послать первый RPC (например, version/handshake),
   получить ответ.
   _Проверка / **метрика слоя 2**:_ GSP-RM ответил на RPC. ← стена слоя 2 пробита.

После п.5 открывается путь к слоям 3–5 (память, каналы, дисплей) как к набору
RPC к живому GSP-RM.

---

## 8. Связь с нашим кодом

- Слой 1 (`driver/RTXProbe`, `ada_regs.h`) уже даёт: маппинг BAR0 и чтение
  идентификации. Это фундамент для §3.1.
- Следующий артефакт кода — **VBIOS reader** (§7.1): можно начать как user-space
  утилиту (по аналогии с `pcie_probe`), читающую ROM карты и парсящую FWSEC.
  Реверса нет — структуры описаны в nova-core `vbios.rs`.
- Регистры Falcon/GSP — заносить в `ada_regs.h` по мере разбора, со ссылкой на
  источник, как и `PMC_BOOT_0`.

---

## Источники (референс-база)

- NVIDIA open-gpu-kernel-modules — https://github.com/NVIDIA/open-gpu-kernel-modules
- Linux kernel, nova-core FWSEC — https://docs.kernel.org/gpu/nova/core/fwsec.html
- Linux kernel, nova-core Falcon — https://docs.kernel.org/gpu/nova/core/falcon.html
- Linux kernel, nova-core FSP — https://dri.freedesktop.org/docs/drm/gpu/nova/core/fsp.html
- Linux kernel, drm/nouveau (GSP RPC очереди) — https://docs.kernel.org/gpu/nouveau.html
- nova-core unbind/booter патч — https://www.spinics.net/lists/dri-devel/msg551975.html

_Все источники публичные; содержимое пересказано для соответствия лицензионным
ограничениям._
