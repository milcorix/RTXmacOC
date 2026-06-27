# RTXmacOC — Дорожная карта, Месяц 1 (Beachhead)

> **Цель месяца:** не поднять карту, а доказать, что мы можем с ней **говорить**
> из macOS — смапить BAR0 и прочитать `PMC_BOOT_0` (chip ID Ada).
> Пробьём эту стену — дальше дело техники и времени.

Контекст и обоснование пути — см. [ARCHITECTURE.md](./ARCHITECTURE.md).

---

## Неделя 1 — Цель и фундамент

**Результат:** воспроизводимое окружение + реальные данные карты.

> **Исполнителю:** пошаговый разбор этой недели с точными командами, ожидаемым
> выводом и критериями готовности — в [week1-runbook.md](./week1-runbook.md).
> Шаблоны артефактов уже заведены: `docs/testbed.md`, `docs/hw-dumps/`,
> скелеты `skeletons/EmptyKext/` и `skeletons/EmptyDext/`, корневой `Makefile`.

- [x] Зафиксировать таргет: **x86 Hackintosh под OpenCore**, RTX как основная карта.
      → зафиксировано в `docs/testbed.md` и ARCHITECTURE.md.
- [~] Собрать `pcie_probe` на реальном хакинтоше с RTX 4070 Super:
      `clang pcie_probe.c -framework IOKit -framework CoreFoundation -o pcie_probe`
      → **компилируется в CI** (macOS-раннер, GitHub Actions). Запуск на реальном
      железе ждёт macOS на ПК (нужна вторая GPU/USB-загрузка для вывода).
- [~] Снять реальный вывод: BAR0/BAR1/BAR3, subsystem-id, revision-id, class-code.
      → сняты **из Windows**: device `0x2783`, subsys `1569:F302`, rev `0xA1`,
      class `0x030000`, **BAR0=0x52000000 (16MB)**, BAR1=0x40000000 (2GB),
      BAR3=0x50000000 (32MB). См. `docs/hw-dumps/`, `docs/testbed.md`.
      Снятие через `pcie_probe` на самой macOS — когда поднимем macOS на ПК.
- [x] Поставить Xcode + SDK; проверить сборку kext и dext.
      → **GitHub Actions, macOS-раннер.** kext `RTXProbe` собирается (✅),
      dext — best-effort (iig на Xcode 26.5 допиливаем). Вместо «пустышек» —
      **реальные драйверы** (матчинг 10DE:2783 + маппинг BAR0 + чтение PMC_BOOT_0).
- [x] Завести тестовый стенд (версия macOS, конфиг OpenCore, статус SIP) — в `docs/testbed.md`.
      → заполнено по Windows-части; macOS/OpenCore/SIP — после поднятия macOS.

## Неделя 2 — Карта графического стека macOS

**Результат:** документ, как macOS видит и обслуживает GPU.

- [ ] Разобрать цепочку: IOPCIFamily → PCIDriverKit → IOGraphics/IOAccelerator.
- [ ] Отметить, что открыто (XNU, IOKit, IOGraphics частично) и что закрыто (Metal).
- [ ] Таблица: модель доступа на каждой версии macOS (kext vs dext, требования SIP).
- [ ] Решить: целевой минимум macOS-версии для первой фазы.
- [ ] Документ: `docs/macos-graphics-stack.md`.

## Неделя 3 — Карта стороны NVIDIA + первый dext

**Результат:** понимание GSP-инициализации + матчащийся dext.

- [ ] Разобрать open-gpu-kernel-modules: путь инициализации GSP, загрузка прошивки,
      минимальная последовательность до «карта ожила». Конспект в `docs/gsp-notes.md`.
- [ ] Найти определения регистров Ada (`PMC_BOOT_0` и др.) — занести в заголовок проекта.
- [ ] Написать минимальный **PCIDriverKit dext**, который матчится на `10DE:2783`.
- [ ] Заставить dext успешно загрузиться и приматчиться к карте (без чтения пока).

## Неделя 4 — Closing the loop (пробитие стены)

**Результат:** dext читает chip ID из железа.

- [ ] dext мапит BAR0 и читает `PMC_BOOT_0` (offset 0x0), декодит architecture/chip id.
- [ ] Вывести «вижу Ada AD104» из самого dext. ← **главный результат месяца.**
- [ ] Architecture Decision Record зафиксировать (путь = GSP-портирование).
- [ ] Структура репо + `CONTRIBUTING.md`, чтобы команда подключалась.
- [ ] Ретро месяца: что подтвердилось, что оказалось сложнее, план на месяц 2.

---

## Definition of Done месяца 1

1. `pcie_probe` отработал на реальном железе, лог приложен.
2. Есть документированная карта стека macOS и стороны NVIDIA (GSP).
3. **Драйвер (kext или dext) читает `PMC_BOOT_0` из BAR0 и печатает chip ID Ada.**
4. Путь и риски зафиксированы в ARCHITECTURE.md, репо готов к контрибьюторам.

Если п.3 закрыт — стена пробита. Это и есть то, до чего «никто не добрался».
