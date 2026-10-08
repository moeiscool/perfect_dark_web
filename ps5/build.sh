#!/usr/bin/env bash
# Builds the PS5 version: a native folder title that runs build-web/pd.wasm through the native
# host (native_host/), so PS5 players share online matches with browser players.
#
#   web/build.sh          # the WebAssembly build first
#   ps5/build.sh          # -> ps5/dist/<TITLE_ID>/ ; then copy your ROM into its assets/ folder
#
# Linux (Ubuntu 24.04+ / WSL2). Needs: clang-18 lld-18 llvm-18 lld cmake ninja-build python3 git wget
# (see ps5/README.md). Downloads and caches its dependencies in $PS5SDK_ROOT (default ~/ps5sdk):
#   ps5-native-app-boilerplate (folder-title packaging, PS5 payload SDK, zlib, runtime shim)
#   ps5-opengl SDK 0.3.0 (OpenGL over AGC) and the PS5 SDL2 port built against it
#   wabt (wasm2c)
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="${PS5SDK_ROOT:-$HOME/ps5sdk}"
TITLE_ID="${TITLE_ID:-PPSA06400}"
APP_NAME="${APP_NAME:-Perfect Dark}"
PD_WASM="${PD_WASM:-$REPO/build-web/pd.wasm}"
HEAP_MB="${PS5_HEAP_MB:-1024}"       # app heap: pd.wasm's memory (256 MiB+) plus the GL driver's
WORK="$REPO/build-ps5"
OUT="$REPO/ps5/dist"

TEMPLATE_URL=https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git
TEMPLATE_REV=c93771e0f9bf4f0993c3c317d7df4cc01888fb72
GLSDK_URL=https://github.com/blackbearreloaded/ps5-opengl/releases/download/v0.3.0/ps5-opengl-sdk-0.3.0.tar.gz
GLSDK_SHA256=a7bd6b85f00398eaf8d87ecc58fa0bde7cab79e065acc783268070d2f14b403c
SDL_URL=https://github.com/ps5-payload-dev/SDL.git
SDL_REV=8c56053f13ca13a0c050de613706ff69eb615836
WABT_URL=https://github.com/WebAssembly/wabt.git

TEMPLATE="$ROOT/native-app-boilerplate"
GLSDK="$ROOT/extracted/ps5-opengl-sdk-0.3.0"
GLSRC="$ROOT/ps5-opengl-030/ps5-opengl"
SDL="$ROOT/SDL"
SDLBUILD="$GLSRC/build/sdl2-pd"
WABT="${WABT_DIR:-$ROOT/wabt}"

step() { printf '\n==> %s\n' "$*"; }

[[ -f $PD_WASM && -f $(dirname "$PD_WASM")/pd.snap.json ]] || { echo "build the game first: web/build.sh" >&2; exit 1; }
mkdir -p "$ROOT/extracted"

# ---------------------------------------------------------------------------------------------
step "dependencies in $ROOT"

if [[ ! -d $TEMPLATE/.git ]]; then
  git clone -q "$TEMPLATE_URL" "$TEMPLATE"
fi
git -C "$TEMPLATE" -c advice.detachedHead=false checkout -q "$TEMPLATE_REV"
chmod +x "$TEMPLATE/tooling/prospero-clang18"
if [[ ! -f $TEMPLATE/runtime/libc.prx || ! -x $TEMPLATE/.deps/native/ps5-payload-sdk/bin/prospero-lld ]]; then
  (cd "$TEMPLATE" && bash tools/setup-native-dependencies.sh && bash tools/rebuild-libc.sh)
fi
PAYLOAD_SDK="$TEMPLATE/.deps/native/ps5-payload-sdk"
export PS5_PAYLOAD_SDK="$PAYLOAD_SDK"   # the compiler wrapper needs it
ZLIB_ROOT="$TEMPLATE/.deps/native/zlib/root/usr"

if [[ ! -d $GLSDK/sdk ]]; then
  tarball="$ROOT/ps5-opengl-sdk-0.3.0.tar.gz"
  [[ -f $tarball ]] || wget -q -O "$tarball" "$GLSDK_URL"
  echo "$GLSDK_SHA256  $tarball" | sha256sum --check --strict
  tar xzf "$tarball" -C "$ROOT/extracted"
fi
if [[ ! -d $GLSRC ]]; then
  mkdir -p "$ROOT/ps5-opengl-030"
  tar xf "$GLSDK/sources/ps5-opengl.tar" -C "$ROOT/ps5-opengl-030"
fi

if [[ ! -f $SDLBUILD/sdk/lib/libSDL2.a ]]; then
  [[ -d $SDL/.git ]] || git clone -q "$SDL_URL" "$SDL"
  git -C "$SDL" -c advice.detachedHead=false checkout -q "$SDL_REV"
  # the SDK's SDL build leaves audio out; turn on the PS5 (SceAudioOut) backend
  sed -i 's/^foreach(feature AUDIO RENDER HAPTIC/foreach(feature RENDER HAPTIC/;
          s/ScePad;SceUserService;SceSystemService")/ScePad;SceUserService;SceSystemService;SceAudioOut")/' \
    "$GLSRC/integration/SDL2/CMakeLists.txt"
  rm -rf "$SDLBUILD"
  (cd "$GLSRC" && python3 integration/SDL2/build.py native --sdl-source "$SDL" --sdk-prefix "$GLSDK/sdk" \
    --out build/sdl2-pd --payload-sdk "$PAYLOAD_SDK" --compiler-wrapper "$TEMPLATE/tooling/prospero-clang18")
fi

if [[ ! -x $WABT/build/wasm2c ]]; then
  [[ -d $WABT/.git ]] || git clone -q --recursive --depth 1 "$WABT_URL" "$WABT"
  cmake -S "$WABT" -B "$WABT/build" -DBUILD_TESTS=OFF -DBUILD_LIBWASM=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$WABT/build" --target wasm2c -j"$(nproc)"
fi

# ---------------------------------------------------------------------------------------------
step "native host for PS5 (pd.wasm -> C, cross-compiled)"

PS5_TEMPLATE="$TEMPLATE" cmake -S "$REPO/native_host" -B "$WORK/host" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$REPO/ps5/toolchain.cmake" -DPS5_TEMPLATE="$TEMPLATE" \
  -DCMAKE_BUILD_TYPE=Release \
  -DPDHOST_LIBRARY=ON -DPDHOST_BOUNDS_CHECK=ON -DPDHOST_PLATFORM_NAME=ps5 \
  -DPDHOST_SDL2_INCLUDE_DIR="$SDLBUILD/sdk/include/SDL2" \
  -DWABT_DIR="$WABT" -DPD_WASM="$PD_WASM" \
  -DZLIB_INCLUDE_DIR="$ZLIB_ROOT/include" -DZLIB_LIBRARY="$ZLIB_ROOT/lib/libz.a" \
  -DCMAKE_FIND_ROOT_PATH="$ZLIB_ROOT"
cmake --build "$WORK/host" -j"$(nproc)"

# ---------------------------------------------------------------------------------------------
step "folder title $TITLE_ID"

# The same assembly as the SDK's integration/SDL2/folder.py: the boilerplate's packaging with the
# SDK's runtime shims, an app-owned heap and the GL driver linked in.
APP="$WORK/app"
rm -rf "$APP"
mkdir -p "$APP"
for d in runtime sce_sys tooling tools .deps/native; do
  mkdir -p "$APP/$(dirname "$d")"
  cp -a "$TEMPLATE/$d" "$APP/$d"
done
mkdir -p "$APP/src" "$APP/vendor" "$APP/build/native-imports"
cp "$REPO/ps5/src/ps5_main.c" "$APP/src/"
cp "$GLSRC/native-app/runtime_shims.c" "$APP/src/"
sed "s/#define PS5_OPENGL_HEAP_SIZE (128u \* 1024u \* 1024u)/#define PS5_OPENGL_HEAP_SIZE (${HEAP_MB}u * 1024u * 1024u)/" \
  "$GLSRC/native-app/app_heap.c" > "$APP/src/app_heap.c"
grep -q "(${HEAP_MB}u \* 1024u" "$APP/src/app_heap.c" || { echo "app_heap.c changed upstream" >&2; exit 1; }
cp "$APP/tooling/native/ps5-pie.ld" "$APP/tooling/native/ps5-pie-base.ld"
cp "$GLSRC/native-app/ps5-pie.ld" "$GLSRC/native-app/app-symbols.map" "$APP/tooling/native/"

replace_once() {
  python3 - "$1" "$2" "$3" <<'PY'
import sys, pathlib
p, old, new = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
t = p.read_text()
if t.count(old) != 1:
    sys.exit(f"boilerplate changed: {p}: {old}")
p.write_text(t.replace(old, new))
PY
}
replace_once "$APP/tooling/native/sce_module_writer.cpp" \
  "write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());" \
  "write_u64(result.data, result.heap_size, 0x10000000ULL);"
replace_once "$APP/tools/build.sh" 'bash "$root/tools/setup-native-dependencies.sh" >/dev/null' \
  'test -x "$root/.deps/native/ps5-payload-sdk/bin/prospero-lld"'
replace_once "$APP/tools/build.sh" '[[ -f $root/runtime/libc.prx ]] || bash "$root/tools/rebuild-libc.sh"' \
  'test -f "$root/runtime/libc.prx"'
replace_once "$APP/tools/build.sh" "--eh-frame-hdr \\" \
  "--eh-frame-hdr --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size \\"

APPSDK="$APP/.deps/native/ps5-payload-sdk"
cp "$GLSDK/sdk/lib/libSceAgc.so" "$GLSDK/sdk/lib/libSceAgcDriver.so" "$APPSDK/target/lib/"

# title identity, icon, and an assets folder for the ROM
(cd "$APP" && TITLE_ID="$TITLE_ID" APP_NAME="$APP_NAME" APP_CATEGORY=game bash tools/init-project.sh sce_sys/param.json)
# the GL driver needs the SDK app's memory setup (GPU/CPU page tables, address space), which the
# boilerplate's param.json leaves out
python3 - "$APP/sce_sys/param.json" "$GLSRC/native-app/param.json" <<'PY'
import json, sys
dst, src = sys.argv[1], sys.argv[2]
param, sdk = json.load(open(dst)), json.load(open(src))
for key in ("amm", "kernel"):
    if key not in sdk:
        sys.exit(f"SDK param.json has no {key}")
    param[key] = sdk[key]
json.dump(param, open(dst, "w"), indent=2)
open(dst, "a").write("\n")
PY
mkdir -p "$APP/assets"
cp "$REPO/ps5/assets/README.txt" "$APP/assets/"

compiler_rt="$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a"
libs=("$WORK/host/libpdhost.a")
while IFS= read -r lib; do libs+=("$lib"); done < <(find "$WORK/host/_deps/mbedtls-build" -name '*.a' | sort)
libs+=("$ZLIB_ROOT/lib/libz.a" "$SDLBUILD/sdk/lib/libSDL2.a" "$GLSDK/sdk/lib/libPS5OpenGLCore33.a")
libs+=("$APPSDK/target/lib/libunwind.a" "$APPSDK/target/lib/libc++abi.a" "$APPSDK/target/lib/libc++.a" "$compiler_rt")
for lib in "${libs[@]}"; do [[ -f $lib ]] || { echo "missing $lib" >&2; exit 1; }; done
{
  printf 'SEARCH_DIR("%s/target/lib")\nSEARCH_DIR("%s/sdk/lib")\nEXTERN(ps5_agc_gate2_run)\nGROUP (\n' "$APPSDK" "$GLSDK"
  printf '  "%s"\n' "${libs[@]}"
  printf '  -lScePad\n  -lSceUserService\n  -lSceSystemService\n  -lSceAudioOut\n)\n'
} > "$APP/vendor/libpd.a"

(cd "$APP" && PS5_PAYLOAD_SDK="$APPSDK" APP_SOURCE_DIR=src APP_STATIC_ARCHIVES=vendor/libpd.a APP_ASSETS=assets \
  APP_DEFINITIONS= APP_INCLUDE_PATHS= PACBREW_PACKAGES= PACBREW_INCLUDE_PATHS= PACBREW_STATIC_ARCHIVES= \
  APP_RUNTIME_MODULES= bash tools/build.sh Folder)

rm -rf "$OUT/$TITLE_ID"
mkdir -p "$OUT"
cp -a "$APP/dist/$TITLE_ID" "$OUT/"
step "done: $OUT/$TITLE_ID"
echo "Copy your ROM to $OUT/$TITLE_ID/assets/pd.ntsc-final.z64, then install the folder (ps5/README.md)."
