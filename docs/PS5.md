# How the PS5 version works

The PS5 version is a native folder title for jailbroken consoles (`ps5/`). It doesn't compile the game for the PS5's CPU the way the PC port does. It runs **the WebAssembly build of the game** (`build-web/pd.wasm`, the file browsers run), translated to C and compiled for the console. This page explains why, and how the pieces fit. To build and install it, see [ps5/README.md](../ps5/README.md).

## Why run the WebAssembly build

Online matches are deterministic lockstep (see `port/src/net.c` and `web/net/`):

* Every player's machine runs the whole match from the same seed and the same inputs.
* The machines compare state hashes every couple of seconds.
* A player who joins or rejoins gets a **copy of another machine's game memory**: the simulation arena plus the game's globals, at addresses listed in `pd.snap.json`.

That only works if every player runs **exactly the same program**, with the same memory layout and the same floating-point results down to the last bit. A normal native build of the game has a different layout (64-bit pointers, different struct padding, different global addresses) and different float code (compiler choices, fused multiply-add). It could never share a match with browser players.

So the PS5 runs the browser build itself. [wasm2c](https://github.com/WebAssembly/wabt/tree/main/wasm2c) turns `pd.wasm` into C that does exactly what the WebAssembly does. Its linear memory is one byte array with the same addresses, and its arithmetic follows WebAssembly's IEEE rules. That C is compiled for the console, along with a small "host" that provides everything the game asks of the outside world.

## The layers

```
pd.wasm (the game, identical everywhere)
  │  imports: pdhost_* (input, video, audio), GL, a few libc/WASI calls, lobby/netplay bridge
  ▼
native_host/ (C)                      browser: web/pd-host.js + web/pdhost-lib.js
  main.c       instantiates the game, runs main(), restarts it for online matches
  fs.c         files (/data, /save, /tmp), time, memory growth
  gl.c         GL calls: wasm addresses -> native pointers, shader #version
  net.c        online play (lobby + match client), recorded-match replay
  ws.c json.c  WebSocket client (mbedTLS for wss://), JSON reader
  platform_sdl.c   window, GL context, controllers, keyboard, mouse, audio (SDL2)
  ▼
ps5/ (console glue)
  ps5_main.c   picks folders, calls pdhostMain()
  build.sh     cross-compiles everything and packages the folder title
```

### 1. A small host interface in the web build

Emscripten's SDL2 port talks to the browser through hundreds of JavaScript functions and inline JS snippets. A console can't provide those, so the web build was changed to stop using it. The relevant commit is "Run the web build through a small host interface instead of SDL".

* **`port/include/pdhost.h`** defines about 25 imports with plain signatures:
  * events (keys, mouse, pads, resize) as 8 integers
  * pad buttons and axes in SDL's GameController layout
  * GL context create and swap
  * queued 16-bit audio
  * notifications: fatal error, save written, netplay hash, local input
* **`port/src/web_sdl.c`** implements the part of SDL2 the port actually uses (81 functions) on top of those imports. So `port/src/input.c`, `video.c`, `audio.c` and fast3d didn't change.
  * It also hands glad only the ~70 GL functions fast3d calls.
  * It carries SDL's own scancode names, because `pd.ini` stores key binds by name.
* **In the browser**, `web/pdhost-lib.js` (an Emscripten JS library) forwards the imports to `web/pd-host.js`, which handles keyboard, mouse, the Gamepad API, Web Audio and canvas sizing.

The result: `pd.wasm` went from 369 imports to 121, of which 71 are GL. A native host has to implement only those.

### 2. The native host (`native_host/`)

* **Build.** CMake runs `wasm2c pd.wasm --num-outputs 8`, which produces about 34 MB of C in 8 files so they compile in parallel. It compiles that output with the host and with wasm2c's runtime (`wasm-rt-impl.c`, `wasm-rt-mem-impl.c`).
* **`pdbuild.h`** is generated at configure time. It holds:
  * the build id: the first 12 hex digits of the sha1 of `pd.wasm`, which the lobby checks
  * the 666 snapshot ranges from `pd.snap.json`, used to make snapshots for other players
* **Starting the game** copies what Emscripten's JS runtime does: `__wasm_call_ctors`, then `__main_argc_argv` with the arguments placed on the wasm stack (`_emscripten_stack_alloc`). The arguments match the browser's: `--basedir /data --savedir /save`, plus any extras.
* **Blocking instead of asyncify.** In the browser the game yields every frame or tick through ASYNCIFY (`netJsWaitForTick`, `videoWebWaitForFrame`, `emscripten_sleep`). A native host simply **blocks** inside those imports. The asyncify state never changes, so the instrumented code runs straight through.
* **Files.** `fs.c` maps the game's paths: `/data` → the ROM folder, `/save` → saves, and everything else → a scratch folder. It speaks Emscripten's ABI:
  * WASI errno numbers (ENOENT is 44, not 2)
  * musl `O_*` flags
  * the wasm32 `struct stat` (96 bytes, `st_size` at offset 24) and `struct tm` (44 bytes)
* **GL.** `gl.c` turns wasm addresses into native pointers:
  * Generated ids (`glGen*`) are written straight into wasm memory.
  * `glGetString` results are copied into the game's heap through its `malloc` export.
  * Shaders are GLSL ES 3.00. On desktop GL and the PS5's GL, `#version 300 es` becomes `#version 330 core`.
  * The game must take the same "ES 3.0" path it takes in a browser, so:
    * the version string says "OpenGL ES 3.0"
    * the host reports **one** made-up extension, because glad gives up when there are none
* **Same results.** The C is compiled with `-ffp-contract=off -fno-fast-math` (no fused multiply-add, no reassociation).
* **Checked by replay.** `web/net/headless.js --record` records a match with the browser build, and `pdhost --replay` runs it natively. All state hashes must be identical, and they are.

### 3. Online play on native hosts

* **Lobby.** `net.c` implements the lobby transport the in-game **Online Matches** menu uses (`lobbyJs*` imports, see `port/src/lobby.c`), over its own WebSocket client.
* **Match client.** It also ports the match half of `web/net-client.js`:
  * hello and join; starting a match fresh (relay server)
  * the tick queue (`netPushTick`, up to 600 ticks ahead)
  * snapshots: zlib raw inflate, then writing the ranges and calling `netAfterRestore`
  * answering a relay's snapshot requests with its own snapshot
  * names queued before their tick
  * resyncs
  * reconnecting with the player's token
  * sending inputs only when they change, plus a heartbeat
  * hash reports every 120 ticks
  * the end-of-match result
* **Joining restarts the game.** A running game can't be switched into a match, which is why the browser reloads the page. Here, picking a match calls `hostRestart()`, which `longjmp`s out of the import back to `runInstance()`. That frees the wasm instance and starts a new one with `--net-match /tmp/net/match.cfg --net-slot N`. After the match, the instance restarts normally with the Online menu opened (`lobbyJsTakeReturnFlag`).
* **Leaving:** hold Back + Start (or F10) for 1.5 seconds.
* **Tested** on Linux with the same code: a native player and a browser player shared matches on the Node server and on the Cloudflare relay. Each joined from the other's snapshot, and the state checks agreed with no desyncs.

## The PS5-specific part (`ps5/`)

**Toolchain:** everything public, pinned, and fetched by `ps5/build.sh` into `~/ps5sdk`. It follows the same setup as the [Ship of Harkinian PS5 port](https://github.com/mshivam019/oot64-ps5):

| Piece | What it gives |
| --- | --- |
| [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate) at `c93771e` | `prospero-clang18` compiler wrapper, the PS5 payload SDK, zlib, the `libc.prx` runtime shim, FSELF signing and folder-title packaging |
| [ps5-opengl SDK 0.3.0](https://github.com/blackbearreloaded/ps5-opengl/releases/tag/v0.3.0) (sha256 checked) | OpenGL 3.3/4.6 core on the PS5 GPU (Mesa over AGC), with EGL |
| [PS5 SDL fork](https://github.com/ps5-payload-dev/SDL) at `8c56053`, built with the SDK's `integration/SDL2/build.py` | window, GL context, DualSense/DualShock controllers (the backend reports a GameController mapping), audio |
| wabt | wasm2c |

**How the title is put together:**
1. The native host is cross-compiled as a **static library**: `-DPDHOST_LIBRARY=ON` with `ps5/toolchain.cmake`.
   * `PDHOST_BOUNDS_CHECK` makes wasm2c check every memory access instead of relying on guard pages. The console can't reserve the 8 GB of address space guard pages need.
   * The memory then comes from `calloc`/`realloc` (`WASM_RT_USE_MMAP=0`).
2. The title is linked the same way as the GL SDK's own SDL example (`integration/SDL2/folder.py`). The steps are in `ps5/build.sh`:
   * the template's packaging, plus the SDK's `runtime_shims.c` (which sends stdout and stderr to `/download0/ps5-opengl.log`)
   * the SDK's `app_heap.c`, with its heap raised from 128 MiB to 1 GiB (`PS5_HEAP_MB`), because the game's memory alone starts at 256 MiB
   * `malloc`, `calloc`, `realloc`, `free`, `posix_memalign` and `malloc_usable_size` wrapped by that heap
   * the SDK's linker script and symbol map, and its AGC import stubs
   * one link group: the host library, mbedTLS, zlib, SDL2, `libPS5OpenGLCore33.a`, libc++ and compiler-rt, plus `ScePad`, `SceUserService`, `SceSystemService` and `SceAudioOut`
3. `ps5/src/ps5_main.c`:
   * reads the ROM from `/app0/assets/pd.ntsc-final.z64`
   * keeps saves in `/app0/UserData` (or `/download0/perfectdark` if that isn't writable)
   * calls `pdhostMain()` with `--fullscreen`
4. The title id is `PPSA06400`. `tools/init-project.sh` sets it in `param.json`.

**Problems hit while making it build:**
* **The compiler wrapper needs `PS5_PAYLOAD_SDK` exported**, or every compile fails, including CMake's compiler test.
* **The boilerplate installs zlib under `.deps/native/zlib/root/usr`**, not `root`.
* **The console's libc has no `localtime_r`.** The host now works out the date itself (UTC; the game only logs it).
* **SDL's audio backend is switched off** in the SDK's SDL build. `build.sh` turns it back on with two `sed` edits, the same change the Ship of Harkinian port makes.
* **TLS random seeding:** mbedTLS's entropy sources may be blocked in the sandbox, so on the console the TLS random generator is seeded from `arc4random_buf`. Certificates aren't checked (there's no CA store).
* **Linker:** the payload SDK's linker wrapper looks for `ld.lld` from the default LLVM, so both `lld-18` and `lld` need installing.

## Status and what to check first

* **Built, signed and packaged; not yet run on a console.** The shared code is proven on Linux: identical hashes and online play with browsers.
* On the console, check `/download0/ps5-opengl.log` first. **Memory is the likeliest problem.** If the title doesn't start, rebuild with a smaller `PS5_HEAP_MB` (for example 768).
* **Licensing:** the eboot links the GPL-3 ps5-opengl SDK, so GPL terms apply if you share builds. Never share a build with a ROM in it.
* **Build id:** a PS5 build only joins matches on a server running the same `pd.wasm` build id. Each commit changes it, because the version string is part of the build. Rebuild the PS5 title whenever the server's build changes.
