#!/bin/sh
# milcorix-fixpreboot.sh — быстрая попытка починить загрузку БЕЗ переустановки.
# Пересобирает Preboot/граф криптекса (dyld shared cache). ЗАПУСКАТЬ В macOS RECOVERY:
#   diskutil mount disk2s1
#   sh /Volumes/*/milcorix-fixpreboot.sh
set -u
echo "##################################################"
echo "#  MILCORIX: repair Preboot (updatePreboot)      #"
echo "##################################################"

echo ">> раскладка APFS:"
diskutil apfs list | grep -E "Volume (disk|.*Role)" 2>/dev/null | head -40

# 1. Data-том (для полной пересборки Preboot нужен разблокированный Data)
DATA=$(diskutil apfs list | grep -i "(Data)" | grep -oE 'disk[0-9]+s[0-9]+' | head -1)
if [ -z "$DATA" ]; then echo "!! Data-том не найден"; diskutil apfs list; exit 1; fi
echo ""
echo ">> Data = $DATA — введи пароль FileVault (скрытый ввод):"
diskutil apfs unlockVolume "$DATA" || echo "  (возможно уже разблокирован)"

# 2. System-том
SYS=$(diskutil apfs list | grep -i "(System)" | grep -oE 'disk[0-9]+s[0-9]+' | head -1)
if [ -z "$SYS" ]; then echo "!! System-том не найден"; diskutil apfs list; exit 1; fi
echo ">> System = $SYS"
diskutil mount "$SYS" 2>/dev/null || true

# 3. Пересобрать Preboot для группы томов
echo ""
echo ">>> diskutil apfs updatePreboot $SYS"
diskutil apfs updatePreboot "$SYS"
RC=$?

echo ""
if [ "$RC" -eq 0 ]; then
  echo "##################################################"
  echo "#  updatePreboot OK.                              #"
  echo "#  reboot -> обычный macOS. Если загрузится —     #"
  echo "#  победа. Если снова петля — криптекс битый,     #"
  echo "#  нужен Reinstall (скажи мне).                   #"
  echo "##################################################"
else
  echo "##################################################"
  echo "#  updatePreboot FAILED (rc=$RC). Сфоткай вывод.  #"
  echo "##################################################"
fi