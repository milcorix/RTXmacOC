# milcorix — macOS kext (Фаза 1: картинка через IOFramebuffer)

Цель: НЕускоренный вывод рабочего стола macOS через нашу RTX 4070S. Это отдельный от
Metal путь (Library Validation его не трогает — см. `docs/macos-display-plan.md`).

## Архитектура
- `MilcorixFB` — subclass `IOFramebuffer`. Публикует режимы (из EDID) и scanout-апертуру
  для WindowServer; `setDisplayMode` дергает наш GSP-modeset.
- Ядро драйвера (`driver/gsp/*`) переносимо через колбэки `nv_mmio_t` (ctx/rd/wr/udelay).
  В kext колбэки — обёртки над `IOPCIDevice`/`IOMemoryMap` (BAR0), `IODelay`.
- GSP-boot (слои 2–4) + modeset (слой 5) — тот же код, что в `tools/gsp_boot_linux.c`,
  но `nv_mmio_t` смотрит на BAR0 через IOKit.

## Статус
- 🔧 Скелет класса + matching + маппинг BAR (компилируется вне железа условно).
- Prereq: Фаза 0 — голова должна физически сканировать (общий блокер с Linux-треком).

## Сборка (на macOS, позже)
Xcode/kext SDK, `IOKit` framework. Matching на PCI `10DE:2783` (RTX 4070 Super, Ada AD104).
На старте — грузим с SIP off для отладки; подпись не требуется для FB-kext (в отличие от
Metal-бандла), но для загрузки стороннего kext на Big Sur+ нужен режим сниженной
безопасности (Reduced Security) / `kmutil`.

## Прошивка GSP (стена 2)
Оркестратор (`MilcorixFB::gspBringUp` → переносимый `nv_gsp_bringup`) грузит подписанные
блоба NVIDIA через `nv_fw_blob_get` (шим `fw_blob_macos.cpp`, BSD vnode API). В ядре macOS
нет `popen`/zstd, поэтому блоба кладутся на диск **уже распакованными** (в git не
коммитятся — правило 7):

```
/Library/Application Support/Milcorix/fw/booter_load-535.113.01.bin
/Library/Application Support/Milcorix/fw/booter_unload-535.113.01.bin
/Library/Application Support/Milcorix/fw/bootloader-535.113.01.bin
/Library/Application Support/Milcorix/fw/gsp-535.113.01.bin
```

Развернуть из linux-firmware-эквивалента (распаковка zstd делается заранее, вне ядра):
```sh
sudo mkdir -p "/Library/Application Support/Milcorix/fw"
for n in booter_load booter_unload bootloader gsp; do
  zstd -dc "<src>/$n-535.113.01.bin.zst" \
    | sudo tee "/Library/Application Support/Milcorix/fw/$n-535.113.01.bin" >/dev/null
done
```
