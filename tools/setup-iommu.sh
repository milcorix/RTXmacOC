#!/usr/bin/env bash
#
# setup-iommu.sh — включить IOMMU и освободить RTX от nouveau для HW-прогона FWSEC.
# Делает ОДНУ обратимую правку GRUB (бэкап рядом) и регенерит конфиг. Нужен root.
#
#   sudo bash tools/setup-iommu.sh
#   → reboot
#   → sudo bash tools/bench-vfio-run.sh   (получить лог)
#   → sudo bash tools/revert-iommu.sh ; reboot   (вернуть систему как было)
#
# Параметры, которые добавляем в командную строку ядра:
#   intel_iommu=on iommu=pt   — поднять VFIO/IOMMU (boot-time, иначе никак);
#   modprobe.blacklist=nouveau — чтобы nouveau не держал единственную GPU
#                                (десктоп уйдёт на simpledrm/efifb — это норма).
set -euo pipefail

GRUB=/etc/default/grub
BAK="$GRUB.rtxbak"
ADD="intel_iommu=on iommu=pt modprobe.blacklist=nouveau"

if [ "$(id -u)" -ne 0 ]; then echo "ERR: запусти через sudo"; exit 1; fi
[ -f "$GRUB" ] || { echo "ERR: нет $GRUB"; exit 1; }

# Бэкап один раз.
[ -f "$BAK" ] || cp -a "$GRUB" "$BAK"
echo "Бэкап: $BAK"

# Уже добавлено?
if grep -q "intel_iommu=on" "$GRUB"; then
    echo "Параметры уже присутствуют в $GRUB — пропускаю правку."
else
    # Дописать ADD внутрь кавычек GRUB_CMDLINE_LINUX_DEFAULT='...'.
    python3 - "$GRUB" "$ADD" <<'PY'
import sys, re
path, add = sys.argv[1], sys.argv[2]
s = open(path).read()
def patch(m):
    q = m.group(2)              # кавычка ' или "
    body = m.group(3).strip()
    return f'{m.group(1)}{q}{(body + " " + add).strip()}{q}'
s2, n = re.subn(r'(GRUB_CMDLINE_LINUX_DEFAULT=)(["\'])(.*?)\2', patch, s)
if n == 0:
    raise SystemExit("не нашёл GRUB_CMDLINE_LINUX_DEFAULT")
open(path, "w").write(s2)
print("OK: добавлено ->", add)
PY
fi

echo "--- новая строка ---"
grep -nE "GRUB_CMDLINE_LINUX_DEFAULT" "$GRUB"

echo "--- регенерация grub.cfg ---"
update-grub

echo
echo "ГОТОВО. Теперь:"
echo "  1) reboot"
echo "  2) после загрузки: cd ~/Документы/RTXmacOC/probe && sudo bash tools/bench-vfio-run.sh"
echo "     (экран может погаснуть — лог пишется в tools/fwsec-frts.log)"
echo "  3) проверь: ls -A /sys/kernel/iommu_groups | wc -l  — должно быть > 0."
echo "     Если 0 — в BIOS включи VT-d (Intel VT for Directed I/O) и повтори."
