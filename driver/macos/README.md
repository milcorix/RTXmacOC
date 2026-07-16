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
