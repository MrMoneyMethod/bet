#!/usr/bin/env bash
# Cross-compile a self-contained x64 Windows executable using Zig's bundled MinGW.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ -z "${ZIG:-}" ]]; then
  if command -v zig >/dev/null 2>&1; then ZIG="$(command -v zig)";
  elif [[ -x .cache/ziglang/ziglang/zig ]]; then ZIG="$PWD/.cache/ziglang/ziglang/zig";
  else echo 'Install Zig 0.13.0 or set ZIG to its executable.' >&2; exit 1; fi
fi
mkdir -p build/windows/generated releases .cache/zig-global .cache/zig-local
python3 scripts/embed_assets.py --output build/windows/generated/embedded_assets.hpp
export ZIG_GLOBAL_CACHE_DIR="$PWD/.cache/zig-global"
export ZIG_LOCAL_CACHE_DIR="$PWD/.cache/zig-local"
"$ZIG" c++ -target x86_64-windows-gnu -std=c++17 -O2 -g0 -static -fno-sanitize=all \
  -DNOMINMAX -DWIN32_LEAN_AND_MEAN \
  -Ivendor -Isrc -Ibuild/windows/generated \
  src/core.cpp src/feeds.cpp src/app.cpp src/main.cpp \
  -lwinhttp -lws2_32 -lshell32 -o build/windows/Edgecourt.exe
cp build/windows/Edgecourt.exe releases/Edgecourt.exe
printf '\nBuilt releases/Edgecourt.exe\n'
