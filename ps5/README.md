# Perfect Dark for PS5 (jailbroken consoles)

A native PS5 folder title. It runs **the same `pd.wasm` as the browser version**: [native_host/](../native_host/README.md) translates it to C with wasm2c, and it's cross-compiled for the console. So PS5 players can:

* **Play online with browser players.** Open **Online Matches** in the Perfect Menu and join the same matches. It uses the same servers: perfectdark.m03.ca's lobby, your own `web/server.js`, or the Cloudflare Worker.
* **Play split screen offline** with up to 4 controllers. Combat Simulator, Co-Operative and Counter-Operative work like on the N64. Each controller needs a signed-in user on the console.

> **Status: built, not yet run on a console.** The build has been checked on the Linux host it shares with the PC version. That host gave identical results to the browser build, played in online matches together with browsers, and gave and received match snapshots. The PS5 binary links and signs, but it hasn't been started on a PS5 yet. The log (see below) is the first thing to check.

## What you need

* A PS5 that can run homebrew folder titles. The [Ship of Harkinian port](https://github.com/mshivam019/oot64-ps5) uses the same setup: firmware 9.00 or lower with etaHEN, kstuff and ShadowMountPlus started from Payload Manager, an FTP server, and a homebrew launcher that registers folder titles.
* Your own Perfect Dark ROM: NTSC v1.1 (`pd.ntsc-final.z64`), the same one the browser version uses.
* To build: Linux or WSL2 (Ubuntu 24.04 or later).

## Building

```
sudo apt install clang-18 lld-18 llvm-18 lld cmake ninja-build python3 python3-venv git wget unzip
# plus the Emscripten SDK for the game itself (see the main README)

web/build.sh        # builds build-web/pd.wasm
ps5/build.sh        # builds ps5/dist/PPSA06400/
```

On its first run, `ps5/build.sh` downloads and caches its dependencies in `~/ps5sdk` (`PS5SDK_ROOT` changes that). All of them are pinned and public:

* [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate), which packages the folder title. It also fetches the [PS5 payload SDK](https://github.com/ps5-payload-dev/sdk) and zlib.
* The [ps5-opengl SDK 0.3.0](https://github.com/blackbearreloaded/ps5-opengl/releases/tag/v0.3.0), which provides OpenGL over the console's GPU. The script checks its checksum.
* The [PS5 SDL2 port](https://github.com/ps5-payload-dev/SDL), built against that SDK with its audio backend turned on.
* [wabt](https://github.com/WebAssembly/wabt), for wasm2c.

| Variable | Default | What it does |
| --- | --- | --- |
| `TITLE_ID` | `PPSA06400` | the title's id |
| `APP_NAME` | `Perfect Dark` | the title's name |
| `PS5_HEAP_MB` | 1024 | the app heap, which holds the game's memory (256 MiB and up) and the GL driver's. Lower it if the title doesn't start. |
| `PD_WASM` | `build-web/pd.wasm` | the game build. **It must be the build your online server runs.** |

## Installing

1. Copy your ROM into the built folder as `ps5/dist/PPSA06400/assets/pd.ntsc-final.z64`.
2. Copy the whole `PPSA06400` folder to the console over FTP, for example to `/mnt/ext1/etaHEN/games/PPSA06400` or `/data/homebrew/PPSA06400`.
3. Register it with your homebrew launcher, then start it from the home screen.

To update, replace `eboot.bin` (and `sce_module/`). Keep `assets/` and `UserData/`.

## Playing

* **Saves and settings** go to `UserData/` in the title folder. If that isn't writable, they go to `/download0/perfectdark`.
* **The log** is written to `/download0/ps5-opengl.log`. It shows the GL driver, the controllers and the online connections.
* **Controls:** DualSense or DualShock 4, in the same layout as a PC controller.
* **Online:** open the Perfect Menu, then **Online Matches**.
  * The default server is `perfectdarklobby.m03.ca`. **Change Server…** picks another.
  * A match starts by restarting the game into it.
  * **Hold Create (Back) + Options (Start)** for a second and a half to leave a match.
  * Your PS5 build must be the same build as the server's, or the menu says the server runs a different version.
* **Split screen:** connect up to 4 controllers, each with its own signed-in user, and start a Combat Simulator, Co-Operative or Counter-Operative game as on the N64.

## How it fits together

* `ps5/src/ps5_main.c` picks the folders and starts the native host.
* `ps5/toolchain.cmake` cross-compiles `native_host/` (pd.wasm included) with the boilerplate's compiler.
* `ps5/build.sh` assembles the title the same way the GL SDK's own SDL example does (`integration/SDL2/folder.py`): the SDK's runtime shims, an app-owned heap with `malloc` wrapped, and the GL driver, SDL2, mbedTLS and zlib linked into `eboot.bin`.
* The game's memory is bounds-checked (`PDHOST_BOUNDS_CHECK`), because the console can't reserve the guard pages a PC uses.
* Secure connections use mbedTLS without certificate checks, since the console has no certificate store to check against.

## Licenses

The title links the ps5-opengl SDK, which is GPL-3.0-or-later, and SDL2 (zlib), mbedTLS (Apache-2.0) and zlib. If you share a built `eboot.bin`, the GPL's terms apply to it. Don't share builds that contain a ROM.
