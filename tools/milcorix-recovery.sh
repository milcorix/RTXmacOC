#!/bin/sh
# milcorix-recovery.sh — отключить kext MilcorixFB + вытащить крэш-логи.
# ЗАПУСКАТЬ В macOS RECOVERY (Utilities -> Terminal):
#   diskutil mount disk0s1
#   sh /Volumes/*/milcorix-recovery.sh
#
# Делает две вещи:
#   1) переименовывает AuxiliaryKernelExtensions.kc на Data-томе (kext больше не грузится);
#   2) копирует panic-репорты и наши kext-логи на EFI (/Volumes/EFI/milcorix-logs),
#      чтобы прочитать их из Linux и найти реальную причину падения.
# Ничего не удаляет (AuxKC только -> .disabled), APFS-запись родным diskutil.
set -u
echo "========================================"
echo "   MILCORIX RECOVERY"
echo "========================================"

# Каталог, откуда запущен скрипт = корень EFI (туда же сложим логи)
EFIDIR=$(dirname "$0")
LOGDIR="$EFIDIR/milcorix-logs"
mkdir -p "$LOGDIR" 2>/dev/null

# 1. Найти Data-том macOS
echo ">> ищу Data-том..."
DATADEV=$(diskutil apfs list | grep -i "(Data)" | grep -oE "disk[0-9]+s[0-9]+" | head -1)
if [ -z "$DATADEV" ]; then
  echo "!! Data-том НЕ найден. Полный список ниже — сфоткай:"
  diskutil apfs list
  exit 1
fi
echo "   Data-том: $DATADEV"

# 2. Смонтировать read-write
diskutil mount "$DATADEV" >/dev/null 2>&1
MP=$(diskutil info "$DATADEV" | sed -n 's/^[[:space:]]*Mount Point:[[:space:]]*//p')
if [ -z "$MP" ] || [ ! -d "$MP" ]; then
  echo "!! не смог смонтировать $DATADEV"
  exit 1
fi
echo "   смонтирован: $MP"

# 3. СНАЧАЛА собрать крэш-логи (пока том смонтирован) --------------------------
echo ">> собираю крэш-логи в $LOGDIR ..."
# 3a. Kernel panic-репорты (.panic / .ips — текст/JSON, читаются из Linux)
for d in "$MP/Library/Logs/DiagnosticReports" "$MP/Library/Logs/CrashReporter"; do
  if [ -d "$d" ]; then
    cp "$d"/*.panic "$LOGDIR"/ 2>/dev/null
    cp "$d"/*.ips   "$LOGDIR"/ 2>/dev/null
  fi
done
# 3b. Персистентный unified-log (бинарь .tracev3 — распарсим позже на macOS)
if [ -d "$MP/var/db/diagnostics" ]; then
  # только заголовок + Persist (последние срезы), чтобы не тащить гигабайты
  mkdir -p "$LOGDIR/diagnostics" 2>/dev/null
  cp "$MP/var/db/diagnostics/Persist/"*.tracev3 "$LOGDIR/diagnostics/" 2>/dev/null
  cp "$MP/var/db/diagnostics/"*.* "$LOGDIR/diagnostics/" 2>/dev/null
fi
# 3c. Что реально впечено в staged-extensions (подтвердить наш kext)
ls -la "$MP/Library/StagedExtensions/Library/Extensions/" > "$LOGDIR/staged-extensions.txt" 2>/dev/null
diskutil apfs list > "$LOGDIR/diskutil-apfs.txt" 2>/dev/null
CNT=$(ls -1 "$LOGDIR" 2>/dev/null | wc -l | tr -d ' ')
echo "   скопировано элементов: $CNT (папка milcorix-logs на EFI)"

# 4. Найти AuxKC
KC="$MP/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
if [ ! -f "$KC" ]; then
  KC=$(find "$MP/Library/KernelCollections" -maxdepth 2 -name "AuxiliaryKernelExtensions.kc" 2>/dev/null | head -1)
fi
if [ -z "$KC" ] || [ ! -f "$KC" ]; then
  echo "!! AuxKC не найден — возможно, уже отключён. Содержимое:"
  ls -la "$MP/Library/KernelCollections/" 2>/dev/null
  # логи всё равно собрали — это не фатально
  echo "   (логи собраны, можешь ребутнуться в Linux и показать их мне)"
  exit 1
fi
SZ=$(ls -la "$KC" | awk '{print $5}')
echo "   найден AuxKC: $KC ($SZ байт)"

# 5. Переименовать AuxKC
NEW="$KC.disabled"
[ -e "$NEW" ] && NEW="$KC.disabled.bak"
if mv "$KC" "$NEW"; then
  sync
  echo ""
  echo "########################################"
  echo "#  ГОТОВО.                              #"
  echo "#  - kext отключён (AuxKC -> .disabled) #"
  echo "#  - крэш-логи на EFI/milcorix-logs     #"
  echo "#                                       #"
  echo "#  Дальше выбери ОДНО:                  #"
  echo "#  (A) reboot -> обычный macOS (грузится)#"
  echo "#  (B) reboot -> Linux, покажи логи мне #"
  echo "########################################"
else
  echo "!! ОШИБКА mv (том read-only / FileVault?). Сфоткай экран."
  exit 1
fi
