# Native host for pd.wasm

Runs the game's **WebAssembly build** natively. `pd.wasm` is translated to C with
[wasm2c](https://github.com/WebAssembly/wabt/tree/main/wasm2c) at build time and compiled together
with a small host. No interpreter or JIT is involved.

Online matches need every player to compute exactly the same game, and joining a match copies the
game's memory. So a native player can only be in a match with browser players if they run the
**identical** build. This host does that. It is the base of the PS5 version (`ps5/`) and works on
Linux for testing.

`pd.wasm` reaches the outside world only through its imports, and this host implements them:

| File | What it covers |
| --- | --- |
| `src/main.c` | instantiation, `main(argc, argv)` like Emscripten's runtime, the `pdhost_*` interface ([port/include/pdhost.h](../port/include/pdhost.h)) |
| `src/fs.c` | files. The game's `/data` maps to `--data` (the ROM), `/save` to `--save` (Game Pak, `pd.ini`), and everything else to `--tmp`. Also time and heap growth. |
| `src/gl.c` | the ~70 GL calls the renderer makes. Wasm addresses become native pointers, and GLSL ES 3.00 shaders get a native `#version`. |
| `src/platform_sdl.c` | window, GL 3.3 context, keyboard, mouse, up to 8 game controllers and audio, on SDL2 |
| `src/platform_null.c` | no window, input or audio (`--headless` runs) |
| `src/net.c` | online play: the lobby transport of the Online menu and the match client (a C port of `web/net-client.js`), plus `--replay` |
| `src/ws.c`, `src/json.c` | WebSocket client (TLS through mbedTLS) and a small JSON reader |

## Building (Linux)

```
# wabt, for wasm2c and its runtime
git clone --recursive https://github.com/WebAssembly/wabt
cmake -S wabt -B wabt/build -DBUILD_TESTS=OFF && cmake --build wabt/build --target wasm2c

# the game's WebAssembly build (needs the Emscripten SDK)
web/build.sh

# the host; PDHOST_PLATFORM=null builds a headless one without SDL
sudo apt install libsdl2-dev zlib1g-dev   # mbedTLS is fetched by CMake
cmake -S native_host -B build-native -DWABT_DIR=$PWD/wabt
cmake --build build-native -j
```

## Running

Put your NTSC v1.1 ROM in a data directory as `pd.ntsc-final.z64`:

```
build-native/pdhost --data data --save save [--fullscreen] [game options, eg. --skip-intro]
```

### Online

The **Online Matches** menu works the same as in the browser. The default lobby is
`perfectdarklobby.m03.ca`; the CMake option `PDHOST_DEFAULT_LOBBY` changes it.

* **Server addresses:**
  * A host name alone uses `wss://` (port 443).
  * `host:port` uses `ws://`, except ports 443, 8443 and 9443.
  * A full `ws://` or `wss://` URL is used as is.
* **Certificates** aren't checked, since consoles have no CA store.
* **Joining:** picking a match restarts the game instance into it, as the browser reloads the page.
* **Leaving:** hold **Back + Start** on a controller (or **F10**) for a second and a half. The end of a match also takes you back to the Online menu.
* **Testing:** `--server HOST --join ROOM [--name NAME] [--password PW]` joins a match at startup.

Native players and browser players share matches on both the Node server (`web/server.js`) and the Cloudflare relay (`cloudlare_worker_server/`). This was tested on both: the native host joined from the server's snapshot or from a browser's, sent its own snapshot to a browser that rejoined, and the state checks agreed with no desyncs.

### Checking that it computes the same game as the browser

Record a match with the browser build in Node, replay it natively, and compare the state hashes:

```
node web/net/headless.js pd.ntsc-final.z64 3600 1234 --record /tmp/rec
cp /tmp/rec/match.cfg data/
build-native/pdhost --data data --save save --headless --net-match /data/match.cfg \
    --replay /tmp/rec/ticks.bin | grep -E '^[0-9]+ [0-9a-f]{8}$' > native.txt
diff native.txt /tmp/rec/hashes.txt && echo identical
```

The float math must stay exactly WebAssembly's, so the host compiles with
`-ffp-contract=off -fno-fast-math` (no fused multiply-add, no reassociation).
