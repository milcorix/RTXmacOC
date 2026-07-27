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
# -Os обязателен: без оптимизации кадр nv_gsp_bringup достигает 7.5 КиБ при
# 16-КиБ стеке ядра, а поверх него идёт чтение 36-МиБ блоба через VFS.
CXXFLAGS=(-arch "$ARCH" -isysroot "$SDK" -I"$KHDR" -I"$KPHDR" -Os
          -fapple-kext -fno-builtin -fno-exceptions -fno-rtti -fno-common
          -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=gnu++17)
clang++ -x c++ -c driver/macos/MilcorixFB.cpp   "${CXXFLAGS[@]}" -o "$OUTDIR/MilcorixFB.o"
clang++ -x c++ -c driver/macos/fw_blob_macos.cpp "${CXXFLAGS[@]}" -o "$OUTDIR/fw_blob_macos.o"
clang++ -x c++ -c driver/macos/mfb_klog.cpp      "${CXXFLAGS[@]}" -o "$OUTDIR/mfb_klog.o"
clang++ -x c++ -c driver/macos/MilcorixUserClient.cpp "${CXXFLAGS[@]}" -o "$OUTDIR/MilcorixUserClient.o"

# --- 2. Переносимое ядро GSP как kernel-C -------------------------------------
CFLAGS=(-arch "$ARCH" -isysroot "$SDK" -I"$KHDR" -Os
        -ffreestanding -fno-builtin -fno-common
        -DKERNEL -DKERNEL_PRIVATE -DAPPLE -std=gnu11)
CORE_OBJ=()
for f in driver/gsp/*.c; do
    o="$OUTDIR/core_$(basename "$f" .c).o"
    clang -c "$f" "${CFLAGS[@]}" -o "$o"
    CORE_OBJ+=("$o")
done

# --- 2b. kmod_info: обязательная запись загрузчика kext'ов --------------------
# Xcode генерирует её сам; при ручной сборке без неё kextload/kmutil падает
# («kext has no kmod_info», rc=31). Структура связывает bundle-id с точками
# входа, а libkmodc++ добавляет вызов конструкторов/деструкторов C++.
cat > "$OUTDIR/kmod_info.c" <<'KMOD'
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(dev.milcorix.MilcorixFB, "1.0.0", _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t  *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
KMOD
clang -c "$OUTDIR/kmod_info.c" -arch "$ARCH" -isysroot "$SDK" -I"$KHDR" \
      -fno-builtin -fno-common -DKERNEL -DKERNEL_PRIVATE -DAPPLE \
      -o "$OUTDIR/kmod_info.o"

# --- 3. Настоящая kext-линковка (MH_KEXT_BUNDLE) ------------------------------
# `ld -kext`: KPI-символы (IOKit/libkern) остаются неопределёнными и резолвятся
# при загрузке kextload'ом против запрошенных в Info.plist OSBundleLibraries.
# Наши nv_*/C++ символы должны быть закрыты — иначе kext не загрузится.
# -lkmodc++ / -lkmod дают _start/_stop, поднимающие статические C++-объекты
# (в них живёт OSDefineMetaClassAndStructors — без их запуска класс не
# зарегистрируется). Библиотеки лежат ВНУТРИ SDK, а ld сам туда не смотрит,
# поэтому путь указываем явно.
KMODLIBS=()
if [ -f "$SDK/usr/lib/libkmodc++.a" ] && [ -f "$SDK/usr/lib/libkmod.a" ]; then
    KMODLIBS=(-L"$SDK/usr/lib" -lkmodc++ -lkmod)
    echo "kmod-библиотеки: $SDK/usr/lib"
elif [ -f /usr/lib/libkmodc++.a ] && [ -f /usr/lib/libkmod.a ]; then
    KMODLIBS=(-lkmodc++ -lkmod)
    echo "kmod-библиотеки: /usr/lib"
else
    echo "FAIL: не найдены libkmod/libkmodc++ (искал в $SDK/usr/lib и /usr/lib)."
    echo "      Без них у kext нет точек входа _start/_stop и статические"
    echo "      C++-конструкторы не выполнятся. Что есть рядом:"
    ls -1 "$SDK/usr/lib" 2>/dev/null | grep -i kmod || echo "      (ничего)"
    exit 1
fi

KEXTBIN="$OUTDIR/MilcorixFB.bin"
ld -kext -arch "$ARCH" \
   "$OUTDIR/MilcorixFB.o" "$OUTDIR/fw_blob_macos.o" "$OUTDIR/mfb_klog.o" \
   "$OUTDIR/MilcorixUserClient.o" "$OUTDIR/kmod_info.o" "${CORE_OBJ[@]}" \
   "${KMODLIBS[@]}" \
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

# Проверка наличия kmod_info — без неё kextload откажется грузить бандл.
if ! nm "$BUNDLE/Contents/MacOS/MilcorixFB" | grep -q "_kmod_info"; then
    echo "FAIL: в бинаре нет _kmod_info — kextload такой kext не примет"
    exit 1
fi

echo "OK: собран $BUNDLE"
echo "    тип бинаря: $(file -b "$BUNDLE/Contents/MacOS/MilcorixFB")"
echo "    kmod_info:  есть"
echo ""
echo "Установка на целевом стенде (RTX 4070S, SIP off + AMFIPass):"
echo "  sudo tools/macos_install.sh          # прошивки + kext одной командой"
echo ""
echo "Драйвер по умолчанию ВЫКЛЮЧЕН. Включение — boot-arg milcorix=1 (проверка"
echo "bring-up'а) или milcorix=2 (полный вывод); из Linux: tools/milcorix_stage.py"
