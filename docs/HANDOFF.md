# HANDOFF — сводка для следующего агента

> Самодостаточная выжимка, чтобы продолжить проект без чтения всей истории.
> Сначала прочитай: этот файл → `AGENTS.md` → `docs/IMPLEMENTATION.md` →
> `docs/PORTING-MAP.md` → спека `.kiro/specs/rtx-tahoe-full-support/`.

## Цель проекта (6 мес.)

NVIDIA RTX (Ada AD104, RTX 4070 Super, `10DE:2783`) должна работать в **macOS
Tahoe на Hackintosh (OpenCore)** так же полноценно, как AMD: детект, init, дисплей,
ускорение, Metal, перформанс, без хрупких обходов. **ИЛИ** — документированно
доказать технически (логами/дизасмом), что именно блокирует Apple, и испытать
обход. Цель бинарна. Полная спека: `.kiro/specs/rtx-tahoe-full-support/`.

## Жёсткий блокер (доказан, Неделя 2)

На Big Sur+ **нет публичной точки расширения** для стороннего GPU-акселератора в
WindowServer: (1) library validation не пускает чужой Metal-бандл в процессы Apple;
(2) Metal-driver ABI закрыт; (3) DriverKit не поддерживает графику. → Слои 5–6
(дисплей/Metal) штатно недостижимы; обход — `amfi_get_out_of_my_way`/AMFIPass +
реверс Metal-ABI (нестабильно). Детали: `docs/macos-graphics-stack.md`.

## Где мы (слои)

| Слой | Что | Статус |
|---|---|---|
| 1 PCIe bring-up | найти карту, BAR0, `PMC_BOOT_0` | 🟢 декод подтверждён железом; kext-загрузка ждёт стенда |
| 2 GSP bring-up | FWSEC-FRTS→WPR2→Booter→GSP-RM→RPC | 🟢 **СЛОЙ 2 ЗАВЕРШЁН НА ЖЕЛЕЗЕ 2026-06-30**: задача 6 (FWSEC-FRTS→WPR2, Booter mbox0=0, GSP RISC-V active) И задача 7 (**`GSP_INIT_DONE` получен по RPC!**). Полный bring-up: SET_SYSTEM_INFO+SET_REGISTRY в cmdq → Booter → дренаж msgq с исполнением `GSP_RUN_CPU_SEQUENCER` (mid-init ресет GSP-ядра) → `GSP_INIT_DONE (0x1001)`. Доказательство: `docs/hw-dumps/20260630-rtx4070s-gsp-INIT_DONE-OK.log`. macOS-kext-шим 🟡 |
| 3 память (GMMU) | VRAM через RPC | 🟢 **ЗАКРЫТ A+B+C+D НА ЖЕЛЕЗЕ** (2026-06-30..07-14): A — двусторонний RPC + `GET_GSP_STATIC_INFO` + RM-цепочка; B — `RM_CONTROL`/`FB_GET_INFO_V2` + `FERMI_VASPACE_A`; C — `ALLOC_MEMORY`/`NV01_MEMORY_LIST_FBMEM` (1 МиБ); **D — прямой GMMU**: клиент строит page-tables во VRAM (PRAMIN, `driver/gsp/gmmu.*`) + `RM_CONTROL COPY_SERVER_RESERVED_PDES (0x90f10106)` (`nv_gsp_rm_vaspace_copy_pdes`, порт `r535_mmu_promote_vmm`) → `status=NV_OK`, read-back PTE `MATCH`. Ранний RPC `MAP_MEMORY_DMA (14)` был отвергнут (`NV_ERR_INVALID_FUNCTION`, vGPU-путь) — оставлен как отрицательный результат. Пруфы: `docs/hw-dumps/20260714-rtx4070s-layer3-passD-gmmu-OK.log` (D 🟢), `...-passD-20260714.log` (D 🔴 RPC). Код `driver/gsp/{gsp_rm,gmmu}.*`, тех-запись `docs/gsp-layer3-rpc.md` §4D. |
| 4 каналы | command submission | 🔧 **framing + офлайн 2026-07-15**: `driver/gsp/gsp_fifo.*` — channel alloc (`AMPERE_CHANNEL_GPFIFO_A 0xC56F`) + BIND/SCHEDULE (`NVA06F_CTRL_*`), `NV_CHANNEL_ALLOC_PARAMS` (360б, compile-probe), классы Ada сверены с `nvif/class.h`. Порт nouveau `r535_chan_ramfc_write`. HW впереди (device-info → буферы во VRAM + GPFIFO в GPU-VA → alloc+bind+schedule). Тех-запись `docs/gsp-layer4-fifo.md`. |
| 5 дисплей | IOFramebuffer/WindowServer | ⛔ замок Apple |
| 6 Metal | IOAccelerator/Metal bundle | ⛔ замок Apple |

## Что подтверждено НА ЖЕЛЕЗЕ (🟢)

Через Windows + RW-Everything на целевой RTX 4070 Super (`docs/hw-dumps/20260628-*`):
- BAR0 = `0x52000000` (16 MB).
- `NV_PMC_BOOT_0` (BAR0+0x0) = **`0x194000A1`** → Ada, chipset `0x194` (AD104), rev `0xA1`.
- WPR2-регистры (`0x521FA824/828`) читаемы; `HI=0` → WPR2 в простое не активен
  (наш `nv_wpr2_is_set` корректен).

Через Linux/VFIO на той же RTX 4070 Super (`docs/hw-dumps/20260629-*`, headless, без ребута):
- `PMC_BOOT_0=0x194000A1` подтверждён и из Linux; VRAM usable = 12282 MiB.
- **FWSEC-FRTS отработал: mbox0=0, WPR2 set=1 [0x2ff800000..0x2ff8e0000]** — это первый 🟢 слоя 2.
- FWSEC desc V3 @vbios+0x45818 (engine GSP=0x400, ucode_id=9, sig_count=2, fuse_ver=1).
- Три фикса, найденные/подтверждённые на железе (см. PORTING-MAP «аудит 2026-06-29»):
  1) **ожидание GFW boot** после FLR (иначе FB-регистры читаются как priv-фолт `0xBADFxxxx`);
  2) **сигнатура образа `0x4E56`** ("NV") для NVIDIA-расширенных образов (FWSEC code_type 0xE0) —
     без неё `walk_images` останавливался на EFI и FWSEC-образ не находился (rc=NOIMG);
  3) дамп PROM + точный код ошибки локатора (диагностика).
- Стенд-метод без ребута: `tools/run-fwsec-detached.sh` (systemd-run гасит gdm → отцепляет
  fbcon → vfio → харнесс → возвращает GPU nvidia → поднимает gdm). VBIOS можно снять и через
  sysfs `/sys/.../rom`, НО он усечён (только PciAt+EFI, без FWSEC) — нужен PROM-shadow BAR0+0x300000.

**Всё остальное — 🟡 «компилируется в CI» или 📄 «сверено с исходником», НЕ
проверено на железе.** Правило R11/Property 1: «работает» только при HW-логе.

## Что написано в коде (компилируется в CI, не запускалось на macOS)

- `driver/RTXProbe/` — kext: матчинг `10DE:2783`, маппинг BAR0, чтение `PMC_BOOT_0`.
- `driver/RTXProbeDext/` — dext-аналог (PCIDriverKit).
- `ada_regs.h`, `falcon_regs.h` — регистры (сверены с nova-core).
- `tools/vbios_dump.c` — разбор VBIOS, извлечение FWSEC (CLI).
- `driver/gsp/fwsec_locate.{h,c}` — переиспользуемый локатор FWSEC.
- `driver/gsp/fwsec_patch.{h,c}` — патч FWSEC (FRTS-команда + RSA3K-подпись).
- `driver/gsp/falcon.{h,c}` — примитивы Falcon (reset GA102/select_core/program_brom/
  DMA-load/boot/mailbox/WPR2/GFW/fuse-версия) поверх абстрактного `nv_mmio_t`.
- `driver/gsp/fb_layout.{h,c}` — расчёт региона WPR2/FRTS из объёма VRAM.
- `driver/RTXProbe/FwsecRun.{hpp,cpp}` — **kext-оркестратор FWSEC-FRTS** (самодостаточен):
  VBIOS из BAR0 ROM-shadow → locate → DMA-буфер → patch → reset→dma_load→boot →
  проверка mbox0/WPR2. Точка входа: `RTXRunFwsecFrts(pci, bar0Base)`.

Вся логика портирована из **nova-core** (ядро Linux) со сверкой — соответствия в
`docs/PORTING-MAP.md`. Ничего не выдумывать; новые регистры — оттуда же, с атрибуцией.

## Критический путь дальше (tasks.md)

1. ~~**Задача 8: HW-верификация P1 на стенде.**~~ ✅ **ВЫПОЛНЕНО 2026-06-29.** Метрика
   `mbox0==0` И `WPR2 set=1` получена на RTX 4070S через Linux/VFIO. Доказательство:
   `docs/hw-dumps/20260629-rtx4070s-fwsec-frts-linux-OK.log`. Портируемая логика
   `driver/gsp/*` теперь 🟢. Гейт Правила 0 ОТКРЫТ → можно начинать задачи 6–7.
   (Остаётся 🟡: macOS-kext-шим `FwsecRun.cpp` — те же модули, но IOPCIDevice/
   IOBufferMemoryDescriptor на самой macOS не прогнаны; нужен GFW-wait и там — добавлен.)
2. Задача 6: Booter → GSP-RM (порт `firmware/booter.rs` + `gsp/*`). Большой порт.
3. Задача 7: очереди RPC (`message_queue_priv.h`, nouveau `r535.c`) → первый RPC = метрика слоя 2.
4. Задачи 9–10 (память/каналы), 12–17 (дисплей/Metal = замки Apple, гейт R10).

### Что сделано в этой сессии (2026-06-29) — ПЕРВЫЙ 🟢 СЛОЯ 2
- Прогон FWSEC-FRTS на реальной RTX 4070S через Linux/VFIO **без ребута и без монитора**.
  Метрика достигнута: `mbox0=0`, `WPR2 set=1 [0x2ff800000..0x2ff8e0000]`.
- Найдены и исправлены 2 реальных бага (на CI/чтении исходника были незаметны):
  - `falcon.c`/харнесс: добавлено **`nv_wait_gfw_boot_completed`** — ждать GFW после FLR,
    иначе `NV_USABLE_FB_SIZE_IN_MB` = `0xBADF5108` (priv-фолт) → мусорный FRTS-регион;
  - `fwsec_locate.c`: **`walk_images` принимает сигнатуру `0x4E56`** (NVIDIA-расширенные
    образы), а не только `0xAA55` — иначе FWSEC-образ (0xE0) недостижим (rc=NOIMG).
  - Сверено с nova-core `vbios.rs` (`0xAA55 | 0x4E56`), см. PORTING-MAP.
- Инструменты: `tools/run-fwsec-detached.sh` (прогон без ребута c возвратом GUI),
  дамп PROM в харнессе, дампы в `docs/hw-dumps/20260629-*`.
- **Найдены подписанные блобы для блокера №2:** `/usr/lib/firmware/nvidia/ad10x/gsp/`
  (booter_load/booter_unload/bootloader + GSP-RM, версии 535.113.01 и 570.144) — лежат в
  пакете nvidia-utils. Это кандидат-источник Booter/GSP-RM для задач 6–7 (проверить лицензию).

### Что сделано в сессии (2026-06-28)
- Аудит FWSEC-FRTS против nova-core master: исправлено D1 (повторный
  `reset_wait_mem_scrubbing` после `select_core` в `falcon.c`), D2 (отказ для
  не-V3 дескриптора в `FwsecRun.cpp`); D3 (IFR-заголовок) — помечен `TODO: verify`.
- `RTXProbe::start()` теперь вызывает `RTXRunFwsecFrts` на Ada (turnkey).
- Написаны runbook'и: `docs/bench-test-fwsec.md` (macOS-kext) и
  `docs/bench-test-fwsec-linux.md` (Linux/VFIO, headless).
- **Добавлен `tools/fwsec_run_linux.c`** — Linux-VFIO-хост для тех же модулей
  `driver/gsp/*` (mmap BAR0 + VFIO_IOMMU_MAP_DMA). Позволяет получить первый 🟢
  слоя 2 без macOS и без монитора (не требует установки ОС — live-USB).

### Рекомендованный путь к первому 🟢 слоя 2
Стенд: целевая машина (12400F+RTX, без iGPU) → **Linux live-USB headless** →
RTX на `vfio-pci` → SSH → `tools/fwsec_run_linux`. Метрика `mbox0=0`+`WPR2 set=1`.
Это дешевле и надёжнее macOS-стенда (нет проблемы с дисплеем/установкой). macOS-kext
проверяется отдельно позже. Подробности — `docs/bench-test-fwsec-linux.md`.

## Как проверять на железе (ограничения стенда)

- Целевая машина: i5-12400**F** (БЕЗ iGPU) + RTX → в macOS **чёрный экран** (нет
  драйвера дисплея — это и есть то, что строим). Дисплей не нужен для проверки
  kext: грузить headless, читать логи по SSH.
- Установить macOS вслепую нельзя. Есть вторая машина: **i5 4-го поколения (Haswell)
  с рабочей iGPU** — это «станция установки». План: на Haswell поставить **macOS
  Monterey** (не Tahoe — Haswell не тянет; для L1–L2 версия неважна) на **USB-SSD**,
  включить SSH + автозагрузку kext, OpenCore настроить под Alder Lake → перенести
  SSD в RTX-машину → boot headless → SSH → логи.
- Дешёвая проверка регистров без macOS: **RW-Everything на Windows** (читали так
  `PMC_BOOT_0`), либо `tools/nv_mmio_linux.c` с Linux live-USB. Windows не сносить.

## Правила (соблюдать строго)

1. Регистры/структуры — только из nova-core/OGK/nouveau со сверкой и записью в
   PORTING-MAP. Непроверенное → `TODO: verify`.
2. Прошивки (FWSEC/Booter/GSP-RM) — грузить как есть; единственный патч — FWSEC
   (init_cmd + signature).
3. «Компилируется» ≠ «работает». Статусы: 🟢 HW / 🟡 CI / 📄 SRC. Не завышать.
4. После каждого шага — обновлять `IMPLEMENTATION.md` и `PORTING-MAP.md`.
5. Все коммиты пушить (`git push origin master`); CI собирает на macOS-раннере.

## Открытое решение для владельца

A — поднять стенд и верифицировать готовый FWSEC-FRTS-путь (рекомендуется), либо
B — продолжать блайнд-порт Booter+RPC. На момент передачи владелец склонялся к
проверке железа (есть Haswell-станция).

## Репозиторий / окружение

- GitHub: `github.com/prd1324/RTXmacOC`, ветка `master`, CI `.github/workflows/build-macos.yml`.
- Разработка ведётся с Windows; macOS-сборка — только через CI или стенд.
