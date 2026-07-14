# Слой 3 — двусторонний RPC к GSP-RM: A+B+C на железе; D = прямой GMMU (RPC-путь = тупик)

**Статус:** 🟢 проход A (2026-06-30) + проходы B, C (2026-07-01) на RTX 4070 Super
(AD104), Linux/VFIO. Проход D через `MAP_MEMORY_DMA (14)` **проверен на железе
2026-07-14 и отвергнут GSP** (`rpc_result=0x2a NV_ERR_INVALID_FUNCTION`) — этот RPC
не диспетчеризуется в GSP-offload модели (см. §4D). Дальше D переписывается на
**прямой GMMU-маппинг** (host-side page-tables, как nouveau `vmm`).
**Доказательство:** `docs/hw-dumps/20260630-rtx4070s-layer3-rpc-OK.log` (A),
`docs/hw-dumps/20260701-rtx4070s-layer3-passB-OK.log` (B),
`docs/hw-dumps/20260701-rtx4070s-layer3-passC-vram-OK.log` (C),
`docs/hw-dumps/gsp-boot-layer3-passD-20260714.log` (D — отрицательный, fn=14 отвергнут).
**Оркестратор:** `tools/gsp_boot_linux.c` (тот же прогон, что и слой 2; блоки
«СЛОЙ 3»/«3B»/«3C»/«3D»).
**Портируемая логика:** `driver/gsp/gsp_rm.{c,h}`. Офлайн-тест: `tools/gsp_rm_test.c`
(`make gsp-rm-test`). Эталон — nouveau `r535.c`, nvrm-заголовки 535.113.01.

Этот документ — запись того, как после `GSP_INIT_DONE` (слой 2) поднимается
**двусторонний RPC** и делаются первые RPC слоя 3. Если регрессует — здесь детали.

---

## 0. Проходы и метрики

Слой 2 дал односторонний канал: cmdq до бута + дренаж событий из msgq. Слой 3
(память/GMMU/VRAM) — это набор RPC к живому GSP-RM, и для него нужен
**двусторонний RPC**: послать команду пост-бут, дождаться типизированного ответа.

**Проход A** (фундамент RPC + RM-клиент):
1. `GET_GSP_STATIC_INFO (65)`: GSP вернул `rpc_result=0` и **карту FB-регионов VRAM**.
   Двусторонний RPC работает + карта памяти GPU.
2. `GSP_RM_ALLOC (103)`: цепочка client→device→subdevice, `status=NV_OK(0)`.
   Наш RM-клиент на GPU — фундамент для аллокаций.

**Проход B** (память через RPC):
3. `GSP_RM_CONTROL (76)` / `FB_GET_INFO_V2 (0x20801303)`: `status=NV_OK`, реальный
   конфиг VRAM/heap (RAM=12282 МиБ, HEAP, BAR1=256 МиБ). Третий RPC-глагол работает.
4. `GSP_RM_ALLOC` `FERMI_VASPACE_A (0x90f1)`: `status=NV_OK` — **GPU VA-пространство
   (корень GMMU)** создано через RPC.

**Проход C** (регистрация VRAM):
5. `ALLOC_MEMORY (4)` / `NV01_MEMORY_LIST_FBMEM (0x82)`: `rpc_result=NV_OK` —
   **зарегистрирован физический VRAM-диапазон** (1 МиБ по phys=0x13100000). В
   GSP-модели VRAM'ом владеет гость: он даёт список физ. страниц (не GSP из кучи).

**Проход D** (VRAM → GPU VA; ❌ RPC-путь отвергнут железом, переписывается):
6. `MAP_MEMORY_DMA (14)` с `hDma=NV01_MEMORY_VIRTUAL`, `hMemory=VRAM memlist`:
   на железе GSP вернул `rpc_result=0x2a` (`NV_ERR_INVALID_FUNCTION`) — функция не
   диспетчеризуется. Промежуточные шаги прошли (`NV01_MEMORY_VIRTUAL` создан,
   `status=0`), но сам маппинг по RPC в GSP-модели невозможен (§4D). Верный путь —
   прямой GMMU (host пишет PDE/PTE во VRAM).

После реализации D (прямой GMMU) — RM-control'ы FIFO/GR для слоя 4 (каналы).

---

## 1. Реальный результат на железе (проходы A/B)

После `GSP_INIT_DONE` (msgq.writePtr=7): канал `cmdq.wptr=2 seq=2 msgq.rptr=7`.

`GET_GSP_STATIC_INFO` → 5 FB-регионов (вершина = размер VRAM):
```
FB[0] 0x0..0x30fffff           rsvd               (резерв внизу)
FB[1] 0x3100000..0x2f034ffff   comp=1 iso=1       ← большой пользовательский VRAM
FB[2] 0x2f0350000..0x2f528ffff comp=1 iso=1
FB[3] 0x2f5290000..0x2f52fffff rsvd
FB[4] 0x2f5300000..0x2ff9fffff                    ← limit+1 = 0x2ffa00000 = 12282 МиБ ✓
GSP internal: hClient=0xc2000005 hDevice=0xabcd0080 hSubdevice=0xabcd2080
              bar1Pde=0x2f3ca9000 bar2Pde=0x2f5293000
```
RM-цепочка:
```
RM client  status=0x0 handle=0xc1d00000
RM device  status=0x0 handle=0xde1d0000
RM subdev  status=0x0 handle=0x5d1d0000
```
Проход B (тот же канал, продолжение):
```
FB_GET_INFO_V2 status=0 (KiB): RAM=12576768 (=12282 МиБ) TOTAL_RAM=12576768
               HEAP=12355136 HEAP_FREE=57720 BAR1=262144 (=256 МиБ)
FERMI_VASPACE_A status=0 handle=0x90f10000       ← корень GMMU
```
Проход C:
```
VRAM memlist phys=0x13100000 size=0x100000 rpc_result=0 handle=0x00ca0001  ← 1 МиБ VRAM
```
`msgq.tx.writePtr`: 7 → 11 (A) → 13 (B) → 14 (C: memlist, +1). На каждый RPC — 1 ответ.
Для D ожидается 15, но это ещё не подтверждено на железе.

---

## 2. Двусторонний RPC — `nv_gsp_rpc_call` (порт r535_gsp_cmdq_push + msg_recv)

Канал `nv_gsp_rpc_chan` держит `shm`, раскладку очередей, `cmdq_wptr`, `seq`,
`msgq_rptr` и колбэки `ring`(звонок 0xc00=0)/`udelay` (переносимость, как `nv_mmio_t`).

**Send:** строим элемент в `cmdq + entryOff + cmdq_wptr*0x1000` через
`nv_gsp_cmdq_write` (тот же, что у пре-бутовых RPC: rpc-заголовок + XOR-checksum +
elem_count). Продвигаем `cmdq_wptr += elem_count` (wrap по msgCount=63), пишем
`cmdq.tx.writePtr`, звоним в звонок, `seq++`.

**Recv (poll до 4с):** пока `msgq_rptr != msgq.tx.writePtr`: читаем элемент в слоте,
берём `elem_count`(@+40), `function`(@rpc+12), `length`(@rpc+8), `rpc_result`(@rpc+16).
Если `function==fn` — копируем payload (`length-32` байт) в выход, отдаём
`rpc_result`, двигаем `msgq_rptr += elem_count` (consume через `msgq.rx.readPtr`).
Иначе — async-событие GSP (0x101b/0x100c): пропускаем с consume и ждём дальше.
Периодически звоним в звонок.

**Продолжение указателей после INIT_DONE — критично:** `cmdq_wptr`/`seq` берём с
тех, что оставил пре-бут (writePtr=2, seq=2); `msgq_rptr` — финальный readPtr дренажа
(7). Не сбрасывать в 0 — иначе рассинхрон с GSP.

---

## 3. GET_GSP_STATIC_INFO (65) — `nv_gsp_get_static_info`

Запрос = `sizeof(GspStaticConfigInfo)=2168` нулей (GSP заполняет ответ, как
`nvkm_gsp_rpc_rd`). Ответ той же функции, `rpc_result=0`. Парсим по смещениям
(получены **compile-probe против SDK 535.113.01**, см. `gsp_rm.c` «PROBE»):
```
sizeof(GspStaticConfigInfo) = 2168
fbRegionInfoParams @840 : numFBRegions@0 (u32), fbRegion[16]@8 (48б/регион)
   регион: base@0 limit@8 reserved@16 performance@24 supportCompressed@28
           supportISO@29 bProtected@30
bar1PdeBase @2088, bar2PdeBase @2096 (u64)
hInternalClient @2152, hInternalDevice @2156, hInternalSubdevice @2160
```
Элемент req/reply = 48+32+2168 = 2248 ≤ 4096 → **одностраничный** (ec=1).
**Самопроверка на железе:** `numFBRegions∈[1..16]`; карта согласована с известным
размером VRAM (вершина FB[4]+1 = 0x2ffa00000). Неверное смещение → мусор → ловим.

---

## 4. GSP_RM_ALLOC (103) — `nv_gsp_rm_alloc` + конструкторы цепочки

`rpc_gsp_rm_alloc_v03_00` (шапка 32б, g_rpc-structures.h):
```
hClient@0 hParent@4 hObject@8 hClass@12 status@16 paramsSize@20 flags@24 reserved[4]@28 params@32
```
req payload = шапка + params; ответ — та же шапка с `status` (NV_OK==0).
Канонические хэндлы/классы (ровно как nouveau r535_gsp_*_ctor):

| объект | hObject | hParent | hClass | params (sizeof) |
|---|---|---|---|---|
| client (root) | `0xc1d00000` | self | `NV01_ROOT` (0x0) | NV0000{hClient, processID=~0, name[100]} (108) |
| device | `0xde1d0000` | client | `NV01_DEVICE_0` (0x80) | NV0080{hClientShare=client, …} (56) |
| subdevice | `0x5d1d0000` | device | `NV20_SUBDEVICE_0` (0x2080) | NV2080{subDeviceId=0} (4) |

Делаем последовательно (ждём ответ каждого до следующего), `status` каждого = 0.

---

## 4B. Проход B — RM_CONTROL + FB_GET_INFO_V2 + FERMI_VASPACE_A

### GSP_RM_CONTROL (76) — `nv_gsp_rm_control` (порт r535_gsp_rpc_rm_ctrl_get/push)
`rpc_gsp_rm_control_v03_00` (шапка 24б, g_rpc-structures.h):
```
hClient@0 hObject@4 cmd@8 status@12 paramsSize@16 flags@20 params@24
```
params — **IN/OUT**: запрос копируется из буфера вызывающего, ответ GSP пишется
обратно в тот же буфер. `status` (@12 ответа) = NV_OK(0). Тот же двусторонний
`nv_gsp_rpc_call` с fn=76.

### FB_GET_INFO_V2 (0x20801303) — `nv_gsp_fb_get_info`
params = `NV2080_CTRL_FB_GET_INFO_V2_PARAMS` (436б): `fbInfoListSize@0` (u32),
`fbInfoList[54]@4` — каждый `NV2080_CTRL_FB_INFO = {index(u32), data(u32)}` (8б).
Заполняем `index` для каждого запрошенного, `data=0`; GSP возвращает `data`.
Индексы (значения в KiB): RAM_SIZE=0x07, TOTAL_RAM_SIZE=0x08, HEAP_SIZE=0x09,
HEAP_FREE=0x16, BAR1_SIZE=0x05, USABLE_RAM_SIZE=0x20.
На железе: RAM=12576768 KiB = ровно 12282 МиБ (совпало с PMC/GET_STATIC_INFO).
Контрол шлём на **наш** subdevice (`0x5d1d0000`). cmd номер — из полного
`ctrl2080fb.h` (OGK 535.113.01; в nouveau-выжимке отсутствовал, поэтому дотянут
из того же публичного репозитория).

### FERMI_VASPACE_A (0x90f1) — `nv_gsp_rm_vaspace_ctor`
RM_ALLOC под device (hParent=device, hObject=`0x90f10000`, hClass=0x90f1).
params = `NV_VASPACE_ALLOCATION_PARAMETERS` (nvos.h, 48б):
```
index@0 flags@4 vaSize@8 vaStartInternal@16 vaLimitInternal@24 bigPageSize@32 vaBase@40
```
Заполняем `index=NV_VASPACE_ALLOCATION_INDEX_GPU_NEW(0)`, прочее 0 → дефолтное
новое полное VA-пространство. На железе: `status=NV_OK`. Это корень GMMU —
основа для маппинга VRAM в GPU-адреса (следующий шаг).

---

## 4C. Проход C — регистрация VRAM (NV01_MEMORY_LIST_FBMEM via ALLOC_MEMORY)

**Модель (важно): в GSP VRAM'ом владеет ГОСТЬ.** GSP-RM НЕ выделяет VRAM гостю из
своей кучи. Гость выбирает физический диапазон (из usable FB-региона карты
GET_STATIC_INFO) и **регистрирует список его физ. страниц** как memlist. Эталон —
nouveau `fbsr_memlist` (`instmem/r535.c`).

`nv_gsp_rm_vram_memlist` шлёт `ALLOC_MEMORY (4)` с `rpc_alloc_memory_v13_01`
(g_rpc-structures.h):
```
hClient@0 hDevice@4 hMemory@8 hClass@12 flags@16 pteAdjust@20 format@24
length@32(u64) pageCount@40 pteDesc@44
```
- `hClass = NV01_MEMORY_LIST_FBMEM (0x82)`.
- `flags = 0x40000200` = NVOS02 PHYSICALITY_CONTIGUOUS(0)@7:4 | LOCATION_VIDMEM(2)@11:8
  | MAPPING_NO_MAP(1)@31:30.
- `format = 6` (NV_MMU_PTE_KIND_GENERIC_MEMORY), `length = size`, `pageCount = size>>12`.
- `struct pte_desc` (sdk-structures.h): u32-битполе (idr:2, reserved1:14, **length:16**)
  @44 → `length=pages` кладётся в старшие 16 бит; затем `pte_pde[]` (u64, 8-выровнен) @48,
  где `pte_pde[i] = (phys>>12) + i` (contiguous).
Успех = `rpc_result == 0` (обычный RPC, без внутреннего status). На железе: физ.
диапазон 0x13100000 (256 МиБ вглубь usable FB-региона), 1 МиБ = 256 страниц → NV_OK.
Реализация держит size ≤ ~3.9 МиБ (один страничный элемент cmdq).

---

## 4D. Проход D — маппинг VRAM в GPU VA

### 4D.1 Заход через `MAP_MEMORY_DMA (14)` — 🔴 ТУПИК на железе (HW 2026-07-14)

Первая гипотеза: смаппить VRAM-memlist в VA-пространство одним RPC
`MAP_MEMORY_DMA (14)` через промежуточный объект `NV01_MEMORY_VIRTUAL` (0x70,
`hDma`), как это делает RM-путь `virtmemMapTo`. Цепочка построилась, но сам
маппинг GSP отверг:
```
СЛОЙ 3C: VRAM memlist        rc=0 rpc_result=0x0 handle=0x00ca0001   OK
СЛОЙ 3D: NV01_MEMORY_VIRTUAL rc=0 status=0x0     handle=0x00700001   OK
СЛОЙ 3D: MAP_MEMORY_DMA      rc=-3 rpc_result=0x2a status=0xffffffff GPU_VA=0x0   FAIL
```
`rpc_result=0x2a` = **`NV_ERR_INVALID_FUNCTION`** («Called function is not valid»).
Внутренний `NVOS46.status` остался `0xffffffff` — GSP до разбора параметров НЕ дошёл.
Это ошибка **диспетчера RPC**, а не логики маппинга.

Фрейминг сверен байт-в-байт с родным `rpcMapMemoryDma_v03_00` (rpc.c, nvrm
535.113.01): функция 14, `NVOS46_PARAMETERS_v03_00` (56б: `hClient@0 hDevice@4
hDma@8 hMemory@12 offset@16 length@24 flags@32 dmaOffset@40 status@48`) — раскладка
верна. Дело не в ней.

**Корневая причина** (по исходникам OGK 535.113.01, `virtual_mem.c:465-474`):
```c
// Skip RPC to the Host RM when local RM is managing page tables.
bRpcAlloc = !(gpuIsSplitVasManagementServerClientRmEnabled(pGpu) || (bSriovFull && ...));
```
`NV_RM_RPC_MAP_MEMORY_DMA` шлётся к firmware **только на пути vGPU/SR-IOV** (когда
таблицами страниц управляет host RM). В **GSP-offload модели страничными таблицами
управляет CPU-side RM локально** (`dmaAllocMap` → прямая запись GMMU PTE во VRAM),
и функция 14 GSP-диспетчером не обслуживается вовсе. Отсюда `INVALID_FUNCTION`.

Это подтверждает раннюю находку: **nouveau тоже не использует `MAP_MEMORY_DMA` RPC**
— его собственный `nvkm_vmm` пишет PDE/PTE сам. Вывод: RPC-путь к GPU VA —
архитектурный тупик; правильный путь ниже.

### 4D.2 Прямой GMMU-маппинг (текущий трек)

VA получаем, управляя таблицами страниц GMMU напрямую со стороны хоста (как
`nouveau vmm` / RM `bar2_walk.c`): выделить page-tables во VRAM, записать иерархию
PDE→…→PTE для Ada, направить корень на наше VA-пространство. Код
`nv_gsp_rm_map_memory_dma`, объект `NV01_MEMORY_VIRTUAL` и блок «3D» остаются в
дереве как задокументированный отрицательный результат; статус 🔴, критерий 🟢
переносится на прямой GMMU-путь.

**Подтверждение выполнимости (разведка 2026-07-14).** Что модель верна и работает
на нашем железе с уже имеющимся BAR0:

- **Кто пишет PTE.** Даже на GSP-client (`IS_GSP_CLIENT`) страничные таблицы
  бутстрапит **CPU-side RM**, не GSP. Пруф: OGK `bar2_walk.c:231` — *«Must use the
  BAR0 window to directly write to the physical [memory]»*, `:847` — *«Set PRAMIN
  window offset to the page needed»*. Это ровно тот путь, что мы выбрали.
- **Как CPU пишет в VRAM (у нас маплен только BAR0).** Через окно **PRAMIN**:
  регистр `NV_PBUS_BAR0_WINDOW` = **`0x1700`** (Maxwell→Ada, `dev_bus.h`), поле
  `_BASE` = `bits[23:0]` = `physVidAddr >> 16`, поле `_TARGET` = `bits[25:24]`
  (`0`=VID_MEM, `2`=SYS_COH, `3`=SYS_NONCOH). Данные окна — по `BAR0 + 0x700000`
  (1 МиБ). Alias-логика (nouveau `nv50.c`, RM `bar2_walk.c:855`): аимить окно на
  `phys & ~(winSize-1)`, писать по `BAR0 + 0x700000 + (phys - winBase)`, при выходе
  за окно — переаимить. BAR0 у нас = 16 МиБ (маплен харнессом), окно доступно.
- **Формат GMMU для Ada = Ampere `ga10x`** (нет отдельного файла Ada ни в OGK, ни
  в nouveau — `tu102_vmm`/`ga10x` переиспользуются). Радикс из nouveau
  `gp100_vmm_desc_12` (4К-лист): уровни сверху вниз
  **PD3(2 бита)→PD2(9)→PD1(9)→PD0(8, dual-PTE 16 байт)→SPT(9, 4К-страницы)**;
  крупные листы (`desc_16`, 64К/2М) — через `LPT`. Все PT выровнены на `0x1000`.
- **Раскладка дескриптора** (`nvkm_vmm_desc`): `{type, bits, size(байт/PTE), align}`.
  `desc_12 = { {SPT,9,8,0x1000}, {PGD,8,16,0x1000}, {PGD,9,8,0x1000},
  {PGD,9,8,0x1000}, {PGD,2,8,0x1000} }`. SPT-PTE = 8 байт, PD0-запись = 16 байт
  (dual: small+big), верхние PD = 8 байт.
- **Биты PTE** (nouveau `gp100_vmm_pgt_pte` + `gp100_vmm_valid`), 64-битное слово:
  `VALID=bit0`; `APERTURE=bits[2:1]` (0=VID_MEM, 1=SYS_COH через PEER, см. `aper()`);
  `VOL=bit3`; `PRIV=bit5`; `RO(read-only)=bit6`; `ADDR=(phys>>4)` в средних битах
  (лежит начиная с bit8, т.к. `data=(addr>>4)|type`); `KIND=bits[63:56]` (для
  простого линейного VRAM — `kind=0`, pitch). Инкремент: `data += (1<<pageShift)>>4`.
- **Биты PDE** (`gp100_vmm_pde`): `APERTURE=bits[2:1]` (1=VRAM, 2=SYS_COH+VOL,
  3=SYS_NONCOH); адрес нижней таблицы `= (pt_phys>>4)`; PD0 пишется как 128-битная
  пара (`VMM_WO128`), верхние PD — 64-битные (`VMM_WO064`). Sparse = `VOL_BIG(bit3)`
  при `VALID=0`.
- **Куда указывает корень.** PD3 (верхний уровень) прописывается в **instance
  block** нашего VA-пространства (`FERMI_VASPACE_A`, уже создан в §4B). Точный
  механизм привязки корня к VASPACE-объекту в GSP-модели — уточнить (варианты:
  `RM_CONTROL` FIFO/`fifoGetVaspacePageDir`, либо чтение `bar2Pde/GET_GSP_STATIC_INFO`
  как образца формата). Это единственная открытая точка перед кодом.

Разложение VA (4К-страницы, 49-битный VA): индексы уровней от старших к младшим —
PD3 `[48:47]`(2), PD2 `[46:38]`(9), PD1 `[37:29]`(9), PD0 `[28:21]`(8), SPT
`[20:12]`(9), смещение `[11:0]`. (Соответствует `page.shift`-таблице nouveau
`tu102`: 47/38/29/21/16/12.)

---

## 5. Тупики/нюансы (чтобы не потерять)

- **VRAM НЕ аллоцируется через `NV01_MEMORY_LOCAL_USER` (0x40) + `NV_MEMORY_ALLOCATION_PARAMS`
  из кучи GSP** — первый заход так делал и GSP отверг на уровне RPC (`rpc_result≠0`, НЕ
  внутренний status; `sizeof(params)=120` был верен — дело не в размере). Правильно:
  гость выбирает физ. страницы и регистрирует memlist (`NV01_MEMORY_LIST_FBMEM`, §4C).
  Диагностика заняла один HW-прогон; не повторять неверную модель.

- **VRAM НЕ маппится в GPU VA через `MAP_MEMORY_DMA (14)` RPC** — на железе GSP
  отвечает `rpc_result=0x2a` (`NV_ERR_INVALID_FUNCTION`), функция не диспетчеризуется
  (§4D.1). Это путь vGPU/SR-IOV; в GSP-offload таблицами управляет host RM локально.
  Не крутить номер функции/раскладку — фрейминг верен, проблема архитектурная.
  Правильно: прямой GMMU (§4D.2). Не повторять RPC-модель маппинга.

- **`cmdq.rx.readPtr` остаётся 0** в диагностике — это НЕ значит, что GSP не читает
  cmdq. Факт обработки виден по росту `msgq.tx.writePtr` (7→11) и `status=0` ответов.
  Не «чинить».
- **Указатели канала продолжаются** после INIT_DONE (см. §2). Сброс = рассинхрон.
- **Смещения GspStaticConfigInfo версионны** (535.113.01). При смене версии прошивки
  пере-снять compile-probe. Самопроверка FB-карты — страховка.
- Ответы этого прохода ≤ 1 страницы; multi-page сборка реализована и протестирована
  офлайн (`tools/gsp_rm_test.c`), но на железе пока не задействована.

---

## 6. Источники (точные)

- nouveau `r535.c`: `r535_gsp_cmdq_push`, `r535_gsp_msgq_wait/recv`,
  `r535_gsp_msg_recv`, `r535_gsp_rpc_send`, `r535_gsp_rpc_rm_alloc_get/push`,
  `r535_gsp_rpc_rm_ctrl_get/push`, `r535_gsp_client_ctor`/`device_ctor`/
  `subdevice_ctor`, `r535_gsp_rpc_get_gsp_static_info`/`postinit`;
  `instmem/r535.c` `fbsr_memlist` (ALLOC_MEMORY + NV01_MEMORY_LIST_FBMEM).
- nvrm 535.113.01: `g_rpc-structures.h` (rpc_gsp_rm_alloc/control_v03_00),
  `rpc_global_enums.h` (MAP_MEMORY_DMA=14, GET_GSP_STATIC_INFO=65,
  GSP_RM_CONTROL=76, GSP_RM_ALLOC=103),
  `cl0000.h`/`cl0080.h`/`cl2080.h` (классы+alloc-params), `cl90f1.h`
  (FERMI_VASPACE_A=0x90f1), `nvos.h` (NV_VASPACE_ALLOCATION_PARAMETERS),
  `nvlimits.h` (NV_PROC_NAME_MAX_LENGTH=100), `gsp_static_config.h`
  (GspStaticConfigInfo), `ctrl2080fb.h` (FB_REGION_INFO; FB_GET_INFO_V2=0x20801303
  + индексы — из полного OGK-заголовка 535.113.01), `cl84a0.h`
  (NV01_MEMORY_LIST_FBMEM=0x82), `g_rpc-structures.h` (rpc_alloc_memory_v13_01),
  `sdk-structures.h` (pte_desc), `nvos.h` (NVOS02/NVOS46 flags),
  `g_sdk-structures.h` (`NVOS46_PARAMETERS_v03_00`), `rpc.c`
  (`rpcMapMemoryDma_v03_00`).
- **Разведка прямого GMMU (§4D.2):** nouveau `nvkm/subdev/mmu/vmmgp100.c`
  (`gp100_vmm_pgt_pte`/`pd0_pte`/`pde`/`pd0_pde`/`pd1_pde`/`gp100_vmm_valid`,
  `gp100_vmm_desc_12`/`_16`), `vmmtu102.c`/`vmmgv100.c` (page-таблица shift,
  переиспользуют `gp100_vmm_desc`), `vmm.h` (`nvkm_vmm_desc`, enum PGD/PGT/SPT/LPT);
  `nvkm/subdev/instmem/nv50.c` (`nv50_instmem_set_bar0_window_addr` → рег `0x001700`,
  окно `0x700000`); OGK 535.113.01 `kernel/gpu/mmu/bar2_walk.c` (CPU пишет PTE через
  BAR0-окно даже на GSP-client), `kernel/gpu/bus/arch/maxwell/kern_bus_gm107.c`
  (`kbusSetBAR0WindowVidOffset_GM107`), `swref/.../tu102/dev_bus.h`
  (`NV_PBUS_BAR0_WINDOW` 0x1700: `_BASE` 23:0=addr>>16, `_TARGET` 25:24),
  `mmu/arch/ampere/kern_gmmu_fmt_ga10x.c` (формат Ada = ga10x).

Реализация: `driver/gsp/gsp_rm.{c,h}`, очереди `driver/gsp/gsp_rpc.{c,h}`,
оркестратор `tools/gsp_boot_linux.c` (блоки «СЛОЙ 3»/«3B»/«3C»/«3D»).
