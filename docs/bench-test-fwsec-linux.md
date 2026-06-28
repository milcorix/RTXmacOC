# bench-test-fwsec-linux.md — HW-верификация FWSEC-FRTS на Linux (VFIO), headless

> Быстрый путь к первому 🟢 для слоя 2 **без macOS и без монитора**. Гоняем тот же
> портируемый код, что и kext (`driver/gsp/*`), но из Linux-харнесса `tools/fwsec_run_linux.c`
> через VFIO. Windows на целевой машине не трогаем — грузимся с USB.
>
> Метрика: в логе одновременно `mbox0=0x00000000` и `WPR2 set=1`. Только такой лог,
> положенный в `docs/hw-dumps/`, даёт право на 🟢 (Правило 0 / R11).

## Почему так (и роль второй машины)

- Целевая машина (i5-12400F + RTX, **без iGPU**) под macOS/Linux даёт чёрный экран —
  драйвера вывода для RTX нет. Поэтому работаем **headless по SSH**.
- Для реального доступа к BAR0+DMA карта должна принадлежать ОС на голом железе через
  `vfio-pci` (нельзя из VM/WSL поверх Windows — единственная GPU занята хостом).
- Linux грузится headless тривиально, в отличие от macOS. Установка ОС не нужна — live-USB.
- **Haswell (с iGPU)** нужен только чтобы один раз подготовить и проверить загрузочную
  флешку (есть картинка). Потом та же флешка едет в целевую машину и грузится вслепую.

## Рабочий цикл (батч, не живой дебаг)

1. Подготовка (со мной): собрать харнесс, флешку.
2. Прогон на стенде: загрузка с USB → `vfio-pci` → запуск → лог на флешку/по SSH.
3. Разбор: лог присылаешь мне → если FAIL, чиню → пересборка → повтор.

Я нужен на шагах 1 и 3, не в реальном времени. Управлять стендом можно с любого
компьютера в той же сети (в т.ч. с Haswell, где открыт Kiro и SSH-клиент).

---

## 0. BIOS (один раз, на целевой машине)

- Включить **VT-d / Intel Virtualization Technology for Directed I/O** (IOMMU).
- (Желательно) Above 4G Decoding — On.
- Windows от этого не страдает.

## 1. Подготовить live-USB (на Haswell, есть картинка)

Подойдёт **Ubuntu 24.04 Server/Desktop live** или Arch — что привычнее. Нужно, чтобы
на флешке были: `gcc`/`clang`, `git`, `openssh-server`, и чтобы при загрузке
поднимался SSH с известным логином/паролем.

1. Записать ISO на USB (Rufus/balenaEtcher — можно прямо из Windows).
2. Загрузиться с неё **на Haswell** (там есть дисплей), убедиться, что система стартует.
3. Поднять SSH и задать пароль:
   ```sh
   sudo systemctl enable --now ssh        # ubuntu: openssh-server
   sudo passwd ubuntu                      # задать пароль для входа
   ip a                                    # запомнить интерфейс (для стенда — Ethernet)
   ```
4. С другого компа в сети проверить `ssh ubuntu@<ip>`. Если заходит — флешка готова.
   (Для live-сессий, где изменения не сохраняются, удобнее persistent-USB или просто
   повторять эти 2 команды после каждой загрузки.)

> Цель шага: одна и та же флешка, которая на целевой машине поднимет SSH сама, чтобы
> на стенд её можно было воткнуть вслепую.

## 2. Загрузить целевую машину headless

1. Воткнуть флешку и Ethernet в i5-12400F, включить (монитор не нужен).
2. Подождать ~1–2 мин, найти IP (по DHCP-таблице роутера или `arp -a`).
3. Зайти `ssh ubuntu@<ip>` с рабочего ПК.

## 3. Привязать RTX к vfio-pci

```sh
lspci -nnk | grep -A3 -i nvidia
# Запомнить BDF и ID. Пример:
#   01:00.0 VGA ... [10de:2783]   (GPU)
#   01:00.1 Audio ... [10de:22bc] (HDMI-аудио той же карты)
```

Обе функции карты (`01:00.0` и `01:00.1`) обычно в одной IOMMU-группе — **обе** надо
отдать vfio-pci, иначе группа «не viable».

```sh
sudo modprobe vfio-pci

# отвязать от текущего драйвера (nouveau/snd_hda), если привязаны:
for f in 0000:01:00.0 0000:01:00.1; do
  echo "$f" | sudo tee /sys/bus/pci/devices/$f/driver/unbind 2>/dev/null
  echo vfio-pci | sudo tee /sys/bus/pci/devices/$f/driver_override
  echo "$f" | sudo tee /sys/bus/pci/drivers/vfio-pci/bind
done

# проверить, что обе функции теперь на vfio-pci:
lspci -nnk -s 01:00
ls /dev/vfio/      # должна появиться нода группы (число)
```

Если nouveau цепко держит карту — добавь в cmdline загрузки
`modprobe.blacklist=nouveau` (через GRUB live-USB) и `intel_iommu=on`. На single-GPU
машине это лишит локальной консоли — но мы и так headless по SSH.

## 4. Собрать и запустить харнесс

```sh
git clone https://github.com/prd1324/RTXmacOC.git
cd RTXmacOC
cc -Wall -Wextra -O2 tools/fwsec_run_linux.c \
   driver/gsp/falcon.c driver/gsp/fwsec_patch.c \
   driver/gsp/fwsec_locate.c driver/gsp/fb_layout.c \
   -o fwsec_run_linux

sudo ./fwsec_run_linux 0000:01:00.0 | tee fwsec-run.txt
```

> Компиляцию харнесса можно заранее проверить в WSL (`linux/vfio.h` там есть). Сам
> прогон — только на голом Linux со стенда.

## 5. Что ждём в логе (критерий успеха)

```
VFIO: BAR0 смаплен, size=0x1000000
VFIO: Memory Space + Bus Master включены ...
DMA-буфер: VA=... IOVA=0x10000000 size=0x100000
PMC_BOOT_0 = 0x194000a1
GFW boot completed = 1
VRAM usable = 12288 MiB (0x300000000 байт)
FRTS region addr=0x<...> size=0x100000
VBIOS ROM-shadow: первые байты 55 aa (ожидается 55 AA)
FWSEC desc @ vbios+0x<...>
desc: version=3 ... sig_count=... 
signature: fuse_ver=... idx=... @ vbios+0x<...>
Falcon reset OK
Falcon dma_load OK
FWSEC done: mbox0=0x00000000 mbox1=0x... WPR2 set=1 [0x<lo>..0x<hi>]
*** FWSEC-FRTS OK, WPR2 создан ***
=== РЕЗУЛЬТАТ: OK (WPR2 создан) ===
```

**Метрика достигнута**, если `mbox0=0x00000000` И `WPR2 set=1` с непустым диапазоном.

## 6. Сохранить результат

```sh
# скопировать лог на dev-машину:
scp ubuntu@<ip>:~/RTXmacOC/fwsec-run.txt \
    docs/hw-dumps/$(date +%Y%m%d)-rtx4070s-fwsec-frts-linux.log
```

После этого можно ставить 🟢 для портируемой логики слоя 2 (явно: «Доказательство:
docs/hw-dumps/<файл>»). macOS-kext-шим остаётся 🟡 до прогона на самой macOS.

## 7. Диагностика по симптомам

| Симптом | Причина | Действие |
|---|---|---|
| `readlink(... iommu_group)` ошибка | IOMMU выключен или карта не на vfio-pci | включить VT-d + `intel_iommu=on`; §3 |
| `группа не viable` | не все функции карты на vfio-pci | привязать и `01:00.1` (аудио) тоже |
| `нет TYPE1_IOMMU` | VT-d off / нет `intel_iommu=on` | BIOS + cmdline |
| `первые байты` не `55 aa`, а `4e 56`(`NVGI`) | ROM начинается с IFR-заголовка (D3) | прислать дамп — добавим разбор IFR в `fwsec_locate.c` |
| `GFW boot completed = 0` | карта не завершила свой boot | подождать после включения; проверить питание карты (доп. 12V) |
| `VRAM usable = 0` | `NV_USABLE_FB_SIZE_IN_MB` пуст | вероятно GFW не прошёл; см. выше |
| `reset Falcon` FAIL | таймаут скраба/выбора ядра | прислать лог: печатается `HWCFG2` |
| `dma_load` FAIL | DMA/IOMMU или выравнивание | прислать лог: печатается `DMATRFCMD`; проверить IOVA-маппинг |
| `boot timeout` | Falcon не дошёл до halted | вероятна неверная подпись/BROM; в логе `mbox0/mbox1` |
| `mbox0 != 0` | FWSEC вернул код ошибки | значение = код; сверить fuse/подпись |
| `WPR2 set=0` при `mbox0=0` | WPR2 не выставлен | редкость; перечитать `0x1FA824/828` |

## 8. Безопасность/обратимость

- Харнесс **не пишет на диск** и не трогает Windows-раздел. Live-USB изолирован.
- FWSEC сбрасывает GSP-Falcon и создаёт WPR2 в VRAM — состояние карты, сбрасывается
  обычной перезагрузкой/выключением. Необратимого ничего не делает.
- Для повторного прогона после FAIL лучше перезагрузить стенд (чистое post-GFW
  состояние карты), затем заново §3–§4.
