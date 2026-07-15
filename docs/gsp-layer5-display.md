# Слой 5 — дисплей / modeset через GSP-RM

**Статус:** 🟢 **5A+5B+5C.1..5C.4d НА ЖЕЛЕЗЕ 2026-07-14..16** (RTX 4070S, Linux/VFIO).
5C.4d: core-channel modeset САБМИТ проглочен (GET==PUT, 51 метод) — тайминг 1280x1024@108МГц
+ SOR0 запрограммированы, первая команда в дисплейный канал. Пруф
`docs/hw-dumps/20260716-rtx4070s-layer5-C4d-coremodeset-OK.log`.
5C.4b: framebuffer 1920x1080 во VRAM (0x14000000) залит R/G/B, read-back совпал.
5C.1: RAMIN + display root `AD102_DISP` (0xc7700000). 5C.2: core channel
`AD102_DISP_CORE_CHANNEL_DMA` (0xc77d0000). 5C.3: SOR acquire (HDMI→SOR0, DP→SOR1).
5C.4a: window channel `GA102_DISP_WINDOW_CHANNEL_DMA` (0xc67e0000). Всё `NV_OK` — вся
инфраструктура modeset (core+window+SOR) поднята. Пруфы:
`docs/hw-dumps/20260714-rtx4070s-layer5-C{1-disproot,2-corechan,3-sor}-OK.log`,
`docs/hw-dumps/20260715-rtx4070s-layer5-C4a-winchan-OK.log`. Ниже: 5A —
`NV04_DISPLAY_COMMON`, `heads=4`, `displayMask=0x7f00` (7 выходов); 5B — перечислены
типы/протоколы всех 7 OR (SOR TMDS/DP), **два монитора обнаружены** (`connected=0x300`),
**EDID прочитан по DDC** (magic `00ffffffffffff00`, 384 байта). Эталон — nouveau
`nvkm/engine/disp/r535.c` (`r535_disp_oneinit`/`r535_outp_new`/`r535_outp_detect`/
`r535_tmds_edid_get`), контролы `ctrl0073*.h`, классы `nvif/class.h`/`g_allclasses.h`.
**Доказательства:** `docs/hw-dumps/20260714-rtx4070s-layer5-A0-disp-OK.log` (5A),
`docs/hw-dumps/20260714-rtx4070s-layer5-B-edid-OK.log` (5B).

**Важно про два трека слоя 5:**
1. **Аппаратный modeset через GSP-RM** (этот документ) — гоним на Linux/VFIO, как
   слои 2–4. От macOS НЕ зависит. Цель: реально засветить монитор с RTX нашим кодом.
2. **Интеграция в macOS WindowServer/IOFramebuffer** — отдельный трек, упирается в
   открытый блокер №1 (нет публичной точки расширения для стороннего GPU-акселератора
   на Big Sur+; см. `docs/macos-graphics-stack.md`). Ведётся по гейт-протоколу R10.

Трек 1 доказывает, что **железо умеет выводить картинку под нашим драйвером** — это
снимает вопрос «а заведётся ли GPU вообще», независимо от замка Apple.

---

## 0. Последовательность GSP-display (порт r535_disp_oneinit) и метрики

После слоёв 2–4 (GSP жив, RPC, RM client/device/subdevice, VA, каналы):

**5A — энумерация дисплейного движка (метрика 🟢, БЕЗ монитора):**
1. `GSP_RM_ALLOC` класс `NV04_DISPLAY_COMMON (0x0073)` под device, hObject=`0x00730000`
   (params 0). Это объект «objcom» для всех контролов `NV0073_*`.
2. `RM_CONTROL NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS (0x730102)` → число heads.
3. `RM_CONTROL NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED (0x730120)` → `displayMask`
   (какие displayId физически есть) + `displayMaskDDC`.
   Метрика: disp-common создан + heads>0 + displayMask≠0 (движок перечислен).

**5B — коннекторы/EDID (метрика 🟢, EDID требует подключённого монитора):**
4. Для каждого displayId из `displayMask`: `NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO`
   (тип/протокол/локация OR — SOR TMDS/DP), `NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE`
   (подключён ли монитор), `NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2` (EDID-байты).
   Для DP — `NV0073_CTRL_CMD_DP_GET_CAPS` (max link rate, MST).

**5C — modeset + scanout (метрика 🟢 = картинка на мониторе, нужен монитор+FB).**
Разбит на под-шаги:
- **5C.1** (🔧 framing): RAMIN (inst 0x10000 во VRAM, обнулить через PRAMIN) +
  `NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM (0x20800a49)` на **внутренний**
  subdevice GSP (`hInternalClient/Subdevice` из GET_STATIC_INFO: 0xc2000005/0xabcd2080)
  → `GSP_RM_ALLOC` display root **`AD102_DISP (0xC770)`** под device (hObject=class<<16,
  params 0). Метрика: WRITE_INST_MEM + root alloc = NV_OK.
- **5C.2** (🟢 HW 2026-07-14: `DISPLAY_CHANNEL_PUSHBUFFER` + `CORE_CHANNEL_DMA`
  handle=0xc77d0000, оба `NV_OK`; пруф `docs/hw-dumps/20260714-rtx4070s-layer5-C2-corechan-OK.log`):
  core channel `AD102_DISP_CORE_CHANNEL_DMA (0xC77D)` под display
  root: пушбуфер во VRAM (≤4К) → `INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER (0x20800a58)` на
  внутр. subdevice GSP → `GSP_RM_ALLOC` core channel (params
  `NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS` 32б: channelInstance=0, offset=0).
  hObject=(class<<16)|inst=0xc77d0000. Window `GA102_DISP_WINDOW_CHANNEL_DMA (0xC67E)`,
  cursor `GA102_DISP_CURSOR (0xC67A)` — для surface scanout (5C.4).
- **5C.3** (🟢 HW 2026-07-14: ASSIGN_SOR — HDMI 0x100→SOR0, DP 0x200→SOR1, `NV_OK`;
  пруф `docs/hw-dumps/20260714-rtx4070s-layer5-C3-sor-OK.log`): SOR acquire
  `NV0073_CTRL_CMD_DFP_ASSIGN_SOR (0x731152)` для
  подключённого displayId — до modeset и до DP link training. params 80б (displayId@4,
  sorExcludeMask@8, sorAssignListWithTag[4]@40 {displayMask,sorType}, flags@76); индекс
  SOR = запись, чей displayMask содержит наш displayId (порт r535_outp_acquire). Затем
  для DP — link training `NV0073_CTRL_CMD_DP_CTRL`, для HDMI/TMDS — modeset напрямую.
- **5C.4** (🔧 фундамент — энкодер методов): framebuffer-surface во VRAM (наш GMMU) +
  поток методов core/window channel → выставить mode из EDID, scanout = картинка.
  Это самый большой под-шаг; метрика требует ВИЗУАЛЬНОГО подтверждения на мониторе.

  **Формат DMA-методов дисплея (NVC37D_DMA, наследует NVC77D):** слово метода =
  `opcode[31:29]=METHOD(0) | count[27:18] | methodOffset[13:2]=addr>>2`, за ним `count`
  слов данных. Регистры канала: `PUT@0`, `GET@4` в user-регионе (core → `BAR0+0x680000`).
  Реализован энкодер `nv_gsp_disp_push_method` (offline-тест) — фундамент под сборку
  потока modeset. Метод-карта (clc37d.h/clc77d.h, база Volta):
  - `HEAD_SET_RASTER_SIZE(h)` = `0x2064 + h*0x400` (WIDTH[14:0]|HEIGHT[30:16]);
  - head-методы: базовый `0x2000 + h*0x400` (RASTER_SYNC_END/BLANK_START/BLANK_END,
    SET_CONTROL, SET_PIXEL_CLOCK_FREQUENCY, SET_CONTROL_OUTPUT_RESOURCE, VIEWPORT_SIZE_IN/OUT);
  - `SOR_SET_CONTROL(sor)`, `SET_CONTEXT_DMA_*`, финальный `UPDATE` (0x200).
  - Surface — через window channel `NVC77E` (SET_STORAGE/SET_PARAMS/SET_CONTEXT_DMA_ISO/
    SET_POINT_IN/SET_SIZE_IN/OUT/UPDATE) + IMM `NVC77B`.

  **Оставшиеся шаги 5C.4** (по одному метрика-HW-прогону, для HDMI-монитора SOR0, без
  DP link training): (a ✅ 🟢 HW 2026-07-15) window channel `GA102_DISP_WINDOW_CHANNEL_DMA
  (0xC67E)` alloc (handle 0xc67e0000, `NV_OK`; пруф
  `docs/hw-dumps/20260715-rtx4070s-layer5-C4a-winchan-OK.log`) [+ IMM `0xC67B` при нужде];
  (b ✅ 🟢 HW 2026-07-15) framebuffer во VRAM
  (FB=0x14000000, 1920x1080 BGRA8888, pitch 7680, size 0x7e9000) + заливка 3 полос R/G/B
  через PRAMIN, read-back совпал (R=0x00ff0000 G=0x0000ff00 B=0x000000ff); пруф
  `docs/hw-dumps/20260715-rtx4070s-layer5-C4b-fb-OK.log`; (c ✅ framing ПОЛНЫЙ поток
  modeset собран, offline-зелёный — см. блок ниже) core+window methods из EDID-таймингов
  + `UPDATE`; (d) HW-сабмит (PRAMIN+PUT+poll) + ctx-dma/RAMHT → **картинка**. Тайминги —
  из EDID (прочитан в 5B). Для DP-монитора дополнительно `NV0073_CTRL_CMD_DP_CTRL`.

  **5C.4c ПОЛНЫЙ поток modeset собран (framing, offline-зелёный):** глубокая разведка
  nouveau dispnv50 (`headc37d_mode`/`_or`/`_view`, `corec37d_init`/`_wndw_owner`/`_update`,
  `wndwc37e_image_set`/`_update`) + сверка смещений по `clc37d.h`/`clc37e.h` (535.113.01).
  Билдеры (`driver/gsp/gsp_disp.{c,h}`, тесты `tools/gsp_disp_test.c`):
  - `build_core_modeset` (15): SOR_SET_CONTROL, PROCAMP(RGB/GRAPHICS), OUTPUT_RESOURCE
    (24bpp+полярности), RASTER_SIZE/SYNC_END/BLANK_END/BLANK_START/**VERT_BLANK2**,
    HEAD_SET_CONTROL(progressive), **PIXEL_CLOCK_FREQUENCY=0x200C(+MAX=0x2028, HERTZ=kHz*1000)**,
    **HEAD_USAGE_BOUNDS=0x2030**, VIEWPORT_POINT_IN/SIZE_IN/SIZE_OUT. (пробел PIXEL_CLOCK закрыт.)
  - `build_core_init` (33): SET_CONTEXT_DMA_NOTIFIER + 8×(FORMAT_USAGE_BOUNDS 0x1F + ROTATED 0
    + USAGE_BOUNDS 0x127fff) + 8×WINDOW_SET_CONTROL OWNER=head(i>>1).
  - `build_core_update`: SET_INTERLOCK_FLAGS(0)+SET_WINDOW_INTERLOCK_FLAGS(mask)+UPDATE(0x1).
  - `build_window_image` (10 NVC37E): SET_PRESENT_CONTROL, SET_SIZE, SET_STORAGE(PITCH),
    SET_PARAMS(X8R8G8B8/RGB/BYPASS), SET_PLANAR_STORAGE(pitch>>6), SET_CONTEXT_DMA_ISO,
    SET_OFFSET(fb>>8), SET_POINT_IN, SET_SIZE_IN/OUT.
  - `build_window_update`: SET_INTERLOCK_FLAGS(WITH_CORE)+UPDATE(0x1).
  - `build_ctxdma_desc` (24б Volta+, flags0=0x45 VRAM|RW|PAGE_SP, start/limit>>8) +
    `ramht_entry` (слот/context RAMHT: chid<<25 | client&0x3fff | inst_off<<9).

  **Механизм сабмита (5C.4d, HW):** методы копятся в пушбуфере во VRAM (core 0x13410000,
  window 0x13412000) через PRAMIN; PUT в user-регионе BAR0 (core→`0x680000`,
  window→`0x690000+head*0x1000`, DWORD-единицы) бампаем MMIO; ждём GET==PUT/notifier.
  ctx-dma NV_DMA_IN_MEMORY: дескриптор в inst-mem дисплея (после RAMHT 0x1000), RAMHT
  {handle,context} даёт доступ каналу. Core+window интерлочатся (атомарный modeset+flip).
  **Открытый риск:** точная семантика `inst_offset` в context RAMHT (nvkm node->offset) и
  chid.user — проверяются на железе.

  **5C.4d 🟢 HW 2026-07-16:** core-channel modeset САБМИТ проглочен. EDID выбранного
  TMDS-выхода (did=0x100→SOR0) распарсен: **1280x1024@108МГц, sync h+/v+**. Поток 408 байт
  (51 метод: init 33 + modeset 15 + update 3) записан в core-пушбуфер (0x13410000) через
  PRAMIN, PUT=0x198 → **GET=0x198 (GET==PUT)** — дисплейный движок исполнил весь поток
  методов без ошибок канала. Тайминг+SOR0 запрограммированы. **Первая команда в дисплейный
  канал на железе.** Механизм сабмита (PRAMIN+PUT/GET, NV507C PTR[11:2]) подтверждён.
  Пруф: `docs/hw-dumps/20260716-rtx4070s-layer5-C4d-coremodeset-OK.log`.
  Примечание: монитор 1280x1024 (не 1920x1080) — для 5C.4e FB+window строить под нативное
  разрешение из EDID. Пиксели → 5C.4e (window surface + ctx-dma/RAMHT).

  **5C.4e HW-прогон #1 (2026-07-16): ctx-dma/RAMHT записаны верно (readback совпал), но
  window GET встал на UPDATE (12/13 методов).** Диагноз: первичная активация окна должна
  быть АТОМАРНО сцеплена (interlocked) с core UPDATE — я ошибочно подал раздельные UPDATE.
  Исправлено: core UPDATE интерлочится с окном0 (`SET_WINDOW_INTERLOCK_FLAGS=1`), window
  UPDATE — с core (`SET_INTERLOCK_FLAGS=WITH_CORE`), оба PUT бампаются → защёлкиваются
  вместе (как nouveau на первом modeset). Пруф #1:
  `docs/hw-dumps/20260716-rtx4070s-layer5-C4e-interlock-stall.log`.
  Заметка: пока поднимаем ОДИН выход (TMDS 0x100→SOR0, head0). Второй монитор (DP 0x200→
  SOR1) — отдельный шаг (head1 + DP link training `DP_CTRL`); разрешение каждого берётся
  из его EDID (авто).

  **5C.4e (оркестратор, готов к HW-прогону):** атомарный interlocked modeset+flip = пиксели.
  В `tools/gsp_boot_linux.c` (после успешного 5C.4d): (1) FB под нативное разрешение из
  EDID (mt.hact×mt.vact, R/G/B полосы); (2) ctx-dma `NV_DMA_IN_MEMORY` (весь VRAM, RDWR) —
  24б дескриптор (flags0=0x45) в inst-mem дисплея @`disp_inst+0x1000` через PRAMIN; (3)
  RAMHT-запись {handle=VRAM, context=chid<<25|inst_off<<9} для window-канала (chid=1),
  т.к. `hcli=0xc1d00000`→`client&0x3fff=0` (без перекрытия битов), с read-back; (4) поток
  window `build_window_image`+`build_window_update`(без интерлока — core уже применён) →
  win-пушбуфер (0x13412000) через PRAMIN; (5) бамп window PUT (`BAR0+0x690000`) → poll
  GET==PUT. Метрика 🟢: window GET==PUT + **ПИКСЕЛИ на мониторе** (R/G/B полосы). **Открытый
  риск:** семантика `inst_offset` в RAMHT-context (node->offset — байт-offset относительно
  disp_inst) — проверяется на железе; при промахе ctx-dma не резолвится → диагностика в логе.

  **5C.4d (оркестратор):** сабмит core-channel modeset БЕЗ surface/
  ctx-dma (де-риск: сначала проверяем сам механизм сабмита + программу таймингов). В
  `tools/gsp_boot_linux.c`: парсим EDID выбранного TMDS-выхода → строим поток
  `build_core_init(notifier=0)` + `build_core_modeset` + `build_core_update(interlock=0)`
  → пишем в core-пушбуфер (0x13410000) через PRAMIN → бампаем PUT (`BAR0+0x680000+0x0`,
  PTR[11:2]=байт-offset) → ждём GET==PUT (`+0x4`). Метрика 🟡: GET==PUT (GSP/дисплей
  проглотил поток) + монитор синхронизируется (тайминг+SOR запрограммированы; пикселей
  пока нет — они в 5C.4e через window+ctx-dma). PUT/GET-смещения — nvhw `cl507c.h`
  (NV507C_PUT=0x0, GET=0x4, PTR[11:2]).

---

## 1. Классы и хэндлы (nvif/class.h, r535.c, сверено)

| Класс | Значение | Назначение |
|---|---|---|
| `NV04_DISPLAY_COMMON` | `0x0073` | objcom — все контролы NV0073_* (hObject `0x00730000`) |
| `AD102_DISP` | `0xC770` | display root класс (Ada), alloc `oclass<<16` |
| core/wndw/wimm/curs | `*7d/*7e/*7b/*7a` | каналы дисплея (5C) |

## 2. Контролы 5A (ctrl0073system.h, сверено)

| Контрол | cmd | params |
|---|---|---|
| `SYSTEM_GET_NUM_HEADS` | `0x730102` | `{u32 subDeviceInstance; u32 flags; u32 numHeads}` (12б, numHeads OUT@8) |
| `SYSTEM_GET_SUPPORTED` | `0x730120` | `{u32 subDeviceInstance; u32 displayMask; u32 displayMaskDDC}` (12б, OUT@4/@8) |

Контролы шлём `GSP_RM_CONTROL (76)` на hObject=`NV04_DISPLAY_COMMON` (наш objcom),
через `nv_gsp_rm_control`.

**Возможный prereq:** `NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM` на внутренний
subdevice GSP — в nouveau делается до объектов. Для 5A (статичные запросы) сначала
пробуем без него; если GSP вернёт ошибку — добавим (это шаг 5C-prereq).

---

## 3. Что реализовано (framing 5A)

- `driver/gsp/gsp_disp.{c,h}`: `nv_gsp_disp_common_alloc` (`NV04_DISPLAY_COMMON`),
  `nv_gsp_disp_get_num_heads`, `nv_gsp_disp_get_supported`.
- `tools/gsp_disp_test.c` (`make gsp-disp-test`): framing alloc + оба контрола на
  синтетическом канале.
- Оркестратор `tools/gsp_boot_linux.c` (блок «СЛОЙ 5 A0»): alloc disp-common +
  печать numHeads + displayMask/DDC.

**5A 🟢 HW 2026-07-14:** `NV04_DISPLAY_COMMON` alloc `status=NV_OK`; `GET_NUM_HEADS`
→ `heads=4`; `GET_SUPPORTED` → `displayMask=0x7f00` (7 выходов, биты 8–14), `DDC=0x7f00`.
prereq inst-mem НЕ потребовался. Пруф: `docs/hw-dumps/20260714-rtx4070s-layer5-A0-disp-OK.log`.

**5B 🟢 HW 2026-07-14:** `OR_GET_INFO` по всем 7 displayId → все `type=SOR`, протоколы
TMDS_A/B, DP_A/B (idx=~0 — назначается при acquire). `CONNECT_STATE` → `connected=0x300`
(два монитора: 0x100 TMDS_A + 0x200 DP_B). `GET_EDID_V2` для обоих → `size=384`, magic
`00ffffffffffff00` (валидный EDID по DDC). Карта под нашим драйвером **видит мониторы и
читает их EDID**. Пруф: `docs/hw-dumps/20260714-rtx4070s-layer5-B-edid-OK.log`.
**5C.1 🟢 HW 2026-07-14:** `WRITE_INST_MEM` (RAMIN дисплея 0x13400000, 64К, на внутр.
subdevice GSP `0xabcd2080`/client `0xc2000005`) `status=NV_OK`; `AD102_DISP` root alloc
`status=NV_OK`, handle `0xc7700000`. **Дисплейный движок активирован, display root есть.**
Пруф: `docs/hw-dumps/20260714-rtx4070s-layer5-C1-disproot-OK.log`.
**Дальше:** 5C.2 (каналы дисплея core/wndw/curs), 5C.3 (SOR/link training), 5C.4
(framebuffer+methods=картинка).

---

## 4. Источники

- nouveau `nvkm/engine/disp/r535.c` (`r535_disp_oneinit`/`_init`, `r535_outp_new`,
  `r535_conn_new`, `r535_outp_detect`, `r535_tmds_edid_get`, `r535_dp_*`).
- nvrm 535.113.01: `ctrl/ctrl0073/ctrl0073system.h` (NUM_HEADS/SUPPORTED/CONNECT_STATE),
  `ctrl0073specific.h` (OR_GET_INFO/GET_EDID_V2/GET_ALL_HEAD_MASK), `ctrl0073dp.h`
  (DP_GET_CAPS/DP_CTRL/AUXCH), `nvif/class.h`+`g_allclasses.h` (NV04_DISPLAY_COMMON,
  AD102_DISP).
