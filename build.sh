#!/usr/bin/env bash
# Build SyncTool on Linux / macOS.
#   ./build.sh              dynamic libstdc++ (Linux), plain binary (macOS)
#   STATIC=1 ./build.sh     fully static binary (Linux; recommended for releases)
set -euo pipefail
cd "$(dirname "$0")"

CXX="${CXX:-c++}"
CC="${CC:-cc}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)}"
mkdir -p build output

INC="-I third_party/miniz"
CSRC="third_party/miniz/miniz.c third_party/miniz/miniz_zip.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c"

$CC -O2 $INC -c third_party/miniz/miniz.c -o build/miniz.o
$CC -O2 $INC -c third_party/miniz/miniz_zip.c -o build/miniz_zip.o
$CC -O2 $INC -c third_party/miniz/miniz_tdef.c -o build/miniz_tdef.o
$CC -O2 $INC -c third_party/miniz/miniz_tinfl.c -o build/miniz_tinfl.o

LDFLAGS=(-pthread)
if [[ "$(uname -s)" == "Linux" ]]; then
  if [[ "${STATIC:-0}" == "1" ]]; then
    LDFLAGS+=(-static)
  else
    LDFLAGS+=(-static-libstdc++ -static-libgcc)
  fi
fi

$CXX -std=c++17 -O2 -Wall -Wextra "${LDFLAGS[@]}" $INC \
  src/main.cpp build/miniz.o build/miniz_zip.o build/miniz_tdef.o build/miniz_tinfl.o \
  -o output/SyncTool

echo "Build OK: $(pwd)/output/SyncTool"
