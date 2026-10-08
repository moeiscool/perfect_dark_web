#!/usr/bin/env bash
# Builds the Android APK: the native host (native_host/) running build-web/pd.wasm, so Android
# players share online matches with browser and PS5 players.
#
#   web/build.sh            # the WebAssembly build first
#   android/build.sh        # -> android/dist/perfectdark-<build>.apk
#
# Linux. Needs: openjdk-17-jdk-headless, python3, git, wget, unzip, cmake (for wabt). Downloads
# and caches the rest in $ANDROID_CACHE (default ~/androidsdk): Android command-line tools, SDK
# platform 35, build-tools, NDK r27, CMake, Gradle, the SDL2 source and wabt (wasm2c). Installing
# the SDK packages accepts the Android SDK licenses.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT="${ANDROID_CACHE:-$HOME/androidsdk}"
PD_WASM="${PD_WASM:-$REPO/build-web/pd.wasm}"
OUT="$REPO/android/dist"

CLT_URL=https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
GRADLE_VERSION=8.10.2
SDL_VERSION=2.32.10
SDL_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
NDK_VERSION=27.2.12479018
WABT="${WABT_DIR:-$ROOT/wabt}"

step() { printf '\n==> %s\n' "$*"; }

[[ -f $PD_WASM && -f $(dirname "$PD_WASM")/pd.snap.json ]] || { echo "build the game first: web/build.sh" >&2; exit 1; }
command -v java >/dev/null || { echo "install a JDK 17 (openjdk-17-jdk-headless)" >&2; exit 1; }
mkdir -p "$ROOT"
export ANDROID_HOME="$ROOT" ANDROID_SDK_ROOT="$ROOT"

# ---------------------------------------------------------------------------------------------
step "dependencies in $ROOT"

if [[ ! -x $ROOT/cmdline-tools/latest/bin/sdkmanager ]]; then
  wget -q -O "$ROOT/clt.zip" "$CLT_URL"
  rm -rf "$ROOT/cmdline-tools"
  unzip -q "$ROOT/clt.zip" -d "$ROOT/clt-tmp"
  mkdir -p "$ROOT/cmdline-tools"
  mv "$ROOT/clt-tmp/cmdline-tools" "$ROOT/cmdline-tools/latest"
  rm -rf "$ROOT/clt-tmp"
fi
SDKMANAGER="$ROOT/cmdline-tools/latest/bin/sdkmanager"
if [[ ! -d $ROOT/ndk/$NDK_VERSION || ! -d $ROOT/platforms/android-35 ]]; then
  yes | "$SDKMANAGER" --licenses >/dev/null || true
  "$SDKMANAGER" "platforms;android-35" "build-tools;35.0.0" "ndk;$NDK_VERSION" "cmake;3.22.1" "platform-tools"
fi

GRADLE="$ROOT/gradle-$GRADLE_VERSION/bin/gradle"
if [[ ! -x $GRADLE ]]; then
  wget -q -O "$ROOT/gradle.zip" "https://services.gradle.org/distributions/gradle-$GRADLE_VERSION-bin.zip"
  unzip -q -o "$ROOT/gradle.zip" -d "$ROOT"
fi

SDL="$ROOT/SDL2-$SDL_VERSION"
if [[ ! -f $SDL/include/SDL.h ]]; then
  wget -q -O "$ROOT/SDL2-$SDL_VERSION.tar.gz" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VERSION/SDL2-$SDL_VERSION.tar.gz"
  echo "$SDL_SHA256  $ROOT/SDL2-$SDL_VERSION.tar.gz" | sha256sum --check --strict
  tar xzf "$ROOT/SDL2-$SDL_VERSION.tar.gz" -C "$ROOT"
fi

if [[ ! -x $WABT/build/wasm2c ]]; then
  [[ -d $WABT/.git ]] || git clone -q --recursive --depth 1 https://github.com/WebAssembly/wabt.git "$WABT"
  cmake -S "$WABT" -B "$WABT/build" -DBUILD_TESTS=OFF -DBUILD_LIBWASM=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$WABT/build" --target wasm2c -j"$(nproc)"
fi

# SDL's Java side (SDLActivity and friends), from the same SDL release as the native library
JAVA_SDL="$REPO/android/app/src/main/java/org/libsdl/app"
rm -rf "$JAVA_SDL"
mkdir -p "$JAVA_SDL"
cp "$SDL"/android-project/app/src/main/java/org/libsdl/app/*.java "$JAVA_SDL/"

# a signing key of this machine's own, made once (kept outside the repository)
KEYSTORE="$ROOT/perfectdark.keystore"
PASSFILE="$ROOT/perfectdark.keystore.pass"
if [[ ! -f $KEYSTORE ]]; then
  head -c 24 /dev/urandom | base64 | tr -d '/+=' > "$PASSFILE"
  chmod 600 "$PASSFILE"
  keytool -genkeypair -keystore "$KEYSTORE" -alias perfectdark -keyalg RSA -keysize 2048 -validity 10000 \
    -storepass "$(cat "$PASSFILE")" -keypass "$(cat "$PASSFILE")" -dname "CN=Perfect Dark Web" >/dev/null
fi

# ---------------------------------------------------------------------------------------------
BUILD_ID="$(sha1sum "$PD_WASM" | cut -c1-12)"
step "APK for game build $BUILD_ID"

"$GRADLE" -p "$REPO/android" --no-daemon -q assembleRelease \
  -PpdWasm="$PD_WASM" -PwabtDir="$WABT" -PsdlSource="$SDL" \
  -Pkeystore="$KEYSTORE" -PkeystorePassword="$(cat "$PASSFILE")" \
  -PversionName="web-$BUILD_ID" -PversionCode="$(date -u +%y%j%H)"

mkdir -p "$OUT"
APK="$OUT/perfectdark-$BUILD_ID.apk"
cp "$REPO/android/app/build/outputs/apk/release/app-release.apk" "$APK"
"$ROOT/build-tools/35.0.0/apksigner" verify "$APK"
step "done: $APK"
