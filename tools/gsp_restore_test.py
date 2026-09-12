#!/usr/bin/env python3
"""OFFLINE: EXIT-обработчик стенда с подменёнными sysfs и системными командами."""
import os
from pathlib import Path
import subprocess
import tempfile


script = Path(__file__).with_name("run-gsp-boot-detached.sh").read_text()
# Берём только определения функций, не исполняем основной путь отключения GPU.
functions = script.split("drv_of() {", 1)[1].split('\necho "=== run-gsp-boot-detached', 1)[0]
functions = "drv_of() {" + functions
bdf, aud = "0000:01:00.0", "0000:01:00.1"


def check(name, harness_rc=0, fail_bind="", fail_gdm=False, already_native=False):
    with tempfile.TemporaryDirectory(prefix="rtx-restore-test-") as tmp:
        root = Path(tmp)
        pci = root / "pci"
        for driver in ("nvidia", "snd_hda_intel", "vfio-pci"):
            (pci / "drivers" / driver).mkdir(parents=True)
            (pci / "drivers" / driver / "bind").touch()
        for device, native in ((bdf, "nvidia"), (aud, "snd_hda_intel")):
            dev = pci / "devices" / device
            dev.mkdir(parents=True)
            (dev / "driver_override").write_text("vfio-pci\n")
            (dev / "driver").symlink_to(pci / "drivers" / (native if already_native else "vfio-pci"))

        # timeout имитирует bind/unbind. В отличие от ядра, modprobe намеренно
        # не привязывает устройства: это воспроизводит уже загруженный модуль.
        mocks = r'''
modprobe() { return 0; }
sleep() { return 0; }
systemctl() { [ "$*" = "start gdm" ] && [ "$FAIL_GDM" = 0 ]; }
timeout() {
    [ "$#" = 7 ] && [ "$1" = 15 ] && [ "$2" = bash ] &&
        [ "$3" = -c ] && [ "$5" = _ ] || return 97
    local fn="$6" target="$7"
    case "$fn" in "$BDF"|"$AUD") ;; *) return 97 ;; esac
    case "$target" in
        "$TEST_PCI/devices/$fn/driver/unbind")
            rm "$TEST_PCI/devices/$fn/driver" ;;
        "$TEST_PCI/drivers/nvidia/bind"|"$TEST_PCI/drivers/snd_hda_intel/bind")
            [ "$fn" != "$FAIL_BIND" ] || return 1
            ln -s "${target%/bind}" "$TEST_PCI/devices/$fn/driver" ;;
        *) return 97 ;;
    esac
}
'''
        code = "set -u\n" + functions.replace("/sys/bus/pci", str(pci)) + mocks
        code += '\ntrap restore_gui EXIT\nexit "$HARNESS_RC"\n'
        env = dict(os.environ, BDF=bdf, AUD=aud, DIR=tmp, TEST_PCI=str(pci),
                   FAIL_BIND=fail_bind, FAIL_GDM=str(int(fail_gdm)), HARNESS_RC=str(harness_rc))
        result = subprocess.run(["bash", "-c", code], env=env, text=True, capture_output=True)
        failed = bool(fail_bind or fail_gdm)
        expected = harness_rc or (3 if failed else 0)
        assert result.returncode == expected, (name, result.returncode, result.stdout, result.stderr)
        marker = (root / "gsp-boot-DONE").read_text()
        assert marker == f"rc={expected} restore_failed={int(failed)}\n", (name, marker)
        for device, native in ((bdf, "nvidia"), (aud, "snd_hda_intel")):
            dev = pci / "devices" / device
            assert (dev / "driver_override").read_text() == "\n"
            if device == fail_bind:
                assert not (dev / "driver").exists()
            else:
                assert (dev / "driver").resolve().name == native, (name, device)
        print(f"PASS: {name}")


check("GPU и аудио возвращаются к уже загруженным драйверам")
check("повторное восстановление", already_native=True)
check("код сбоя стенда сохранён", harness_rc=42)
check("неудачный bind аудио отвергает общий успех", fail_bind=aud)
check("неудачный bind GPU отвергает общий успех", fail_bind=bdf)
check("первичная ошибка не затёрта ошибкой восстановления", harness_rc=42, fail_bind=aud)
check("сбой запуска gdm не считается успехом", fail_gdm=True)
print("GSP restore OFFLINE: PASS (без доступа к GPU)")
