#!/usr/bin/env bash
# Builds LimbIkScaleFix.asi with the 32-bit MinGW-w64 cross-compiler, the same
# toolchain and flags as the released and in-game-tested builds. The output is
# reproducible: the same compiler version gives a byte-identical file.
#   Debian/Ubuntu: sudo apt-get install g++-mingw-w64-i686-win32
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-i686-w64-mingw32-g++}"
mkdir -p dist
"$CXX" -std=c++17 -O2 -msse2 -mfpmath=sse -fno-math-errno -Wall -Wextra \
    -shared -static -static-libgcc -static-libstdc++ -s -Wl,--no-insert-timestamp \
    -o dist/LimbIkScaleFix.asi LimbIkScaleFix.cpp
cp LimbIkScaleFix.ini dist/
(cd dist && sha256sum LimbIkScaleFix.asi | tee LimbIkScaleFix.asi.sha256)
