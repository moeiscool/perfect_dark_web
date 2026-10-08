# How the Android version works

The Android app (`android/`) is built the same way as the PS5 version. It runs **the WebAssembly build of the game** (`build-web/pd.wasm`), translated to C by wasm2c, through the shared native host (`native_host/`). Read [PS5.md](PS5.md) first. It explains:
* why the WebAssembly build is used: lockstep online play needs every player to run the identical program
* the `pdhost` interface
* how the native host and its online client work

This page covers what's specific to Android. To build and install it, see [android/README.md](../android/README.md).

## Layout

The structure follows [izzy2lost's Android port](https://github.com/izzy2lost/perfect_dark) of the PC game: a launcher activity takes care of the ROM, and an SDL activity runs the game.

```
android/
  build.sh                    toolchain download/caching, Gradle build, signing -> android/dist/*.apk
  settings.gradle, build.gradle, gradle.properties, app/build.gradle
  app/src/main/
    AndroidManifest.xml       landscape, GLES 3 required; touch/gamepad/leanback optional; INTERNET, VIBRATE
    java/ca/m03/perfectdark/
      LauncherActivity.java   ROM: system file picker -> MD5 check -> copy into the app's folder
      GameActivity.java       extends SDLActivity: libraries, arguments, fullscreen, keep screen on
    java/org/libsdl/app/      SDL's Java side, copied in by build.sh (not in git)
    res/                      crosshair icons, Android TV banner, "Change ROM" shortcut
  app/jni/
    CMakeLists.txt            SDL2 (shared) + native_host (static, Android options) + libmain.so
    android_main.c            SDL_main -> pdhostMain(), stdout/stderr -> logcat
```

### The two activities

* **`LauncherActivity`** makes sure the ROM is in `Android/data/ca.m03.perfectdark/files/data/pd.ntsc-final.z64`.
  * If it's missing (or the "Change ROM" shortcut was used), it opens `ACTION_OPEN_DOCUMENT` and copies the file in while computing its MD5.
  * It accepts NTSC v1.1 and warns about v1.0. Anything else is deleted again.
  * Then it starts `GameActivity`.
  * It uses plain `android.app` classes, so the app has no AndroidX dependency.
* **`GameActivity`**:
  * extends SDL's `SDLActivity`
  * loads `libSDL2.so` and `libmain.so`
  * passes `--data <files>/data --save <files>/save --tmp <cache>/pd --fullscreen`, plus any `pdargs` extra, which tests use (`--replay`, `--join`, `--headless`…)
  * keeps the screen on and draws edge to edge under display cutouts, with immersive full screen

### Native part (`app/jni/CMakeLists.txt`)

Gradle's `externalNativeBuild` runs CMake with the NDK. `build.sh` passes the paths of `pd.wasm`, wabt and the SDL source as Gradle properties.

* **SDL2 2.32.10** is built from its release source as `libSDL2.so`, which is what `SDLActivity` loads.
* **`native_host/`** is built as a static library with:
  * `PDHOST_GLES`: an OpenGL ES 3.0 context, and shaders used as they are. They're already GLSL ES 3.00, the same as in a browser.
  * `PDHOST_TOUCH`: the on-screen controls (below).
  * `PDHOST_BOUNDS_CHECK`: explicit memory checks, so no `SIGSEGV` handler is installed next to ART's own.
  * `PDHOST_PLATFORM_NAME=android`: this is what the lobby sees.
* **`libmain.so`**:
  * links the host, mbedTLS, SDL2, `GLESv3`, `EGL`, `android`, `log` and the NDK's `z`
  * `android_main.c` pipes stdout and stderr into logcat under the tag `PerfectDark`, so the game's log and the netplay hash lines can be read with `adb logcat -s PerfectDark`
* **ABIs:** `arm64-v8a` for phones and tablets, and `x86_64` for Chromebooks and the emulator. minSdk 24, targetSdk 35.

### Touch controls (`native_host/src/touch.c`)

* **A virtual controller.** The on-screen controls are presented to the game as **one more game controller**: pad events in SDL's GameController layout, through the same `pdhost` interface real pads use. So menus, gameplay and online matches need no touch-specific code anywhere in the game.
* **Which pad it is:** the first touch registers the virtual pad. It takes slot 0 when no physical controller has connected yet, which makes it player 1. Physical controllers connected later skip its slot.
* **Layout:**
  * **Sticks:** a floating move stick on the left half and a floating look stick on the right. Each centres where the thumb lands; full tilt is 11% of the screen height.
  * **Buttons:** A/B/X/Y in a diamond at the upper right, Fire (right trigger) and Aim (left trigger) at the lower right, and Back and Start at the top.
  * These match the game's default pad binds in `port/src/input.c`: the right trigger fires, the left trigger aims, A/B/X/Y act as on a controller, Start pauses, and Back shows the scoreboard online.
* **Drawing:** a tiny GLES program draws them as anti-aliased circles just before the buffer swap. It saves and restores the GL state it touches (program, VAO, buffer, framebuffer, viewport, blending, depth, scissor and cull).
* **Hiding:** pressing a controller button or a key hides them. SDL's synthesised mouse events from touches are turned off (`SDL_HINT_TOUCH_MOUSE_EVENTS=0`), so a touch doesn't also move the free-aim cursor.
* **Other Android details** in `platform_sdl.c`:
  * The back button or gesture (`SDL_SCANCODE_AC_BACK`) is reported as Escape: pause, or back in menus.
  * The window is landscape only.
  * The GL surface asks for 8 bits per colour channel. Mobile EGL may otherwise choose RGB565.

### Building (`android/build.sh`)

On first run it caches everything in `~/androidsdk`:
* the command-line tools; then `sdkmanager` installs platform 35, build-tools 35, NDK r27.2.12479018, CMake 3.22.1 and platform-tools, accepting the SDK licenses
* Gradle 8.10.2 (no wrapper jar in the repository)
* the SDL 2.32.10 tarball (sha256 checked)
* wabt

Each run then:
1. copies SDL's Java sources into the app
2. makes a signing key once (`perfectdark.keystore`, with a random password beside it)
3. runs `gradle assembleRelease` with the build id as `versionName` (`web-<build>`) and the build time as `versionCode`
4. checks the signature with `apksigner`

**Keep the keystore.** Android only installs an update over an existing install if it's signed with the same key.

## How it was tested

These tests ran on the x86_64 Android 15 emulator (KVM) on the build server:

* **Install and launcher:** the APK installed. The launcher showed the ROM screen; with the ROM present, it went straight into the game.
* **Same results as the browser:** a match recorded with the browser build (`web/net/headless.js --record`) was replayed in the app with `--headless --replay`. All 60 state hashes in logcat were identical to the browser build's. The NDK-compiled wasm2c code matches the browser bit for bit (x86_64).
* **The game runs:** about 34 fps with software GL, and keyboard events reach it.
* **Touch controls:** they appeared on the first touch, registered as pad 0 and were assigned to player 1, and drew correctly.
* **Not confirmed: the game's own picture.** It stayed black on the emulator's default SwiftShader GL translator.
  * The game issued its draw calls to the screen with no GL errors and complete framebuffers.
  * The same code with an OpenGL ES 3.2 context on Mesa (Linux, Xvfb) rendered correctly.
  * The emulator's other GPU modes (host Mesa, ANGLE/SwiftShader) crashed the emulator's own UI thread (`hwuiTask`: "pthread_mutex_lock called on a destroyed mutex").
  * So this is most likely an emulator limitation, but **it needs checking on a real device**: `adb logcat -s PerfectDark` shows any GL errors.

Testing tips:
* **Files pushed with `adb root`** into `Android/data/ca.m03.perfectdark/files` belong to root, and the app can't read them. `chown` them to the app's user.
* **Android's "Viewing full screen" notice** covers the screen on first start. `adb shell settings put secure immersive_mode_confirmations confirmed` skips it.
* **Extra arguments** go through the launcher: `adb shell am start -n ca.m03.perfectdark/.LauncherActivity --es pdargs "'--headless --replay /sdcard/…/ticks.bin'"`.

## Not done yet

* **arm64 determinism** is expected but unverified. The flags that keep float math exact (`-ffp-contract=off`, no fast-math) apply on arm64 too, but only x86_64 could be checked here.
* **Touch layout** is a first version: positions, sizes and sensitivity may need tuning on real phones. Labels on the buttons would help.
* **Scoreboard and notices:** the online scoreboard and join notices are drawn by the web page in the browser. Native hosts (Android, PS5) don't show them yet.
* **Build id:** the APK has to be rebuilt whenever the server's game build changes, as with the PS5 build.
