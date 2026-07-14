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

1. **PCIe bring-up** — найти карту, смапить BAR0, прочитать `PMC_BOOT_0`.
   🟡 код есть и компилируется в CI; **на железе не запускался**.
2. **GSP bring-up** — поднять GPU через GSP, наладить RPC. 🟢 **ЗАВЕРШЁН на железе**
   (2026-06-30, Linux/VFIO): FWSEC-FRTS→WPR2→Booter→GSP RISC-V active → **`GSP_INIT_DONE`
   по RPC** (задачи 6 И 7). Полная тех-запись: **`docs/gsp-bringup-layer2.md`** (читать,
   если что-то по слою 2 регрессует — там все смещения структур, опкоды, грабли).
3. Memory management (GMMU/VRAM) через RPC. 🟢 **проходы A+B+C на железе**: A — двусторонний
   RPC + `GET_GSP_STATIC_INFO` + RM client/device/subdevice; B — `RM_CONTROL`/`FB_GET_INFO_V2`
   + `FERMI_VASPACE_A` (корень GMMU); C — регистрация VRAM (`NV01_MEMORY_LIST_FBMEM` через
   `ALLOC_MEMORY`; VRAM'ом владеет гость). Проход D (VRAM → GPU VA): 🟢 **прямой
   GMMU-маппинг** (page-tables во VRAM + `COPY_SERVER_RESERVED_PDES`, как `nouveau vmm`);
   RPC-путь `MAP_MEMORY_DMA (14)` был отвергнут GSP (`NV_ERR_INVALID_FUNCTION`, vGPU-путь).
   Тех-запись: **`docs/gsp-layer3-rpc.md`** §4D.
4. Command submission (каналы).
5. Display / modeset — **вывод картинки**. Конечная видимая цель.
6. 3D/compute (Metal) — самое дальнее.

Важно: слой 5 (дисплей) невозможен без 2+3+4. Слой 2 закрыт; слой 3 — все три RPC-глагола,
GMMU-корень, VRAM memlist и VA-маппинг (прямой GMMU + COPY_SERVER_RESERVED_PDES) на железе.

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

0. **Перед реализацией слоя 2+ — сверься с `docs/IMPLEMENTATION.md` (что уже есть)
   и `docs/PORTING-MAP.md` (из какого upstream портируется что).** Это источник
   правды, чтобы не сойти с пути корректной реализации. После значимого шага —
   обнови оба файла.
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

> **Новому агенту: начни с `docs/HANDOFF.md`** — самодостаточная сводка.
> Детальный статус — `docs/IMPLEMENTATION.md`; соответствие исходникам — `docs/PORTING-MAP.md`.
>
> **Легенда честности:** 🟢 HW = подтверждено логом с железа; 🟡 CI = только
> компилируется; 📄 SRC = совпадает с исходником при чтении кода.

- ⚠️ **БЛОКЕР (Неделя 2):** на Big Sur+ нет публичной точки расширения для стороннего
  GPU-акселератора в WindowServer (library validation + приватный Metal-ABI +
  DriverKit без графики). Слои 5–6 заблокированы Apple; обход — `docs/macos-graphics-stack.md`.
- 🟢 **Подтверждено на железе** (Windows/RW-Everything на RTX 4070 Super):
  BAR0=`0x52000000`, `PMC_BOOT_0=0x194000A1` (Ada AD104, rev A1), адреса WPR2-регистров.
  См. `docs/hw-dumps/`.
- Слой 1: 🟢 декод подтверждён железом; загрузка НАШего kext на macOS — ждёт стенда.
- Слой 2: 🟢 **ЗАВЕРШЁН НА ЖЕЛЕЗЕ 2026-06-30** (RTX 4070S, Linux/VFIO) — задачи 6 И 7.
  Полная цепочка за прогон (`tools/gsp_boot_linux.c`): FWSEC-FRTS→WPR2 → staging GSP-RM
  (fwimage+radix3+bootloader+signature+WprMeta+libos+очереди+rmargs) → **SET_SYSTEM_INFO+
  SET_REGISTRY в cmdq до бута** → SEC2 Booter `mbox0=0` → GSP RISC-V active → дренаж msgq
  с исполнением **`GSP_RUN_CPU_SEQUENCER`** (mid-init ресет GSP-ядра) → **`GSP_INIT_DONE`
  (0x1001) по RPC**. Доказательство: `docs/hw-dumps/20260630-rtx4070s-gsp-INIT_DONE-OK.log`.
  **ПОЛНАЯ ТЕХ-ЗАПИСЬ (смещения структур, опкоды, грабли): `docs/gsp-bringup-layer2.md`** —
  читать первым делом при любой регрессии слоя 2. Портируемая логика `driver/gsp/*`
  (falcon, fwsec_*, fb_layout, booter, gsp_fw, gsp_rpc, elf64, fw_blob) — 🟢.
  macOS-kext-шим `FwsecRun.*` всё ещё 🟡 (RPC-часть на kext не портирована).
- Слой 3: 🟢 **ПРОХОД A НА ЖЕЛЕЗЕ 2026-06-30** (RTX 4070S, Linux/VFIO) — двусторонний RPC.
  Тот же прогон (`tools/gsp_boot_linux.c`, блок «СЛОЙ 3») после `GSP_INIT_DONE`:
  **`GET_GSP_STATIC_INFO` (65)** → карта 5 FB-регионов VRAM (вершина=12282 МиБ) + внутр.
  хэндлы GSP + barPde; затем **`GSP_RM_ALLOC` (103)** цепочка `NV01_ROOT`→`NV01_DEVICE_0`→
  `NV20_SUBDEVICE_0`, каждый `status=NV_OK`. Доказательство:
  `docs/hw-dumps/20260630-rtx4070s-layer3-rpc-OK.log`. Портируемый модуль
  `driver/gsp/gsp_rm.{c,h}`, офлайн-тест `tools/gsp_rm_test.c` (`make gsp-rm-test`).
  **ТЕХ-ЗАПИСЬ: `docs/gsp-layer3-rpc.md`** — читать при регрессии слоя 3.
- Слой 3 **проходы B, C**: 🟢 **НА ЖЕЛЕЗЕ 2026-07-01** (тот же прогон, блоки «3B»/«3C»):
  **`GSP_RM_CONTROL` (76) / `FB_GET_INFO_V2`** → конфиг VRAM (RAM=12282 МиБ, BAR1=256 МиБ,
  `NV_OK`); **`FERMI_VASPACE_A` (0x90f1)** → корень GMMU `NV_OK`; **`ALLOC_MEMORY` (4) /
  `NV01_MEMORY_LIST_FBMEM` (0x82)** → регистрация VRAM-диапазона (1 МиБ) `rpc_result=0`.
  Доказательства: `docs/hw-dumps/20260701-rtx4070s-layer3-pass{B,C-vram}-OK.log`. Код:
  `nv_gsp_rm_control`/`nv_gsp_fb_get_info`/`nv_gsp_rm_vaspace_ctor`/`nv_gsp_rm_vram_memlist`.
  **Тупик (в тех-записи §5):** VRAM НЕ аллоцировать из кучи GSP через `NV01_MEMORY_LOCAL_USER`
  (отвергается) — гость даёт физ. страницы (memlist).
- Слой 3 **проход D**: 🟢 **HW 2026-07-14 — прямой GMMU**. RPC-путь `MAP_MEMORY_DMA (14)`
  оказался тупиком (`NV_ERR_INVALID_FUNCTION` — путь vGPU/SR-IOV; лог
  `docs/hw-dumps/gsp-boot-layer3-passD-20260714.log`). Верный путь реализован: КЛИЕНТ сам
  строит page-tables во VRAM (PRAMIN, формат Ada ga10x/gp100 — `driver/gsp/gmmu.{c,h}`,
  `nv_gmmu_map_range`), затем `RM_CONTROL VASPACE_COPY_SERVER_RESERVED_PDES (0x90f10106)`
  отдаёт GSP физ-адреса корней PD3/PD2/PD1 (`nv_gsp_rm_vaspace_copy_pdes`, порт
  `r535_mmu_promote_vmm`). На железе: `status=NV_OK` + read-back листовой PTE `MATCH`
  (`0x1310001` = `(0x13100000>>4)|VALID`). Лог:
  `docs/hw-dumps/20260714-rtx4070s-layer3-passD-gmmu-OK.log`. Код `nv_gsp_rm_map_memory_dma`
  + `nv_gsp_rm_vmem_ctor` остаются как задокументированный отрицательный результат.
- Спека: `.kiro/specs/rtx-tahoe-full-support/` (requirements/design/tasks).
- Прогон без ребута/монитора: `tools/run-gsp-boot-detached.sh` (весь слой 2 + слой 3
  проходы A/B/C/D), `tools/run-fwsec-detached.sh` (только FWSEC). Linux/VFIO, возвращают GUI.
- Дальше: **слой 4** (каналы: FIFO/GR RM-control'ы). Слой 3 закрыт полностью (A+B+C+D).

## Ключевые источники (референс-база)

- NVIDIA open-gpu-kernel-modules — https://github.com/NVIDIA/open-gpu-kernel-modules
- nova-core (Rust, чистый разбор) — https://docs.kernel.org/gpu/nova/core/
- drm/nouveau (GSP/RPC) — https://docs.kernel.org/gpu/nouveau.html


## Правило 0 — Hardware Verification Gate (главное правило проекта)

Статус "done" имеет ТРИ разных значения, и их нельзя путать:

| Маркер | Значение | Что требуется как доказательство |
|---|---|---|
| 📝 | спроектировано на бумаге | ссылка на upstream-источник |
| 🔧 | код написан, НЕ исполнялся на реальном железе | "BUILD SUCCEEDED" в CI — этого мало для следующего уровня |
| ✅ | подтверждено на реальном железе | путь к файлу в docs/hw-dumps/ с реальным выводом/логом, дата, что именно подтвердилось |

ЗАПРЕЩЕНО:
- ставить ✅, если единственное доказательство — компиляция в CI;
- писать код для слоя N+1, если слой N не дошёл до ✅ хотя бы в
  минимальной метрике (не "весь слой готов", а "ключевая метрика
  слоя подтверждена на железе");
- использовать формулировки уровня "стена пробита", "то, до чего
  никто не добрался" и подобные — статус описывается фактом
  ("прочитан регистр X, значение Y, лог тут"), без оценки значимости;
- продолжать молча, если стоит открытый архитектурный вопрос
  (раздел "Открытые вопросы" ниже) — нужно остановиться и сказать
  "это блокер, нужно решить, прежде чем продолжать".

Перед тем как пометить что-либо ✅, AI обязан явно написать одну
строку: "Доказательство: <путь к логу / файлу дампа>". Если такого
файла нет — статус не выше 🔧.

## Открытые вопросы (блокеры — нельзя строить поверх, не закрыв)

1. Существует ли в современном macOS точка расширения для
   стороннего kext-акселератора WindowServer/IOGraphics, или модель
   уже закрыта Apple? — БЛОКИРУЕТ весь смысл слоёв 5–6.
2. ✅ **РАЗРЕШЁН (2026-06-29).** Где взять подписанные Booter и GSP-RM блобы?
   — В составе **linux-firmware/nvidia** (`/usr/lib/firmware/nvidia/ad104/gsp/`:
   `booter_load/booter_unload/bootloader/gsp-535.113.01.bin.zst`), тот же легальный
   источник, что у nouveau (лицензия NVIDIA разрешает распространение). Грузим как
   есть (правило 2), целевая версия 535.113.01; в репозиторий блобы НЕ коммитим
   (правило 7) — берём из системы. Загрузка/распаковка — `driver/gsp/fw_blob.h`.

Любая новая открытая архитектурная неопределённость такого уровня
добавляется сюда, а не замалчивается до момента, когда уже поздно.
