# Слой 5 — дисплей / modeset через GSP-RM

**Статус:** 🟢 **5A+5B+5C.1..5C.4b НА ЖЕЛЕЗЕ 2026-07-14..15** (RTX 4070S, Linux/VFIO).
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
  `docs/hw-dumps/20260715-rtx4070s-layer5-C4b-fb-OK.log`; (c) core methods (raster/OR/
  control из EDID-таймингов) + `UPDATE`; (d) window methods (surface) + `UPDATE` →
  **картинка**. Тайминги — из EDID (прочитан в 5B). Для DP-монитора дополнительно
  `NV0073_CTRL_CMD_DP_CTRL` (link training).

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
