#!/bin/bash
#
# macos_uninstall.sh — полностью убрать MilcorixFB с мака.
# ЗАПУСКАТЬ НА macOS:  sudo tools/macos_uninstall.sh [--keep-fw]
#
# Нужен, когда на машине остался СТАРЫЙ kext (тот, что читал boot-arg'ов и
# подключался всегда). Новый по умолчанию выключен, но чистая переустановка
# всё равно надёжнее правки поверх.
set -uo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "нужны права root: sudo tools/macos_uninstall.sh"
    exit 1
fi

KEEP_FW=0
[ "${1:-}" = "--keep-fw" ] && KEEP_FW=1

echo "=== выгрузка (если загружен) ==="
kextunload -b dev.milcorix.MilcorixFB 2>/dev/null && echo "выгружен" || echo "не был загружен"

echo ""
echo "=== удаление бандлов ==="
for p in /Library/Extensions/MilcorixFB.kext \
         /System/Library/Extensions/MilcorixFB.kext; do
    if [ -e "$p" ]; then rm -rf "$p"; echo "удалён $p"; fi
done

echo ""
echo "=== очистка staging и пересборка коллекции kext'ов ==="
# clear-staging выкидывает kext из AuxKC — именно это делает его «незагружаемым
# на следующем boot'е». Без этого бандл удалён, а копия в AuxKC осталась бы.
RC_STAGING=0; RC_INSTALL=0
kmutil clear-staging 2>&1 | tail -3 || RC_STAGING=$?
kmutil install --update-all --volume-root / 2>&1 | tail -5 || RC_INSTALL=$?

if [ "$KEEP_FW" -eq 0 ]; then
    echo ""
    echo "=== прошивки ==="
    rm -rf "/Library/Application Support/Milcorix/fw"
    echo "удалены (журнал lastboot.log оставлен)"
fi

echo ""
if [ "$RC_STAGING" -ne 0 ] || [ "$RC_INSTALL" -ne 0 ]; then
    echo "############################################################"
    echo "#  ВНИМАНИЕ: kmutil вернул ошибку (staging=$RC_STAGING install=$RC_INSTALL)."
    echo "#  Kext мог остаться в загрузочной коллекции. Проверь вывод"
    echo "#  выше и при необходимости повтори; аварийный путь —"
    echo "#  boot-arg milcorix=0 из Linux (tools/milcorix_stage.py 0)."
    echo "############################################################"
    exit 1
fi

echo "############################################################"
echo "#  MilcorixFB удалён. Перезагрузись — экран пойдёт через    #"
echo "#  штатный EFI-фреймбуфер.                                  #"
echo "#                                                          #"
echo "#  Не забудь убрать boot-arg, если он выставлен:           #"
echo "#    из Linux:  sudo python3 tools/milcorix_stage.py 0     #"
echo "############################################################"
