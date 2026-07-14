# Слой 4 — command submission (каналы FIFO/GR) через GSP-RM

**Статус:** 🟢 **ПРОХОД A НА ЖЕЛЕЗЕ 2026-07-14** (RTX 4070S, Linux/VFIO): A0 (таблица
движков) + A1 (буферы во VRAM) + A2 (channel alloc + BIND + SCHEDULE) — всё `NV_OK`.
Канал `AMPERE_CHANNEL_GPFIFO_A` (CE0) создан и запланирован в runlist.
**Доказательство:** `docs/hw-dumps/20260714-rtx4070s-layer4-passA-chan-OK.log`.
Эталон — nouveau `nvkm/engine/fifo/r535.c` (`r535_chan_ramfc_write`), классы из
`nvif/class.h`, структуры из nvrm 535.113.01 (`alloc/alloc_channel.h`, `ctrl/ctrla06f/*`).

**Результат A0 на железе** (11 движков): GR0 (0x01, runlist 0); COPY0..COPY4
(0x09..0x0d); NVDEC0 (0x1d), NVENC0 (0x26), SEC2 (0x30), OFA (0x3d); SW (0x2c).
CE0 → engineType=0x9, runlist=0 (использован для канала).

**Грабли A2 (решено):** `GPFIFO_SCHEDULE` сперва вернул `status=0x3a`
(`NV_ERR_INVALID_PARAM_STRUCT`) — слали `paramsSize=4`, а
`NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS` = `{NvBool bEnable; NvBool bSkipSubmit}` = **ровно
2 байта**. RM сверяет точный размер структуры контрола. Исправлено на 2 → `NV_OK`.

Слой 3 закрыт (A+B+C+D): двусторонний RPC, RM client/device/subdevice, VA-пространство
(`FERMI_VASPACE_A`), VRAM memlist и прямой GMMU-маппинг (VRAM в GPU VA). Слой 4 —
поднять **канал GPFIFO**, привязать движок (CE/GR), отправить pushbuffer и дождаться
исполнения (семафор). Это первая исполняемая команда GPU.

---

## 0. Модель и метрики (инкрементальные проходы)

В GSP-модели канал живёт на сервере (GSP-RM), а клиент отдаёт ему физ-адреса своих
буферов (instance/RAMFC/USERD во VRAM) и **GPU-VA** кольца GPFIFO (замапленного нашим
прямым GMMU из слоя 3). Порядок ровно как `r535_chan_ramfc_write`:

**Проход A — канал существует и запланирован (метрика 🟢 = SCHEDULE status=NV_OK):**
1. Выделить во VRAM буферы: instance block (RAMFC внутри), USERD, кольцо GPFIFO,
   method-buffer. Замапить GPFIFO в GPU VA (прямой GMMU, слой 3).
2. `GSP_RM_ALLOC` класс `AMPERE_CHANNEL_GPFIFO_A (0xC56F)` под device, hObject=
   `0xf1f00000|chid`, params `NV_CHANNEL_ALLOC_PARAMS` → `status=NV_OK`.
3. `RM_CONTROL NVA06F_CTRL_CMD_BIND (0xa06f0104)` {engineType} на канале.
4. `RM_CONTROL NVA06F_CTRL_CMD_GPFIFO_SCHEDULE (0xa06f0103)` {bEnable=1} → канал в
   runlist. Метрика достигнута.

**Проход B — движок на канале (метрика 🟢 = engine object alloc status=NV_OK):**
5. `GSP_RM_ALLOC` объекта движка на канале: CE `AMPERE_DMA_COPY_B (0xC7B5)` (или
   GR `ADA_A 0xC997`/`ADA_COMPUTE_A 0xC9C0`), hParent=канал.

**Проход C — исполнение команды (метрика 🟢 = семафор во VRAM обновился):**
6. Собрать pushbuffer: методы движка (CE: простой memcopy или SEMAPHORE_RELEASE),
   положить GPFIFO-запись (адрес+длина pushbuffer), обновить USERD GP_PUT, ударить
   в doorbell (`AMPERE_USERMODE_A 0xC561`, регистр VFN). Поллить семафор в VRAM.

Проход A разбит на подшаги: **A0** — engine-info (`FIFO_GET_DEVICE_INFO_TABLE`) для
`engineType`/runlist; **A1** — буферы+VA; **A2** — alloc+bind+schedule.

---

## 1. Классы и хэндлы (сверено с nvif/class.h, v6.11)

| Класс | Значение | Назначение |
|---|---|---|
| `KEPLER_CHANNEL_GROUP_A` | `0xA06C` | TSG (в r535_chan НЕ используется — канал прямо под device) |
| `AMPERE_CHANNEL_GPFIFO_A` | `0xC56F` | канал GPFIFO (Ada = ga102-путь) |
| `AMPERE_USERMODE_A` | `0xC561` | usermode/doorbell (submit, проход C) |
| `AMPERE_DMA_COPY_B` | `0xC7B5` | copy-engine (Ada) |
| `ADA_A` | `0xC997` | 3D/graphics |
| `ADA_COMPUTE_A` | `0xC9C0` | compute |

`ga102_fifo` (nouveau): `.chan = AMPERE_CHANNEL_GPFIFO_A`, `.cgrp =
KEPLER_CHANNEL_GROUP_A`; для GSP идёт `r535_fifo_new`. Наши хэндлы: канал
`0xf1f00000|chid` (как nouveau), движок — свой (`0x0000ce00|inst` для CE и т.п.).

---

## 2. NV_CHANNEL_ALLOC_PARAMS (alloc_channel.h, sizeof=360, compile-probe)

`NV_DECLARE_ALIGNED(..,8)`, `NV_MAX_SUBDEVICES=8`. Смещения (offsetof):
```
hObjectError@0  hObjectBuffer@4  gpFifoOffset@8(u64)  gpFifoEntries@16  flags@20
hContextShare@24  hVASpace@28  hUserdMemory[8]@32  userdOffset[8]@64(u64)
engineType@128  cid@132  subDeviceId@136  hObjectEccError@140
instanceMem@144  userdMem@168  ramfcMem@192  mthdbufMem@216   (NV_MEMORY_DESC_PARAMS 24б)
hPhysChannelGroup@240  internalFlags@244
errorNotifierMem@248  eccErrorNotifierMem@272
ProcessID@296  SubProcessID@300  encryptIv[3]@304  decryptIv[3]@316  hmacNonce[8]@328
```
`NV_MEMORY_DESC_PARAMS` (24б): `base@0(u64) size@8(u64) addressSpace@16 cacheAttrib@20`.
`addressSpace`: 1=SYSMEM, **2=VIDMEM**; `cacheAttrib`: 1 (как nouveau для vidmem).

**flags** (NVOS04, битполя из alloc_channel.h): для kernel-канала (nouveau `.priv=true`):
`PRIVILEGED_CHANNEL_TRUE` (bit5) | `USERD_INDEX_PAGE_FIXED_TRUE` (bit21) |
`USERD_INDEX_VALUE`=userd_i[10:8] | `USERD_INDEX_PAGE_VALUE`=userd_p[20:12]; тип
`CHANNEL_TYPE_PHYSICAL`(0). Для chid=0: `flags = (1<<5)|(1<<21) = 0x00200020`.
`internalFlags` (g_kernel_channel_nvoc.h): PRIVILEGE=USER(0)/ADMIN, ERROR_NOTIFIER_TYPE
=NONE, ECC_ERROR_NOTIFIER_TYPE=NONE. Первый заход `internalFlags=0` (USER, NONE) —
`TODO: verify` на железе.

Заполнение (как r535_chan_ramfc_write): `gpFifoOffset`=GPU VA кольца,
`gpFifoEntries`=len/8, `hVASpace`=наш `0x90f10000`, `engineType`=из device-info,
`instanceMem`{base=inst_phys,size,as=2,ca=1}, `userdMem`{userd_phys,...,as=2,ca=1},
`ramfcMem`{inst_phys+0,0x200,as=2,ca=1}, `mthdbufMem`{mthd_phys,size,as=1/2,ca}.

---

## 3. Контролы канала (ctrla06fgpfifo.h)

| Контрол | cmd | params |
|---|---|---|
| `NVA06F_CTRL_CMD_BIND` | `0xa06f0104` | `{NvU32 engineType}` (4б) |
| `NVA06F_CTRL_CMD_GPFIFO_SCHEDULE` | `0xa06f0103` | `{NvBool bEnable; NvBool bSkipSubmit}` (2б, шлём 4) |

Шлются `GSP_RM_CONTROL (76)` на hObject=канал (наш `nv_gsp_rm_control`). Порядок:
BIND (engineType) → SCHEDULE (bEnable=1). После этого канал в runlist.

---

## 4. engineType и device-info (проход A0)

`engineType` в alloc/BIND — значение `NV2080_ENGINE_TYPE_*` (GR0/COPY0+inst…).
Числовые значения версионны; **правильно брать с железа**, как nouveau
`r535_fifo_runl_ctor`: `RM_CONTROL NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE` на
внутреннем subdevice GSP → таблица движков (runlist/engDesc/RM_ENGINE_TYPE), из неё
берём `engineType`. Для первого HW-прогона можно взять CE0 из таблицы. Реализация
device-info — подшаг A0 (следующий за этим framing-слоем).

---

## 5. Что реализовано в этой сессии (framing, офлайн)

- `driver/gsp/gsp_fifo.{c,h}`: константы классов/контролов, раскладка
  `NV_CHANNEL_ALLOC_PARAMS`, `nv_gsp_rm_channel_alloc` (строит params → `nv_gsp_rm_alloc`
  класс `AMPERE_CHANNEL_GPFIFO_A`), `nv_gsp_rm_channel_bind`,
  `nv_gsp_rm_channel_schedule` (через `nv_gsp_rm_control`).
- `tools/gsp_fifo_test.c` (`make gsp-fifo-test`): compile-probe sizeof/offset
  `NV_CHANNEL_ALLOC_PARAMS` (=360) + framing alloc/bind/schedule на синтетическом канале.

**Проход A 🟢 HW 2026-07-14** (`docs/hw-dumps/20260714-rtx4070s-layer4-passA-chan-OK.log`):
- **A0**: `nv_gsp_fifo_get_device_info` прочитал таблицу движков RTX 4070S (11 шт.):
  GR0(0x01,runl0), SEC2(0x30), NVENC(0x26), NVDEC0(0x1d), **COPY0..COPY4(0x09..0x0d)**,
  OFA/NVJPEG(0x3d), SW(0x2c). CE0 → engineType=0x9, runlist=0.
- **A1**: буферы во VRAM за page-tables прохода D (`pt_base+0x10000`): instance+RAMFC,
  USERD, method-buffer; кольцо GPFIFO — в уже замапленном VA (`va=0x20000000`→`vphys`).
- **A2**: `channel_alloc` (`AMPERE_CHANNEL_GPFIFO_A`, engineType=0x9) `status=NV_OK`
  handle=`0xf1f00000` → `BIND` `NV_OK` → `GPFIFO_SCHEDULE(enable)` `NV_OK`. Канал в runlist.

**Грабли (решено):** `GPFIFO_SCHEDULE` сперва `status=0x3a` (`NV_ERR_INVALID_PARAM_STRUCT`)
— слали `paramsSize=4` вместо ровно 2 (`{NvBool bEnable; NvBool bSkipSubmit}`). RM сверяет
точный размер структуры контрола → фикс на 2 → `NV_OK`.

**Дальше (проход B/C):** B — объект движка CE `AMPERE_DMA_COPY_B (0xC7B5)` на канале
(`nv_gsp_rm_engine_obj_alloc`); C — pushbuffer (CE копия/семафор) + GPFIFO-запись +
USERD GP_PUT + doorbell (`AMPERE_USERMODE_A 0xC561`) + поллинг семафора во VRAM.

---

## 6. Источники (точные)

- nouveau `nvkm/engine/fifo/r535.c` (`r535_chan_ramfc_write`, `r535_chan`,
  `r535_fifo_runl_ctor`, `r535_fifo_2080_type`), `fifo/ga102.c` (`ga102_fifo`:
  chan=AMPERE_CHANNEL_GPFIFO_A, cgrp=KEPLER_CHANNEL_GROUP_A), `include/nvif/class.h`
  (номера классов).
- nvrm 535.113.01: `sdk/nvidia/inc/alloc/alloc_channel.h`
  (`NV_CHANNEL_ALLOC_PARAMS`/`NV_MEMORY_DESC_PARAMS`, флаги NVOS04),
  `sdk/nvidia/inc/ctrl/ctrla06f/ctrla06fgpfifo.h` (BIND/SCHEDULE),
  `sdk/nvidia/inc/nvlimits.h` (`NV_MAX_SUBDEVICES=8`),
  `ctrl/ctrl2080/ctrl2080fifo.h` (`FIFO_GET_DEVICE_INFO_TABLE`).

Реализация: `driver/gsp/gsp_fifo.{c,h}`, офлайн-тест `tools/gsp_fifo_test.c`,
оркестратор (блок «СЛОЙ 4») — `tools/gsp_boot_linux.c` (добавляется на HW-проходе).
