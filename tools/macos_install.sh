#!/bin/bash
#
# macos_install.sh — собрать и установить MilcorixFB на целевом маке одной командой.
# ЗАПУСКАТЬ НА macOS из корня репозитория:  sudo tools/macos_install.sh [FW_DIR]
#
# Что делает:
#   1. собирает загружаемый бандл MilcorixFB.kext (ld -kext + kmod_info);
#   2. раскладывает прошивки GSP в /Library/Application Support/Milcorix/fw;
#   3. ставит kext в /Library/Extensions и отдаёт его в AuxKC.
#
# ВАЖНО: после установки драйвер ОСТАЁТСЯ ВЫКЛЮЧЕННЫМ. Он читает boot-arg
# `milcorix=N` и без него не подключается вообще. Это сделано намеренно: установка
# драйвера дисплея не должна создавать риск остаться без картинки. Включение —
# отдельным осознанным шагом (см. вывод в конце).
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

if [ "$(id -u)" -ne 0 ]; then
    echo "нужны права root: sudo tools/macos_install.sh"
    exit 1
fi

FW_SRC="${1:-}"
KEXT_DST="/Library/Extensions/MilcorixFB.kext"
FW_DST="/Library/Application Support/Milcorix/fw"

echo "=== 1/4 сборка kext ==="
rm -rf build/MilcorixFB.kext
tools/build_milcorixfb_kext.sh build

echo ""
echo "=== 2/4 прошивки GSP ==="
if [ -n "$FW_SRC" ]; then
    # Явно указанный каталог имеет приоритет: пользователь передал его именно
    # затем, чтобы обновить прошивки, а не чтобы услышать «уже разложены».
    tools/install_milcorix_fw.sh "$FW_SRC"
elif [ -d "$FW_DST" ] && [ -n "$(ls -A "$FW_DST" 2>/dev/null)" ]; then
    echo "уже разложены в $FW_DST:"
    ls -la "$FW_DST"
else
    echo "!! прошивок нет и каталог-источник не указан."
    echo "   Нужны три файла (linux-firmware 535.113.01):"
    echo "     booter_load-535.113.01.bin   55928 байт"
    echo "     bootloader-535.113.01.bin    32876 байт"
    echo "     gsp-535.113.01.bin        38061600 байт"
    echo "   Положи их в каталог и повтори:  sudo tools/macos_install.sh <каталог>"
    exit 1
fi

echo ""
echo "=== 3/4 установка kext ==="
rm -rf "$KEXT_DST"
cp -R build/MilcorixFB.kext "$KEXT_DST"
chown -R root:wheel "$KEXT_DST"
chmod -R 755 "$KEXT_DST"
echo "поставлен: $KEXT_DST"

echo ""
echo "=== 4/4 отдать загрузчику kext'ов ==="
# --no-authorization обходит интерактивное одобрение (rc=27) — оно недоступно,
# пока драйвер дисплея ещё не даёт картинку. rc=28 = «нужен ребут», это норма.
set +e
kmutil load --no-authorization -p "$KEXT_DST"
RC=$?
set -e
case "$RC" in
    0)  echo "kext загружен немедленно";;
    28) echo "kext подготовлен, применится после перезагрузки (rc=28 — ожидаемо)";;
    *)  echo "kmutil вернул rc=$RC — смотри вывод выше";;
esac

echo ""
echo "############################################################"
echo "#  УСТАНОВЛЕНО. Драйвер сейчас ВЫКЛЮЧЕН (это норма).       #"
echo "#                                                          #"
echo "#  Включать по стадиям, по одной за перезагрузку:          #"
echo "#    milcorix=1  поднять GSP, вывод НЕ трогать             #"
echo "#    milcorix=2  полный вывод через нашу карту             #"
echo "#                                                          #"
echo "#  Стадия задаётся boot-arg'ом. Проще всего из Linux:      #"
echo "#    sudo python3 tools/milcorix_stage.py 1                #"
echo "#                                                          #"
echo "#  Журнал каждой загрузки (читается и из Linux):           #"
echo "#    /Library/Application Support/Milcorix/lastboot.log    #"
echo "############################################################"
