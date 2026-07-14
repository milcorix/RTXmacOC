# Слой 5 — дисплей / modeset через GSP-RM

**Статус:** 🟢 **5A+5B НА ЖЕЛЕЗЕ 2026-07-14** (RTX 4070S, Linux/VFIO): 5A —
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

**5C — modeset + scanout (метрика 🟢 = картинка на мониторе, нужен монитор+FB):**
5. RAMIN (inst 0x10000 во VRAM) + `NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM`
   (на внутренний subdevice GSP, hInternalSubdevice из GET_STATIC_INFO).
6. `GSP_RM_ALLOC` display root класс **`AD102_DISP (0xC770)`** (Ada) под device.
7. Core/window/cursor каналы дисплея (классы `*7d/*7e/*7b/*7a`), framebuffer-surface
   во VRAM (наш GMMU), attach к head, для DP — link training (`NV0073_CTRL_CMD_DP_CTRL`),
   для HDMI — `SPECIFIC_SET_HDMI_ENABLE`. Выставить mode из EDID, включить scanout.

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
**Дальше:** 5C — modeset+scanout (display root `AD102_DISP 0xC770` + inst-mem + core/wndw
каналы + framebuffer + link training + выставить mode из EDID = картинка на мониторе).

---

## 4. Источники

- nouveau `nvkm/engine/disp/r535.c` (`r535_disp_oneinit`/`_init`, `r535_outp_new`,
  `r535_conn_new`, `r535_outp_detect`, `r535_tmds_edid_get`, `r535_dp_*`).
- nvrm 535.113.01: `ctrl/ctrl0073/ctrl0073system.h` (NUM_HEADS/SUPPORTED/CONNECT_STATE),
  `ctrl0073specific.h` (OR_GET_INFO/GET_EDID_V2/GET_ALL_HEAD_MASK), `ctrl0073dp.h`
  (DP_GET_CAPS/DP_CTRL/AUXCH), `nvif/class.h`+`g_allclasses.h` (NV04_DISPLAY_COMMON,
  AD102_DISP).
