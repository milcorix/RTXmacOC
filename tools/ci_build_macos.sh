#!/usr/bin/env bash
#
# ci_build_macos.sh — сборка артефактов RTXmacOC на macOS-раннере GitHub Actions.
#
# Проверяет, что компилируется Apple-тулчейном:
#   1. pcie_probe        — user-space утилита (IOKit/CoreFoundation)
#   2. RTXProbe (kext)    — компиляция против Kernel.framework
#   3. RTXProbeDext (dext)— iig + компиляция против DriverKit SDK
#
# Это проверка СБОРКИ, не запуска: загрузка драйвера и чтение RTX требуют
# реального железа (см. ARCHITECTURE.md). CI доказывает, что код валиден.
set -euo pipefail

mkdir -p build

echo "::group::Toolchain"
sw_vers || true
xcodebuild -version || true
echo "macOS SDK:    $(xcrun --sdk macosx --show-sdk-path)"
echo "DriverKit SDK: $(xcrun --sdk driverkit --show-sdk-path 2>/dev/null || echo MISSING)"
echo "iig:          $(xcrun --find iig 2>/dev/null || echo MISSING)"
echo "::endgroup::"

echo "::group::1. user-space pcie_probe"
clang pcie_probe.c -framework IOKit -framework CoreFoundation -o build/pcie_probe
echo "OK: pcie_probe собран"
echo "::endgroup::"

echo "::group::2. kext RTXProbe (compile-check)"
MAC_SDK="$(xcrun --sdk macosx --show-sdk-path)"
clang++ -c driver/RTXProbe/RTXProbe.cpp \
  -arch x86_64 \
  -isysroot "$MAC_SDK" \
  -I"$MAC_SDK/System/Library/Frameworks/Kernel.framework/Headers" \
  -fapple-kext -mkernel -fno-builtin -fno-rtti -fno-exceptions -fno-common \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -std=gnu++17 \
  -o build/RTXProbe.o
echo "OK: RTXProbe.cpp компилируется против Kernel.framework"
echo "::endgroup::"

echo "::group::3. dext RTXProbeDext (iig + compile-check)"
DK_SDK="$(xcrun --sdk driverkit --show-sdk-path)"
IIG="$(xcrun --find iig)"
# iig генерирует заголовок и связующий .cpp из .iig
"$IIG" \
  --def driver/RTXProbeDext/RTXProbeDext.iig \
  --header build/RTXProbeDext.h \
  --impl build/RTXProbeDext.iig.cpp \
  -- \
  -isysroot "$DK_SDK" -x c++ -std=gnu++17 -I driver/RTXProbeDext
# компилируем нашу реализацию + сгенерированный glue
clang++ -c \
  -isysroot "$DK_SDK" \
  -arch x86_64 \
  -std=gnu++17 -fno-exceptions -fno-rtti \
  -I build -I driver/RTXProbeDext \
  driver/RTXProbeDext/RTXProbeDext.cpp -o build/RTXProbeDext.o
clang++ -c \
  -isysroot "$DK_SDK" \
  -arch x86_64 \
  -std=gnu++17 -fno-exceptions -fno-rtti \
  -I build -I driver/RTXProbeDext \
  build/RTXProbeDext.iig.cpp -o build/RTXProbeDext.iig.o
echo "OK: RTXProbeDext компилируется против DriverKit SDK"
echo "::endgroup::"

echo "::group::4. kext MilcorixFB (IOFramebuffer, compile-check)"
# MilcorixFB — subclass IOFramebuffer (слой 5, вывод картинки). Компилируется
# против Kernel.framework, как RTXProbe. IOGraphics-заголовки (IOFramebuffer.h)
# в публичном SDK могут отсутствовать — если шаг падает на "file not found",
# заголовки нужно завендорить (driver/macos/vendored/IOKit/graphics/).
MFB_GRAPHICS_INC=""
if [ -d "driver/macos/vendored" ]; then
  MFB_GRAPHICS_INC="-Idriver/macos/vendored"
fi
clang++ -c driver/macos/MilcorixFB.cpp \
  -arch x86_64 \
  -isysroot "$MAC_SDK" \
  -I"$MAC_SDK/System/Library/Frameworks/Kernel.framework/Headers" \
  $MFB_GRAPHICS_INC \
  -fapple-kext -mkernel -fno-builtin -fno-rtti -fno-exceptions -fno-common \
  -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
  -std=gnu++17 \
  -o build/MilcorixFB.o
echo "OK: MilcorixFB.cpp компилируется (IOFramebuffer subclass)"
echo "::endgroup::"

echo "ВСЕ СБОРКИ ПРОШЛИ"
