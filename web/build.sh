#!/usr/bin/env bash
# Builds the WebAssembly version of the game into build-web/.
#
#   web/build.sh            # Release build
#   BUILD_TYPE=Debug web/build.sh
#
# Needs the Emscripten SDK (https://emscripten.org). Set EMSDK to its location or install it in ~/emsdk.
# The asset header generators need python3, same as the native build.

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SRC_DIR/build-web}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
ROMID="${ROMID:-ntsc-final}"

if ! command -v emcc >/dev/null 2>&1; then
  EMSDK="${EMSDK:-$HOME/emsdk}"
  if [ ! -f "$EMSDK/emsdk_env.sh" ]; then
    echo "error: emcc not found and no emsdk at $EMSDK; install it or set EMSDK" >&2
    exit 1
  fi
  # shellcheck disable=SC1091
  source "$EMSDK/emsdk_env.sh" >/dev/null
fi

# SDL2's headers (the build implements the part of SDL it uses itself, see port/src/web_sdl.c)
embuilder build sdl2 >/dev/null

emcmake cmake -S "$SRC_DIR" -B "$BUILD_DIR" -DROMID="$ROMID" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$(nproc 2>/dev/null || echo 4)"

echo
echo "Built $BUILD_DIR/pd.js and pd.wasm. Run: node $SRC_DIR/web/server.js"
