# Perfect Dark for Android

An Android app that runs **the same `pd.wasm` as the browser version**. [native_host/](../native_host/README.md) translates it to C with wasm2c and builds it with the NDK, together with SDL2. So Android players:

* **Play online with browser and PS5 players.** Open **Online Matches** in the Perfect Menu. It uses the same servers.
* **Play split screen offline** with several controllers, like on the N64.
* **Can use touch controls, controllers, or keyboard and mouse.**

The project follows the layout of [izzy2lost's Android port](https://github.com/izzy2lost/perfect_dark): a launcher activity picks and checks the ROM, then an SDL activity runs the game.

> **Status:**
> * **Tested** in the Android emulator (x86_64): the app installs, the ROM pick and check work, the game runs, the touch controls work, and a recorded match gives exactly the same state hashes as the browser build.
> * **Not yet confirmed:** the game's picture. The emulator's software GPU shows the game black, while the same OpenGL ES code renders correctly on Mesa's OpenGL ES. Check the picture, sound and touch feel on a real device first, and send `adb logcat -s PerfectDark` if something's wrong.

## Installing

1. Copy `perfectdark-<build>.apk` to the device and open it. You need to allow installing from that source, which is "Install unknown apps" in Settings. Or run `adb install perfectdark-<build>.apk`.
2. Start **Perfect Dark** and choose your ROM: NTSC v1.1 `.z64` (`pd.ntsc-final.z64`, md5 `e03b088b6ac9e0080440efed07c1e40f`).
   * It's copied into the app's folder (`Android/data/ca.m03.perfectdark/files/data`); nothing is uploaded.
   * v1.0 works too, with a warning.
3. To pick another ROM later: long-press the app icon, then **Change ROM**.

Saves and settings are kept in `Android/data/ca.m03.perfectdark/files/save`.

## Controls

* **Touch.** The controls appear when you touch the screen, and act as player 1's controller:
  * **Left half:** move stick. Put your thumb down anywhere and push.
  * **Right half:** look stick.
  * **Upper right:** A (green), B (red), X (blue), Y (yellow), the same as a controller.
  * **Lower right:** Fire (big red) and Aim (yellow).
  * **Top centre:** Back (scoreboard in online matches) and Start (pause).
* **Controllers:** Bluetooth or USB (Xbox, DualSense, DualShock, Switch Pro and others). Using one hides the touch controls. More controllers give more split-screen players.
* **Keyboard and mouse** work as on PC.
* **Back button or gesture:** pause / back in menus.

## Online

* Open the Perfect Menu, then **Online Matches**. The default server is `perfectdarklobby.m03.ca`.
* To leave a match, hold Back + Start on a controller, or touch both Back and Start.
* The APK must be built from the same game build as the server's (`versionName` shows it: `web-<build>`). Otherwise the menu says the server runs a different version.

## Building

On Linux:

```
sudo apt install openjdk-17-jdk-headless git wget unzip cmake python3
web/build.sh          # the WebAssembly build (needs the Emscripten SDK)
android/build.sh      # -> android/dist/perfectdark-<build>.apk
```

On its first run, `android/build.sh` downloads and caches its dependencies in `~/androidsdk` (`ANDROID_CACHE` changes that):

* the Android command-line tools, which install SDK platform 35, build-tools, NDK r27 and CMake (accepting the Android SDK licenses)
* Gradle 8.10
* the SDL2 2.32.10 source (checksum checked), whose Java side is copied into the app at build time
* wabt (wasm2c)

The APK is signed with a key made once in `~/androidsdk/perfectdark.keystore`. Keep that key: updates must be signed with the same one to install over an earlier build.

The ABIs are arm64-v8a (phones and tablets) and x86_64 (Chromebooks and the emulator). The game's memory is bounds-checked, so it doesn't need a signal handler next to Android's own.
