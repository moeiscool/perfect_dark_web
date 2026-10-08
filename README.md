# Perfect Dark port (008 Second Light)

**Primary Public server: <https://perfectdark.royal-cake-5abc.workers.dev>**
**Development Public server: <https://perfectdark.m03.ca>**

This repository contains a work-in-progress port of the [Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark) to modern platforms.

To run the port, you must already have a Perfect Dark ROM, specifically one of the following:
* `ntsc-final`/`US V1.1`/`US Rev 1` (md5 `e03b088b6ac9e0080440efed07c1e40f`).  
  **This is the recommended version to use**.  
  Called `NTSC version 8.7 final` on the boot screen.
* `ntsc-1.0`/`US V1.0` (md5 `7f4171b0c8d17815be37913f535e4e93`).  
  Technically supported, but not recommended.  
  Called `NTSC version 8.7 final` on the boot screen as well.
* `jpn-final` (md5 `538d2b75945eae069b29c46193e74790`).  
  Technically supported, but requires a separate custom-built executable.  
  Called `JPN version 8.9 final` on the boot screen.
* `pal-final` (md5 `d9b5cd305d228424891ce38e71bc9213`).  
  Technically supported, but requires a separate custom-built executable.  
  Called `PAL 8.7 final` on the boot screen.

## Status

The game is in a mostly functional state, with both singleplayer and split-screen multiplayer modes fully working.  
There are minor graphics- and gameplay-related issues, and possibly occasional crashes.

**The following extra features are implemented:**
* mouselook;
* dual analog controller support;
* widescreen resolution support;
* configurable field of view;
* 60 FPS support, including fixes for some framerate-related issues;
* fixes for a couple original bugs and crashes;
* basic mod support, currently enough to load a few custom levels;
* slightly expanded memory heap size;
* experimental high framerate support (up to 240 FPS):
  * enable `Uncap Tickrate` in `Extended Video Options` to activate;
  * in practice the game will have issues running faster than ~165 FPS, so use VSync or `Video.FramerateLimit` to cap it.
* emulate the Transfer Pak functionality the game has on the Nintendo 64 to unlock some cheats automatically.

**The following platforms are officially supported and tested:**
* Windows 7+: i686, x86_64
* Linux: i686, x86_64
* MacOS: x86_64 (OS 10.9+), arm64 (OS 11.0+)
* Nintendo Switch: arm64
* Web browsers (WebAssembly), with online multiplayer: play at <https://perfectdark.m03.ca>
* PS5 (jailbroken, experimental), Android and other native hosts of the web build: see [ps5/](ps5/README.md), [android/](android/README.md) and [native_host/](native_host/README.md)

## Play in your browser

**Primary Public server: <https://perfectdark.royal-cake-5abc.workers.dev>**
**Development Public server: <https://perfectdark.m03.ca>**

Open it in Chrome, Edge, Brave or Firefox on a computer, choose your Perfect Dark ROM (NTSC v1.1, `.z64`/`.v64`/`.n64`) and press **Start**. The ROM stays on your computer: it is read by the page, remembered in your browser for next time, and never uploaded. Your agents, unlocks and settings are saved in the browser too.

### Full window: install it as an app

Browser fullscreen (F11) shows an exit button when the mouse reaches the top of the screen, which gets in the way of looking up. Install the page as an app instead and play in its own window, maximized:

* **Chrome / Edge / Brave:** the install icon at the right of the address bar, or the **Install app** button on the start screen (or menu → *Cast, save and share* → *Install page as app*).
* Then open **Perfect Dark** from your apps. The window has no address bar, and the mouse can reach every edge.

### Controls

| Input | Action |
| --- | --- |
| `W` `A` `S` `D` | Move / strafe |
| Mouse | Moves the crosshair; pushing it to the screen edge turns the view |
| `C` | Recenter the crosshair (the cursor's current spot becomes the new center) |
| Left click / `Space` | Fire |
| Right click | Aim |
| `E` / `R` | Use / reload |
| Mouse wheel | Change weapon |
| `Tab` / `Esc` | Pause menu |
| Gamepad | Press any button to connect it; it works alongside the keyboard and mouse |
| Hold **Select/View** (Xbox), **Minus** (Switch Pro) or the **touchpad** (PlayStation) | Scoreboard in online matches |

The mouse is never locked to the page: the crosshair follows the cursor around a center point. Every binding can be changed in the game's options. Gamepads need an `https://` address (the public server is).

### Saves

Saves and settings are kept in the browser for that site, written as soon as the game saves. Use **Back up saves** on the start screen (or the top-right button while playing) to download them as a file, and **Restore from backup…** to load them in another browser or computer.

### Online matches

1. Start the game and pick your agent. Your agent's name is your name online.
2. In the **Perfect Menu**, choose **Online Matches**. It lists the matches on the server with their free slots (`2/4 free`, `Full`, `Over`; `*` means a password is needed).
3. Select a match to see its scenario, arena, bots, time left and who is playing, then **Join Match**. Or choose **Create Match…** to set up your own:
   * name, arena (or random), scenario, weapon set, time limit (1-20 min), kill limit, 0-8 bots and their skill, teams, password.
4. The page loads the match. Each player sees only their own view, full screen.
   * Hold the scoreboard button (see Controls) to see the scores.
   * "PLAYERNAME has joined" and similar notices appear on the right.
   * **Leave** (top left) or the end of the match takes you back to the game, straight into Online Matches.
   * If you lose the connection or reload, you rejoin the same slot with your score kept for a minute.

Up to 4 players per match plus up to 8 bots, Combat Simulator only (no story missions). Your actions take effect after one round trip to the server, so a nearby server plays best.

**Change Server…** in Online Matches connects to another lobby server: type its address (`host` or `host:port`) or pick the default again. Players see each other's matches when they use the same server. The choice is saved with your settings. Everyone in a match must run the same game build; a server running a different version says so.

## Download

Latest [automatic builds](https://github.com/fgsfdsfgs/perfect_dark/releases/tag/ci-dev-build) for supported platforms:
* [x86_64-windows](https://github.com/fgsfdsfgs/perfect_dark/releases/download/ci-dev-build/pd-x86_64-windows.zip)
* [i686-windows](https://github.com/fgsfdsfgs/perfect_dark/releases/download/ci-dev-build/pd-i686-windows.zip)
* [x86_64-linux](https://github.com/fgsfdsfgs/perfect_dark/releases/download/ci-dev-build/pd-x86_64-linux.tar.gz)
* [i686-linux](https://github.com/fgsfdsfgs/perfect_dark/releases/download/ci-dev-build/pd-i686-linux.tar.gz)
* [arm64-nswitch](https://github.com/fgsfdsfgs/perfect_dark/releases/download/ci-dev-build/pd-arm64-nswitch.zip)

If you are looking for netplay builds (the `port-net` branch), see [this link](https://github.com/fgsfdsfgs/perfect_dark/blob/port-net/README.md#download).

## Running

You must already have a Perfect Dark ROM to run the game, as specified above.  

This assumes that you're using an x86_64 build. If you aren't, replace `x86_64` below with your arch (e.g. `i686`).

1. Create a directory named `data` next to `pd.x86_64` if it's not there.
2. Put your Perfect Dark NTSC ROM named `pd.ntsc-final.z64` into it.
3. Run the `pd.x86_64` executable.

If you want to use a PAL or JPN ROM instead, put them into the `data` directory and run the appropriate executable:
* PAL: ROM name `pd.pal-final.z64`, executable name `pd.pal.x86_64`.
* JPN: ROM name `pd.jpn-final.z64`, executable name `pd.jpn.x86_64`.

Optionally, you can also put your Perfect Dark for GameBoy Color ROM named `pd.gbc` in the `data` directory if you want to emulate having the Nintendo 64's Transfer Pak and unlock some cheats automatically.

Optionally, you can move the data folder to `~/.local/share/perfectdark` on Linux or `~/Library/Application Support/perfectdark` on MacOS.

Additional information can be found in the [wiki](https://github.com/fgsfdsfgs/perfect_dark/wiki).

A GPU supporting OpenGL 3.0/ES3.0 or above is required to run the port.

### Installing the Nintendo Switch version

The Nintendo Switch build ZIP comes with all 3 regions in different folders: `perfectdark`, `perfectdark_pal` and `perfectdark_jpn`.

Take the folder for the region you want and put it into the `/switch` folder on your SD card, then put your ROM into the `data` folder inside of the folder you extracted as described above.

## Controls

1964GEPD-style and Xbox-style bindings are implemented.

N64 pad buttons X and Y (or `X_BUTTON`, `Y_BUTTON` in the code) refer to the reserved buttons `0x40` and `0x80`, which are also leveraged by 1964GEPD.

Support for one controller, two-stick configurations are enabled for 1.2.

Note that the mouse only controls player 1.

Controls can be rebound in `pd.ini`. Default control scheme is as follows:

| Action           | Keyboard and mouse     | Xbox pad                 | N64 pad                   |
| -                | -                      | -                        | -                         |
| Fire / Accept    | LMB/Space              | RT                       | Z Trigger                 |
| Aim mode         | RMB/Z                  | LT                       | R Trigger                 |
| Use / Cancel     | E                      | N/A                      | B                         |
| Use / Accept     | N/A                    | A                        | A                         |
| Crouch cycle     | N/A                    | LS Click                 | `0x80000000` (Extra)      |
| Half-Crouch      | Shift                  | N/A                      | `0x40000000` (Extra)      |
| Full-Crouch      | Control                | N/A                      | `0x20000000` (Extra)      |
| Reload           | R                      | X                        | X `(0x40)`                |
| Previous weapon  | Mousewheel forward     | B                        | D-Left                    |
| Next weapon      | Mousewheel back        | Y                        | Y `(0x80)`                |
| Radial menu      | Q                      | LB                       | D-Down                    |
| Alt fire mode    | F                      | RB                       | L Trigger                 |
| Alt-fire oneshot | `F + LMB` or `E + LMB` | `A + RT` or  `RB + RT`   | `A + Z`     or `L + Z`    |
| Quick-detonate   | `E + Q`   or `E + R`   | `A + B`  or  `A + X`     | `A + D-Left`or `A + X`    |

## Building

### Windows

1. Install [MSYS2](https://www.msys2.org).
2. Open the `MINGW64` prompt if building for x86_64, or the `MINGW32` prompt if building for i686. (**NOTE:** _do not_ use the `MSYS` prompt)
3. Install dependencies:  
   `pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-zlib mingw-w64-x86_64-cmake mingw-w64-x86_64-python3 mingw-w64-i686-toolchain mingw-w64-i686-SDL2 mingw-w64-i686-zlib mingw-w64-i686-cmake mingw-w64-i686-python3 make git`
4. Get the source code:  
   `git clone --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
5. Run `cmake -G"Unix Makefiles" -Bbuild .`.
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.\
6. Run `cmake --build build -j4 -- -O`.
7. The resulting executable will be at `build/pd.x86_64.exe` (or at `build/pd.i686.exe` if building for i686).
8. If you don't know where you downloaded the source to, you can run `explorer .` to open the current directory.

### Linux

1. Ensure you have gcc, g++ (version 10.0+), make, cmake, git, python3 and SDL2 (version 2.0.12+), libGL and ZLib installed on your system.
   * If you wish to crosscompile, you will also need to have libraries and compilers for the target platform installed, e.g. `gcc-multilib` and `g++-multilib` for x86_64 -> i686 crosscompilation.
2. Get the source code:  
   `git clone --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
3. Run the following command:
   * ```cmake -G"Unix Makefiles" -Bbuild .```
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.
   * Add ` -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32` at the end of the command if you want to crosscompile from x86_64 to x86.
4. Run `cmake --build build -j4`.
5. The resulting executable will be at `build/pd.<arch>` (for example `build/pd.x86_64`).

### MacOS

1. Set up Homebrew.
2. Install dependencies:
   * Execute command: `brew install cmake gcc python3 zlib git`
3. Install SDL2:
   * Execute commands:
     ```
     wget http://libsdl.org/release/SDL2-2.30.9.dmg -O SDL2.dmg
     hdiutil mount SDL2.dmg
     sudo cp -vr /Volumes/SDL2/SDL2.framework /Library/Frameworks
     hdiutil detach /Volumes/SDL2
     ```
   * This installs SDL2 system-wide and this is how the automatic builds are done. The game will also look for it in the executable path, so you could
     download it locally instead.
4. Get the source code:  
   `git clone --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
5. Configure:
   * Execute command: `cmake -G"Unix Makefiles" -Bbuild -DCMAKE_OSX_ARCHITECTURES=x86_64 .`
   * Replace `x86_64` with `arm64` if building for an ARM64 Mac.
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.
6. Build:
   * Execute command: `cmake --build build --target pd -j4 --clean-first`
7. The resulting executable will be at `build/pd.<arch>` (for example `build/pd.x86_64`).
   * You might need to execute `chmod +x build/pd.x86-64` before you can run it.

### Nintendo Switch

1. Set up the [devkitA64 environment](https://devkitpro.org/wiki/Getting_Started).
   * On Windows you can do it under MSYS2 or WSL, usually MSYS2 is recommended.
   * If using MSYS2, make sure to use the **MSYS2** shell, **not** MINGW32 or MINGW64.
2. Install host dependencies:
   * On MSYS2: execute command `pacman -Syuu && pacman -S git make cmake python3`
   * On Linux: use your package manager as normal to install the above dependencies.
3. Install Switch toolchain and dependencies:
   * Execute commands:
     ```
     dkp-pacman -Syuu
     dkp-pacman -S devkitA64 libnx switch-zlib switch-sdl2 switch-cmake dkp-toolchain-vars
     ```
   * If in MSYS2 or `dkp-pacman` doesn't work, replace it with just `pacman`.
4. Get the source code:  
   `git clone --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
5. Ensure devkitA64 environment variables are set:
   * Execute command: `source /opt/devkitpro/switchvars.sh`
   * If your `$DEVKITPRO` path is different, substitute that instead or set the variables manually.
6. Configure:
   * Execute command: `aarch64-none-elf-cmake -G"Unix Makefiles" -Bbuild .`
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.
7. Build:
   * Execute command: `make -C build -j4`
8. The resulting executable will be at `build/pd.arm64.nro`.

### Web browser (WebAssembly)

The game can be built to WebAssembly and played in a browser (see [Play in your browser](#play-in-your-browser)). Only the NTSC v1.1 ROM is supported. The ROM is never served: each player picks their own dump on the page.

#### Building

1. Install the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html) (for example to `~/emsdk`), plus `cmake`, `python3` and Node.js 18+.
2. Build: `EMSDK=~/emsdk web/build.sh`. This produces `build-web/` with `pd.js`, `pd.wasm`, `pd.snap.json` and the page (`index.html`, `pd-web.js`, `net-client.js`, `nethost.js`, `manifest.webmanifest`, `sw.js`, `icons/`).

#### Running your own server

```
cd web && npm install          # the lobby's one dependency (ws)
node web/server.js --rom /private/path/pd.ntsc-final.z64
```

`web/server.js` serves `build-web/` and, on the same port, the online lobby (WebSocket `/net`). The lobby runs every match itself, headless, which needs the ROM; it is read from `--rom` and never served (requests for ROM, `.ini` and `.log` files are refused).

| Option | Default | |
| --- | --- | --- |
| `--port` / `--https-port` | 8080 / 8443 | HTTP and HTTPS ports |
| `--no-https` | | HTTP only (eg. behind a proxy or Cloudflare that does HTTPS) |
| `--cert` / `--key` | self-signed | TLS certificate; without them one is generated on first run |
| `--root` | `build-web` | directory to serve |
| `--rom` | | ROM for running online matches; without it the lobby can't host |
| `--max-rooms` | 4 | matches at once (each one uses about one CPU core at 60 ticks/s and a few hundred MB) |
| `--no-lobby` | | serve the game only |
| `--lobby-url` | | lobby server the game's Online menu uses by default (`host[:port]`); default: this server |

Open `https://<server>:8443/` and accept the certificate warning once. Browsers only allow gamepads on HTTPS pages (or `http://localhost`), and installing as an app needs HTTPS too.

#### On Cloudflare Workers

[cloudlare_worker_server/](cloudlare_worker_server/README.md) runs the whole web version (game files and online lobby) on Cloudflare Workers, with no server of your own and no ROM in the cloud: matches are relayed, and the players' games provide the state for players joining. `npm run deploy` there after building.

#### How perfectdark.m03.ca is deployed

Two processes on one server under pm2 ([deploy/ecosystem.config.js](deploy/ecosystem.config.js)), behind a port 80 proxy and Cloudflare, which provides HTTPS:

* `perfectdark.m03.ca` → `server.js --port 4002 --no-https --no-lobby --lobby-url perfectdarklobby.m03.ca` (the game);
* `perfectdarklobby.m03.ca` → `server.js --port 4003 --no-https --rom <private> --max-rooms 2` (lobby and matches).

`deploy/deploy.sh user@host` builds, uploads only the served files and reloads pm2. Both processes must serve the same build: clients and lobby check that their `pd.wasm` matches.

#### Settings and saves in the browser

Saves (the Game Pak, `eeprom.bin`) and settings (`pd.ini`) live in the browser's IndexedDB for the page's address; the `http://` and `https://` addresses of a server count as different sites. The online server choice is `Net.LobbyServer` in `pd.ini` (empty = default). Add `?args=--debug-pak` to the page address to trace the save system in the console.

The free-aim mouse mode used in the browser can be turned on in desktop builds too with `Input.MouseFreeAim=1` in `pd.ini`; `Input.MouseFreeAimScale` and `Input.MouseRecenterKey` adjust it.

#### How online play works

* **Lockstep.** Every machine in a match (the players' browsers and the server) runs the same simulation from the same seed and the same inputs. The server runs the authoritative copy headless (`web/net/headless.js`) and owns the clock: 60 times a second it combines each player's latest input into a tick and sends it to everyone.
* **Determinism.** In a match the game uses a fixed timestep, a seeded RNG, a fresh Game Pak, and the same settings for every player (full-screen 16:9 view, FOV, etc.), regardless of the local `pd.ini` (`port/src/net.c`).
* **Joining mid-match** uses a snapshot of the server's game state (about 1 MB compressed) instead of a replay. All game memory lives in a fixed-address arena (`port/src/simarena.c`) and the build records where the game's globals are (`pd.snap.json`), so a snapshot can be restored in any instance of the same build.
* **Each browser draws only its own player.** The other players' render passes still run, because they contain game logic, but fast3d skips their geometry (`gfx_set_net_view`).
* **Desync detection.** Clients report a state hash every second, and the server resynchronizes anyone who differs.
* **The lobby** (`web/net/lobby.js`) speaks JSON over the WebSocket. The game's Online menu talks to it directly (`port/src/lobby.c`, `port/src/onlinemenu.c`), so any platform with a WebSocket transport sees the same matches; in the browser the page provides the transport and runs the match (`web/net-client.js`).

Testing tools: `node web/net/selftest.js <rom>` checks determinism and snapshots without a browser, and `node web/net/testplayer.js <server url> <rom> [--create]` joins a server as a headless player with scripted inputs.

#### Native and PS5 versions of the web build

Online matches need every player to run the identical game build, so other platforms run the web build itself:

* [native_host/](native_host/README.md) translates `pd.wasm` to C with wasm2c and runs it natively. It is tested on Linux, where it gives the same results as the browser and shares matches with browser players.
* [android/](android/README.md) builds it as an Android APK with touch controls, controllers and keyboard/mouse.
* [ps5/](ps5/README.md) builds it as a PS5 folder title for jailbroken consoles, with online play and split screen for up to 4 controllers. The title builds, but hasn't been run on a console yet.

#### Legal

No ROM is included, and the files a server sends to players (the page, `pd.js`, `pd.wasm`) contain no ROM data or game assets: textures, models, sounds and text are read at run time from the ROM each player supplies, in their own browser. A server that hosts online matches needs its own ROM, which stays private on that server.

### Notes

Alternate compilers or toolchains can be specified by passing `-DCMAKE_TOOLCHAIN_FILE=whatever` as normal. The port does not build with Visual Studio.

You will need to provide a `jpn-final` or `pal-final` ROM to run executables built for those regions, named `pd.jpn-final.z64` or `pd.pal-final.z64`.

It might be possible to build and run the game on platforms that are not specified in the supported platforms list (e.g. Linux on armv7), but this has not been tested.

## Credits

* the original [decompilation project](https://github.com/n64decomp/perfect_dark) authors;
* Ryan Dwyer for the above, additional help, and `pd-extract`;
* doomhack for the only other publicly available [PD porting effort](https://github.com/doomhack/perfect_dark) I could find;
* [sm64-port](https://github.com/sm64-port/sm64-port) authors for the audio mixer and some other changes;
* [Ship of Harkinian team](https://github.com/Kenix3/libultraship/tree/main/src/fast), Emill and MaikelChan for the libultraship version of fast3d that this port uses;
* lieff for [minimp3](https://github.com/lieff/minimp3);
* Mouse Injector and 1964GEPD authors for some of the 60FPS- and mouselook-related fixes;
* Raf for the 64-bit port;
* NicNamSam for the icon;
* everyone who has submitted pull requests and issues to this repository and tested the port;
* probably more I'm forgetting.
