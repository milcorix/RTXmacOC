#!/usr/bin/env bash
#
# bench-vfio-run.sh — обёртка прогона FWSEC-FRTS на стенде (см. docs/bench-test-fwsec-linux.md).
#
# Делает три вещи одной командой, чтобы не вводить руками на стенде:
#   1. проверяет, что IOMMU активен;
#   2. привязывает обе функции карты (видео 01:00.0 + аудио 01:00.1) к vfio-pci;
#   3. запускает харнесс и пишет ВЕСЬ вывод в лог-файл (на флешку), чтобы результат
#      пережил возможное гашение локальной консоли после захвата BAR'ов vfio.
#
# Запуск (root), лог сразу на смонтированную Ventoy-флешку:
#   sudo ./bench-vfio-run.sh 0000:01:00.0 /mnt/ventoy/fwsec-frts.log ./fwsec_run_linux
#
# Все три аргумента опциональны (см. значения по умолчанию ниже).
set -u

BDF="${1:-0000:01:00.0}"
LOG="${2:-./fwsec-frts.log}"
HARNESS="${3:-./fwsec_run_linux}"

# Весь блок ниже дублируется в $LOG через tee. Файловый вывод не зависит от
# состояния экрана, поэтому лог сохранится даже если консоль погаснет.
{
    echo "=== bench-vfio-run $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
    echo "BDF=$BDF  HARNESS=$HARNESS  LOG=$LOG"
    echo "kernel: $(uname -r)"

    if [ "$(id -u)" -ne 0 ]; then
        echo "ERR: нужен root (sudo)."
        exit 1
    fi

    # 1. IOMMU активен?
    if [ -z "$(ls -A /sys/kernel/iommu_groups 2>/dev/null)" ]; then
        echo "ERR: IOMMU не активен. Включи VT-d в BIOS и добавь в cmdline 'intel_iommu=on'."
        echo "     (в Ventoy/GRUB на загрузке: e -> к строке linux добавить intel_iommu=on)"
        exit 1
    fi
    echo "IOMMU: активен ($(ls /sys/kernel/iommu_groups | wc -l) групп)"

    if [ ! -x "$HARNESS" ]; then
        echo "ERR: харнесс '$HARNESS' не найден/не исполняемый (chmod +x)."
        exit 1
    fi

    modprobe vfio-pci 2>/dev/null || true

    # 2. Привязать обе функции карты к vfio-pci.
    base="${BDF%.*}"   # напр. 0000:01:00
    for fn in "${base}.0" "${base}.1"; do
        [ -e "/sys/bus/pci/devices/$fn" ] || continue
        echo "--- $fn ---"
        if [ -e "/sys/bus/pci/devices/$fn/driver" ]; then
            cur="$(basename "$(readlink "/sys/bus/pci/devices/$fn/driver")")"
            echo "  текущий драйвер: $cur -> unbind"
            echo "$fn" > "/sys/bus/pci/devices/$fn/driver/unbind" 2>/dev/null || true
        fi
        echo vfio-pci > "/sys/bus/pci/devices/$fn/driver_override" 2>/dev/null || true
        echo "$fn" > /sys/bus/pci/drivers/vfio-pci/bind 2>/dev/null || true
        if [ -e "/sys/bus/pci/devices/$fn/driver" ]; then
            now="$(basename "$(readlink "/sys/bus/pci/devices/$fn/driver")")"
        else
            now="none"
        fi
        echo "  теперь драйвер: $now"
    done

    echo "=== запуск харнесса (после этого локальная картинка может погаснуть — это норма, лог пишется в файл) ==="
    "$HARNESS" "$BDF"
    rc=$?
    echo "=== харнесс завершился, код=$rc ==="
} 2>&1 | tee "$LOG"

# sync, чтобы лог точно лёг на флешку перед перезагрузкой.
sync
echo
echo "Готово. Лог: $LOG  (перезагрузись в Windows и пришли этот файл)."
