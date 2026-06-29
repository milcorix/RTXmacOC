#!/usr/bin/env bash
#
# revert-iommu.sh — вернуть GRUB как было до setup-iommu.sh (восстановить nouveau/десктоп).
#   sudo bash tools/revert-iommu.sh ; reboot
set -euo pipefail

GRUB=/etc/default/grub
BAK="$GRUB.rtxbak"

if [ "$(id -u)" -ne 0 ]; then echo "ERR: запусти через sudo"; exit 1; fi
[ -f "$BAK" ] || { echo "ERR: нет бэкапа $BAK — нечего восстанавливать"; exit 1; }

cp -a "$BAK" "$GRUB"
echo "Восстановлен $GRUB из $BAK"
update-grub
echo "ГОТОВО. Сделай reboot — вернётся nouveau и обычный десктоп."
