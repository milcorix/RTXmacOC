#!/usr/bin/env bash
#
# run-fwsec-detached.sh — прогон FWSEC-FRTS БЕЗ ребута, с гарантированным возвратом экрана.
#
# Карта одна и рисует GNOME. Чтобы отдать её в VFIO, надо освободить от nvidia. Мешает
# то, что nvidia_drm держит консольный fb0. Поэтому: гасим gdm → отцепляем fbcon →
# выгружаем nvidia → ГАРД (если карта не освободилась — откат) → vfio → харнесс →
# возврат nvidia → gdm. Любой выход (даже ошибка) поднимает GUI обратно.
#
# ЗАПУСКАТЬ ОТЦЕПЛЕННО (иначе умрёт вместе с экраном):
#   sudo systemd-run --unit=rtx-fwsec --collect bash /ABS/PATH/tools/run-fwsec-detached.sh
#
# Результат: tools/fwsec-frts.log ; маркер: tools/fwsec-DONE
set -u

DIR="$(cd "$(dirname "$0")" && pwd)"
LOG="$DIR/fwsec-frts.log"
HARNESS_SRC="$DIR/fwsec_run_linux"
HARNESS="/tmp/fwsec_run_linux"
BDF="0000:01:00.0"
AUD="0000:01:00.1"

exec > "$LOG" 2>&1

drv_of() { basename "$(readlink "/sys/bus/pci/devices/$1/driver" 2>/dev/null)" 2>/dev/null; }

restore_gui() {
    echo "-- [restore] вернуть GPU nvidia и поднять gdm --"
    for fn in "$BDF" "$AUD"; do
        [ -e "/sys/bus/pci/devices/$fn" ] || continue
        if [ -e "/sys/bus/pci/devices/$fn/driver" ] && [ "$(drv_of "$fn")" = "vfio-pci" ]; then
            timeout 15 bash -c "echo '$fn' > /sys/bus/pci/devices/$fn/driver/unbind" 2>/dev/null || true
        fi
        echo "" > "/sys/bus/pci/devices/$fn/driver_override" 2>/dev/null || true
    done
    modprobe -r vfio_pci vfio_iommu_type1 vfio 2>/dev/null || true
    modprobe nvidia_drm 2>/dev/null || modprobe nvidia 2>/dev/null || true
    modprobe snd_hda_intel 2>/dev/null || true
    sleep 2
    echo "   card driver now: $(drv_of "$BDF")  audio: $(drv_of "$AUD")"
    systemctl start gdm 2>/dev/null || true
    echo "=== DONE $(date -u +%FT%TZ) ==="
    touch "$DIR/fwsec-DONE"
}
trap restore_gui EXIT

echo "=== run-fwsec-detached $(date -u +%FT%TZ) ==="
echo "kernel: $(uname -r)"
[ "$(id -u)" -eq 0 ] || { echo "ERR: нужен root"; exit 1; }
[ -n "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ] || { echo "ERR: IOMMU не активен"; exit 1; }
[ -f "$HARNESS_SRC" ] || { echo "ERR: нет $HARNESS_SRC"; exit 1; }

echo "-- грейс 6с перед гашением экрана --"
sleep 6

# 1. Погасить GUI и всё, что держит GPU.
echo "-- stop gdm + gpu-процессы --"
systemctl stop gdm 2>/dev/null || true
systemctl stop nvidia-persistenced 2>/dev/null || true
sleep 2
fuser -k /dev/nvidia* /dev/dri/* 2>/dev/null || true
pkill -9 -x gnome-shell 2>/dev/null || true
pkill -9 -x Xwayland 2>/dev/null || true
sleep 2

# 2. Отцепить консольный fbcon от GPU (иначе nvidia_drm не выгрузится).
echo "-- unbind fbcon (vtcon) --"
for v in /sys/class/vtconsole/vtcon*/bind; do
    [ -e "$v" ] || continue
    if grep -qi "frame buffer" "$(dirname "$v")/name" 2>/dev/null; then
        echo 0 > "$v" 2>/dev/null || true
        echo "   отцепил $(cat "$(dirname "$v")/name" 2>/dev/null)"
    fi
done
echo 0 > /sys/class/graphics/fbcon/bind 2>/dev/null || true
sleep 1

# 3. Выгрузить nvidia (с таймаутом, чтобы не зависнуть навсегда).
echo "-- unload nvidia --"
for m in nvidia_drm nvidia_modeset nvidia_uvm nvidia; do
    timeout 15 modprobe -r "$m" 2>/dev/null || true
done
sleep 1
cur="$(drv_of "$BDF")"
echo "   driver of $BDF after unload: ${cur:-(none)}"

# 4. ГАРД: если карта всё ещё за nvidia — НЕ лезть в отвязку (она вешает). Откат.
if [ "$cur" = "nvidia" ]; then
    echo "ABORT: nvidia не освободила карту (что-то держит fb/контекст)."
    echo "       Безопасно откатываюсь — нужен boot со свободной GPU (см. free-gpu.sh)."
    exit 2
fi

# 5. Привязать к vfio-pci.
modprobe vfio-pci 2>/dev/null || true
for fn in "$BDF" "$AUD"; do
    [ -e "/sys/bus/pci/devices/$fn" ] || continue
    if [ -e "/sys/bus/pci/devices/$fn/driver" ]; then
        timeout 15 bash -c "echo '$fn' > /sys/bus/pci/devices/$fn/driver/unbind" 2>/dev/null || true
    fi
    echo vfio-pci > "/sys/bus/pci/devices/$fn/driver_override" 2>/dev/null || true
    echo "$fn" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
    echo "   $fn -> $(drv_of "$fn")"
done

# 6. Прогон харнесса.
cp -f "$HARNESS_SRC" "$HARNESS"; chmod +x "$HARNESS"
echo "=== HARNESS ==="
timeout 60 "$HARNESS" "$BDF"
echo "=== harness rc=$? ==="

# 7. restore_gui() из trap EXIT поднимет всё обратно.
exit 0
