# Implementation Plan

> Задачи по фазам из design.md. Статус задачи `[x]` = **код написан** (и/или
> компилируется в CI / сверен с исходником). Это НЕ значит «проверено на железе» —
> HW-верификация вынесена в отдельные задачи (Property 1 / R11). Замки Apple (L5–L6)
> ведутся по гейт-протоколу (R10): доказательство → обход → классификация.

## Overview

План реализует design.md по фазам P1–P5 плюс сквозные задачи. Принцип: сначала
фундамент (P1–P2) с подтверждением на железе, затем замки Apple (P3–P4) по
гейт-протоколу R10, в конце — качество (P5). `[x]` = код написан/сверен, НЕ
«проверено на железе» (см. Notes).

## Tasks

### Фаза P1 — фундамент (L1–L2)

- [x] 1. Идентификация карты: kext/dext матчинг + чтение/декод `PMC_BOOT_0`
  - `driver/RTXProbe`, `driver/RTXProbeDext`, `ada_regs.h`, `pcie_probe.c`
  - декод сверен с nova-core (chipset 0x194); компилируется в CI
  - _Requirements: 2.1, 2.2_

- [x] 2. VBIOS → FWSEC: чтение ROM и извлечение Falcon-ucode
  - `tools/vbios_dump.c` (PCI-образы → BIT → PmuLookupTable → FalconUCodeDescV3)
  - _Requirements: 3.1_

- [x] 3. Примитивы Falcon (движок GSP) поверх абстрактного MMIO
  - `driver/gsp/falcon.{h,c}`, `falcon_regs.h`: reset(GA102)/select_core/program_brom/
    DMA-load/start/boot/mailbox/WPR2/GFW; сверено с nova-core
  - _Requirements: 3.1_

- [x] 4. Патч FWSEC-образа (FRTS + подпись)
  - `driver/gsp/fwsec_patch.{h,c}`: DMEMMAPPER init_cmd=FRTS, FrtsCmd, выбор/вставка
    подписи по fuse-версии
  - _Requirements: 3.1_

- [x] 5. Kernel DMA-буфер в kext и оркестрация запуска FWSEC-FRTS
  - `driver/gsp/fwsec_locate.{h,c}` — переиспользуемый локатор FWSEC (вынесен из vbios_dump)
  - `driver/RTXProbe/FwsecRun.{hpp,cpp}` — kext: VBIOS из BAR0 ROM shadow → locate →
    coherent DMA-буфер (`IOBufferMemoryDescriptor`) → patch FRTS+signature →
    reset(GA102) → dma_load → boot(mbox0=0) → проверка `mbox0==0` и `nv_wpr2_is_set()`
  - компилируется (CI); **на железе не исполнялось**
  - ✅ FRTS-регион теперь считается внутри (`driver/gsp/fb_layout.*`, порт `fb.rs`):
    оркестратор самодостаточен, внешний параметр убран.
  - _Requirements: 3.1, 12.2_

- [ ] 6. Booter Loader → загрузка GSP-RM в WPR2, старт RISC-V
  - порт `firmware/booter.rs`; занести регистры/структуры в PORTING-MAP
  - _Requirements: 3.1, 12.1, 12.2_

- [ ] 7. Очереди RPC GSP-RM и первый handshake
  - `driver/gsp/rpc.*`: command/status очереди в sysmem (порт `message_queue_priv.h`,
    nouveau `r535.c`), отправка первого RPC, чтение ответа
  - _Requirements: 3.1_

- [ ] 8. HW-верификация P1 (требует macOS-стенда с RTX)
  - ✅ ЧАСТИЧНО: декод `PMC_BOOT_0` подтверждён на реальной RTX 4070 Super
    (Windows/RW-Everything, `0x194000A1` → Ada AD104, rev A1;
    `docs/hw-dumps/20260628-rtx4070s-pmc_boot0-windows.md`).
  - осталось на macOS-стенде: «вижу Ada AD104» из НАШего kext, WPR2 set,
    «GSP-RM ответил на RPC» → `docs/hw-dumps/`
  - _Requirements: 2.2, 3.1, 3.4, 11.1_

### Фаза P2 — карта реально работает (L3–L4)

- [ ] 9. Memory management (GMMU/VRAM) через RPC к GSP-RM
  - alloc/map VRAM; модели данных RM API → PORTING-MAP
  - сюда же: вычисление региона WPR2 (`frts_addr/frts_size`) из объёма VRAM
    (`NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE` / `NV_USABLE_FB_SIZE_IN_MB`) — нужно для
    реального запуска FWSEC-FRTS из задачи 5
  - _Requirements: 3.2_

- [ ] 10. Канал команд + тестовая команда GPU
  - pushbuffer/канал через RPC; исполнить тест (например, copy-engine)
  - _Requirements: 3.2_

- [ ] 11. HW-верификация P2 (требует стенда)
  - HW-логи: успешная аллокация VRAM, исполнение команды GPU
  - _Requirements: 3.2, 3.3, 11.1_

### Фаза P3 — вывод изображения (L5, замок Apple)

- [ ] 12. IOFramebuffer-провайдер + modeset на NVDisplay
  - референс движка дисплея — nouveau `dispnv50`; modeset через RPC к GSP-RM
  - _Requirements: 5.1, 5.2_

- [ ] 13. Гейт R10 для L5: доказательство + обход
  - если WindowServer не принимает framebuffer — собрать техническое доказательство
    (логи/символы), испытать минимальный framebuffer, классифицировать (R9)
  - _Requirements: 5.3, 10.1, 10.2, 10.3_

### Фаза P4 — ускорение и Metal (L6, главный замок)

- [ ] 14. Bypass-подсистема: отключение library validation
  - конфиг OpenCore `amfi_get_out_of_my_way=1` / AMFIPass-подход; зафиксировать как
    постоянный обход (R9.2), проверить загрузку стороннего бандла в процесс
  - _Requirements: 6.3, 7.4, 9.2, 10.2_

- [ ] 15. IOAccelerator-сервис драйвера
  - предоставить IOAccelerator-совместимый интерфейс (референс — путь AMD)
  - _Requirements: 6.1, 6.2_

- [ ] 16. Metal GPU driver bundle (реверс приватного ABI)
  - дизассемблер AMD-бандла как образца легального интерфейса; реализовать `MTLDevice`
  - кандидат на «доказанный предел», если ABI непреодолим
  - _Requirements: 7.1, 7.2_

- [ ] 17. Гейт R10 для L6: доказательство + обход + классификация
  - если Metal-ABI/подпись непреодолимы штатно — собрать доказательство, испытать
    обход, классифицировать стабильность
  - _Requirements: 7.4, 10.1, 10.2, 10.3_

### Фаза P5 — качество (стабильность, перф, повседневность)

- [ ] 18. Стабильность и восстановление
  - 24ч под нагрузкой без panic; sleep/wake; изоляция отказа GPU
  - _Requirements: 4.1, 4.2, 4.3_

- [ ] 19. Производительность vs AMD-эталон
  - бенчи graphics/compute, сравнение с AMD на том же стенде, фиксация порога
  - _Requirements: 8.1, 8.2_

- [ ] 20. Повседневность без хрупких обходов
  - убедиться, что прод-конфиг не требует ручных действий на загрузку; документ
    «постоянные vs хрупкие обходы»
  - _Requirements: 9.1, 9.2, 9.3_

### Сквозные задачи (на всём протяжении)

- [ ] 21. Verification harness + AMD baseline
  - процедура снятия HW-логов в `docs/hw-dumps/`; описание AMD-эталонного стенда
  - _Requirements: 1.3, 1.4, 11.1, 11.2, 11.3_

- [ ] 22. Поддержание источника правды
  - после каждого слоя обновлять `PORTING-MAP.md` и `IMPLEMENTATION.md`; новые
    регистры — с атрибуцией; непроверенное — `TODO: verify`
  - _Requirements: 12.1, 12.3_

- [ ] 23. Финальный вывод проекта (R10.4)
  - один из двух: (а) RTX полноценно работает в Tahoe (R2–R9 закрыты HW-логами),
    либо (б) документ-доказательство, какие ограничения Apple блокируют и каков
    предел достигнутого обхода
  - _Requirements: 10.4_

## Task Dependency Graph

```
1 (identify) ─┐
2 (vbios)     ├─> 5 (kernel DMA + FWSEC run) ─> 6 (booter) ─> 7 (RPC handshake) ─> 8 (HW-verify P1)
3 (falcon)    ┤
4 (fwsec patch)┘

7 ─> 9 (memory) ─> 10 (channel) ─> 11 (HW-verify P2)

11 ─> 12 (IOFramebuffer/modeset) ─> 13 (L5 gate R10)
13 ─> 14 (disable lib validation) ─> 15 (IOAccelerator) ─> 16 (Metal bundle RE) ─> 17 (L6 gate R10)
17 ─> 18 (stability) ─> 19 (perf vs AMD) ─> 20 (daily-use) ─> 23 (final determination)

21 (verify harness/AMD baseline) — нужна с задачи 8 и далее
22 (source-of-truth upkeep) — параллельно всем
```

Критический путь: 1–4 → 5 → 6 → 7 → 9 → 10 → 12 → (14) → 15 → 16 → 18 → 23.
Задачи 8/11 (HW-верификация), 13/17 (гейты R10) — обязательные контрольные точки.

Волны выполнения (можно параллелить внутри волны):

```json
{
  "waves": [
    { "wave": 1, "tasks": [1, 2, 3, 4], "note": "фундамент-код, без зависимостей" },
    { "wave": 2, "tasks": [5], "note": "kernel DMA + запуск FWSEC-FRTS" },
    { "wave": 3, "tasks": [6], "note": "Booter -> GSP-RM" },
    { "wave": 4, "tasks": [7], "note": "RPC handshake" },
    { "wave": 5, "tasks": [8, 9, 21], "note": "HW-verify P1 + старт памяти + harness" },
    { "wave": 6, "tasks": [10], "note": "канал команд" },
    { "wave": 7, "tasks": [11], "note": "HW-verify P2" },
    { "wave": 8, "tasks": [12], "note": "IOFramebuffer/modeset (L5)" },
    { "wave": 9, "tasks": [13], "note": "гейт R10 для L5" },
    { "wave": 10, "tasks": [14, 15], "note": "обход lib validation + IOAccelerator" },
    { "wave": 11, "tasks": [16], "note": "Metal bundle (реверс ABI)" },
    { "wave": 12, "tasks": [17], "note": "гейт R10 для L6" },
    { "wave": 13, "tasks": [18, 19, 20], "note": "стабильность/перф/повседневность" },
    { "wave": 14, "tasks": [23], "note": "финальный вывод проекта" }
  ],
  "continuous": [22]
}
```

## Notes

- **Статус `[x]` ≠ «работает».** Это «код написан / компилируется / сверен с
  исходником». Перевод в «работает» — только после HW-лога (задачи 8, 11 и т.д.),
  по Property 1 / R11.
- **Блокер.** Задачи 12–17 (L5–L6) упираются в модель Apple. Возможный легитимный
  исход — не «картинка», а доказанный предел (задача 23, R10.4b). Это не провал по
  требованиям.
- **Нет стенда сейчас.** Все HW-задачи (8, 11, 13, 17, 18, 19, 20) заблокированы до
  появления macOS Tahoe + RTX (и для P3+ — рабочего вывода/AMD-эталона). Пока стенда
  нет — двигаем только код-задачи (5, 6, 7, 9, 10, 12, 15, 16) и подготовку обходов.
- **Прошивки.** Задачи 5–7 грузят FWSEC/Booter/GSP-RM как есть (R12.2); единственный
  патч — FWSEC (задача 4).
- **Источник правды.** Каждая код-задача завершается обновлением `PORTING-MAP.md` и
  `IMPLEMENTATION.md` (задача 22).
