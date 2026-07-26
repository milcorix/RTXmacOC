#!/usr/bin/env bash
#
# build_milcorixfb_kext.sh — собрать ЗАГРУЖАЕМЫЙ бандл MilcorixFB.kext.
#
# В отличие от CI-шага «Link full MilcorixFB kext» (тот делает `ld -r` —
# релокатабл-объект только для проверки, что все nv_* символы разрешены), здесь
# производится настоящий kext-бинарь через `ld -kext` и собирается бандл:
#
#   MilcorixFB.kext/
#     Contents/
#       Info.plist                 (driver/macos/Info.plist)
#       MacOS/MilcorixFB           (Mach-O kext, MH_KEXT_BUNDLE)
#
# Артефакт можно поставить в /Library/Extensions (kextload/kmutil) или заинжектить
# через OpenCore (EFI/OC/Kexts + Kernel→Add). Запуск требует реального железа
# (RTX 4070S) + SIP off + AMFIPass — см. docs/testbed.md.
#
# Запускать НА macOS (нужны Kernel.framework и Apple ld). Кросс-сборка с M1 ок —
# таргет всегда x86_64 (целевой стенд Intel). Использование:
#   tools/build_milcorixfb_kext.sh [OUTDIR]     (по умолчанию OUTDIR=build)
set -euo pipefail

OUTDIR="${1:-build}"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

SDK="$(xcrun --sdk macosx --show-sdk-path)"
KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
KPHDR="$SDK/System/Library/Frameworks/Kernel.framework/PrivateHeaders"
ARCH="x86_64"
mkdir -p "$OUTDIR"

# --- 1. C++-обвязка kext (IOFramebuffer subclass + firmware-шим) --------------
CXXFLAGS=(-arch "$ARCH" -isysroot "$SDK" -I"$KHDR" -I"$KPHDR"
          -fapple-kext -fno-builtin -fno-exceptions -fno-rtti -fno-common
          -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=gnu++17)
clang++ -x c++ -c driver/macos/MilcorixFB.cpp   "${CXXFLAGS[@]}" -o "$OUTDIR/MilcorixFB.o"
clang++ -x c++ -c driver/macos/fw_blob_macos.cpp "${CXXFLAGS[@]}" -o "$OUTDIR/fw_blob_macos.o"
clang++ -x c++ -c driver/macos/mfb_klog.cpp      "${CXXFLAGS[@]}" -o "$OUTDIR/mfb_klog.o"

# --- 2. Переносимое ядро GSP как kernel-C -------------------------------------
CFLAGS=(-arch "$ARCH" -isysroot "$SDK" -I"$KHDR"
        -ffreestanding -fno-builtin -fno-common
        -DKERNEL -DKERNEL_PRIVATE -DAPPLE -std=gnu11)
CORE_OBJ=()
for f in driver/gsp/*.c; do
    o="$OUTDIR/core_$(basename "$f" .c).o"
    clang -c "$f" "${CFLAGS[@]}" -o "$o"
    CORE_OBJ+=("$o")
done

# --- 3. Настоящая kext-линковка (MH_KEXT_BUNDLE) ------------------------------
# `ld -kext`: KPI-символы (IOKit/libkern) остаются неопределёнными и резолвятся
# при загрузке kextload'ом против запрошенных в Info.plist OSBundleLibraries.
# Наши nv_*/C++ символы должны быть закрыты — иначе kext не загрузится.
KEXTBIN="$OUTDIR/MilcorixFB.bin"
ld -kext -arch "$ARCH" \
   "$OUTDIR/MilcorixFB.o" "$OUTDIR/fw_blob_macos.o" "$OUTDIR/mfb_klog.o" "${CORE_OBJ[@]}" \
   -o "$KEXTBIN"

# Санитарная проверка: неопределённых nv_* быть не должно (все закрыты обвязкой/core).
if nm -u "$KEXTBIN" | grep -q "_nv_"; then
    echo "FAIL: в kext-бинаре остались неопределённые nv_* символы:"
    nm -u "$KEXTBIN" | grep "_nv_"
    exit 1
fi

# --- 4. Сборка бандла ---------------------------------------------------------
BUNDLE="$OUTDIR/MilcorixFB.kext"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/Contents/MacOS"
cp driver/macos/Info.plist "$BUNDLE/Contents/Info.plist"
cp "$KEXTBIN"              "$BUNDLE/Contents/MacOS/MilcorixFB"

# CFBundleExecutable обязателен, чтобы kextload нашёл бинарь. Добавляем, если нет.
if ! /usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" \
        "$BUNDLE/Contents/Info.plist" >/dev/null 2>&1; then
    /usr/libexec/PlistBuddy -c "Add :CFBundleExecutable string MilcorixFB" \
        "$BUNDLE/Contents/Info.plist"
fi

echo "OK: собран $BUNDLE"
echo "    тип бинаря: $(file -b "$BUNDLE/Contents/MacOS/MilcorixFB")"
echo ""
echo "Установка на целевом стенде (RTX 4070S, SIP off + AMFIPass):"
echo "  sudo tools/install_milcorix_fw.sh          # разложить прошивки"
echo "  sudo cp -R $BUNDLE /Library/Extensions/"
echo "  sudo kmutil load -p /Library/Extensions/MilcorixFB.kext   # или kextload"
echo "  log stream --predicate 'sender == \"MilcorixFB\"' --level debug"
