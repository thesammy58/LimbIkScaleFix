#!/usr/bin/env bash
# Builds LimbIkScaleFix.asi and its LimbIkScaleFix.pdb (debug symbols) with
# Clang and LLD, using the 32-bit MinGW-w64 headers and libraries. Clang is used
# rather than GCC because GCC can't write PDB files.
#
# The output is reproducible: the same compiler versions give byte-identical
# files wherever the repository is checked out. Source paths inside the PDB are
# recorded under C:\LimbIkScaleFix\ for that reason.
#   Debian/Ubuntu: sudo apt-get install clang lld g++-mingw-w64-i686-win32
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-clang++}"
TARGET=--target=i686-w64-mingw32
mkdir -p dist obj
"$CXX" $TARGET -std=c++17 -O2 -msse2 -mfpmath=sse -fno-math-errno -Wall -Wextra \
    -g -gcodeview -gno-codeview-command-line -fdebug-compilation-dir=. \
    -c LimbIkScaleFix.cpp -o obj/LimbIkScaleFix.o
"$CXX" $TARGET -fuse-ld=lld -shared -static -static-libgcc -static-libstdc++ \
    -Wl,--no-insert-timestamp -Wl,-s -Wl,--pdb=dist/LimbIkScaleFix.pdb \
    -Xlinker -Xlink=-pdbaltpath:%_PDB% -Xlinker '-Xlink=-pdbsourcepath:C:\LimbIkScaleFix' \
    -o dist/LimbIkScaleFix.asi obj/LimbIkScaleFix.o
cp LimbIkScaleFix.ini dist/
(cd dist && sha256sum LimbIkScaleFix.asi LimbIkScaleFix.pdb | tee SHA256SUMS.txt)
