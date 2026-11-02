#!/usr/bin/env bash
# Builds Aseprite on the Fedora host in build-host/ (build/ belongs to the
# dev container, its CMake cache has Debian library paths).
#
#   ./build_fedora.sh              build
#   ./build_fedora.sh --run [...]  build and run (extra args go to aseprite)
#
# Needs: sudo dnf install -y gcc-c++ clang libcxx-devel cmake ninja-build unzip \
#   libX11-devel libXcursor-devel libXi-devel libXrandr-devel mesa-libGL-devel fontconfig-devel

set -e
cd "$(dirname "$0")"

skia=$PWD/.deps/skia-m124

if [ ! -f build-host/CMakeCache.txt ]; then
  # The pre-built Skia needs libstdc++
  cmake -S . -B build-host -G Ninja \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS:STRING=-stdlib=libstdc++ \
    -DCMAKE_EXE_LINKER_FLAGS:STRING=-stdlib=libstdc++ \
    -DLAF_BACKEND=skia \
    -DSKIA_DIR="$skia" \
    -DSKIA_LIBRARY_DIR="$skia/out/Release-x64" \
    -DSKIA_LIBRARY="$skia/out/Release-x64/libskia.a"
fi

ninja -C build-host aseprite

if [ "$1" = "--run" ]; then
  shift
  exec ./build-host/bin/aseprite "$@"
fi
