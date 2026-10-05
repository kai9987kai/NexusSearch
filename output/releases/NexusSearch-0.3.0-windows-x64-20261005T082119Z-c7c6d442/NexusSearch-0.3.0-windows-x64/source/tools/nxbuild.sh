#!/usr/bin/env bash
# nxbuild.sh - compile + link a set of C files with the project's strict flags.
#
#   tools/nxbuild.sh <OUT.exe> <file.c> [more .c files / extra gcc flags ...]
#   e.g. tools/nxbuild.sh $TMP/test_bitmap.exe src/core/*.c src/index/nx_bitmap.c tests/test_bitmap.c
#
# Used by module authors so parallel work never contends on one CMake build tree.
# Baseline ISA is x86-64-v2 (SSE4.2 + POPCNT); AVX2/FMA/BMI2 code must live in
# functions marked NX_TARGET("avx2,fma") and be chosen at runtime (nx_simd.h).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# MSYS2 UCRT64 toolchain (gcc 15). Harmless on other systems.
if [ -d /c/msys64/ucrt64/bin ]; then export PATH="/c/msys64/ucrt64/bin:$PATH"; fi
OUT="$1"; shift
mkdir -p "$(dirname "$OUT")"
ARCH=""
case "$(uname -m 2>/dev/null || echo x86_64)" in
  x86_64|AMD64|amd64) ARCH="-march=x86-64-v2" ;;
esac
exec gcc -std=c11 -O2 -g $ARCH \
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror \
  -D__USE_MINGW_ANSI_STDIO=1 -DNX_MEM_DEBUG \
  -I"$ROOT/src" -I"$ROOT/tests" -I"$ROOT/include" -pthread \
  "$@" -o "$OUT" -lm
