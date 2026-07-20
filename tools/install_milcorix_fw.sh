#!/usr/bin/env bash
#
# install_milcorix_fw.sh — разложить прошивки GSP туда, где их читает kext.
#
# MilcorixFB (fw_blob_macos.cpp) читает РАСПАКОВАННЫЕ блобы из:
#   /Library/Application Support/Milcorix/fw/<name>-535.113.01.bin
# для name ∈ {booter_load, bootloader, gsp}. В ядре macOS нет zstd/popen, поэтому
# распаковка делается здесь (userspace), а не в kext.
#
# Правило 7: блобы в git НЕ коммитим. Источник — linux-firmware NVIDIA
# (ad104/gsp, версия 535.113.01), легально распространяемый. Каталог-источник:
#   * Linux-дев-машина: /usr/lib/firmware/nvidia/ad104/gsp (.bin.zst) — распакуем zstd;
#   * либо готовый каталог с уже распакованными <name>-535.113.01.bin (флаг $1).
#
# На целевом macOS-стенде: заранее подготовить .bin на Linux (этот же скрипт с
# --stage OUTDIR), перенести каталог на мак, затем `sudo install_milcorix_fw.sh OUTDIR`.
#
# Использование:
#   tools/install_milcorix_fw.sh --stage <outdir>   # Linux: распаковать в outdir
#   sudo tools/install_milcorix_fw.sh [srcdir]      # разложить в системный путь
set -euo pipefail

FW_VERSION="535.113.01"
NAMES=(booter_load bootloader gsp)
DEST="/Library/Application Support/Milcorix/fw"
LINUX_SRC="${RTX_FW_DIR:-/usr/lib/firmware/nvidia/ad104/gsp}"

# Распаковать <name>-<ver>.bin.zst из src в dst/<name>-<ver>.bin (нужен zstd).
unpack_one() {
    local name="$1" src="$2" dst="$3"
    local z="$src/$name-$FW_VERSION.bin.zst"
    local b="$src/$name-$FW_VERSION.bin"
    if [ -f "$b" ]; then                     # уже распакован — просто копия
        cp "$b" "$dst/$name-$FW_VERSION.bin"
    elif [ -f "$z" ]; then
        command -v zstd >/dev/null || { echo "нужен zstd для $z"; exit 1; }
        zstd -dcf "$z" > "$dst/$name-$FW_VERSION.bin"
    else
        echo "FAIL: не найден $name (ни $b, ни $z)"; exit 1
    fi
    echo "  $name-$FW_VERSION.bin ($(wc -c < "$dst/$name-$FW_VERSION.bin") байт)"
}

# --- режим --stage: подготовить распакованные .bin (обычно на Linux) ---
if [ "${1:-}" = "--stage" ]; then
    OUT="${2:?укажите каталог: --stage <outdir>}"
    mkdir -p "$OUT"
    echo "Распаковка из $LINUX_SRC -> $OUT (версия $FW_VERSION):"
    for n in "${NAMES[@]}"; do unpack_one "$n" "$LINUX_SRC" "$OUT"; done
    echo "OK: перенесите $OUT на macOS-стенд и запустите: sudo $0 $OUT"
    exit 0
fi

# --- режим установки в системный путь (macOS-стенд, нужен root) ---
SRC="${1:-$LINUX_SRC}"
if [ "$(id -u)" != "0" ]; then
    echo "нужен root: sudo $0 ${1:-}"; exit 1
fi
mkdir -p "$DEST"
echo "Установка прошивок в: $DEST (источник $SRC)"
for n in "${NAMES[@]}"; do unpack_one "$n" "$SRC" "$DEST"; done
chown -R root:wheel "$DEST"
chmod 644 "$DEST"/*.bin
echo "OK: прошивки разложены. kext (fw_blob_macos) прочитает их при enableController."
