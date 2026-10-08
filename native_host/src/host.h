#ifndef PDHOST_NATIVE_HOST_H
#define PDHOST_NATIVE_HOST_H

/**
 * Native host for pd.wasm: runs the exact WebAssembly build the browsers run (translated to C by
 * wasm2c at build time), so a native player can be in the same online match as browser players.
 *
 * The game talks to its host through its imports: the pdhost_* interface (port/include/pdhost.h),
 * the GL functions the renderer uses, the lobby/netplay bridge, and the few libc/WASI calls
 * Emscripten leaves to the host. This directory implements them in C:
 *   main.c      instantiation, arguments, paths
 *   fs.c        files (the game's /data, /save and /tmp map to host directories), time, memory
 *   gl.c        GL calls (wasm pointers -> native pointers), GLSL ES 3.00 -> 3.30
 *   platform_*  window, GL context, input, controllers, audio (SDL2 on Linux and PS5)
 *   net.c       netplay ticks (replay files for testing) and the lobby transport
 */

#include <stdint.h>
#include <stddef.h>
#include "pd.h"

struct w2c_env {
	int unused;
};

struct w2c_wasi__snapshot__preview1 {
	int unused;
};

extern w2c_pd g_pd;

struct hostopts {
	const char *dataDir;   // the game's /data: the ROM (pd.ntsc-final.z64)
	const char *saveDir;   // the game's /save: Game Pak and settings
	const char *tmpDir;    // the game's /tmp and anything else
	const char *replay;    // tick packets to feed the game (netplay testing)
	int fullscreen;
};

extern struct hostopts g_HostOpts;

int pdhostMain(int argc, char **argv);

static inline uint8_t *wmem(void)
{
	return w2c_pd_memory(&g_pd)->data;
}

static inline uint64_t wmemSize(void)
{
	return w2c_pd_memory(&g_pd)->size;
}

// a native pointer for a wasm address (NULL for 0)
static inline void *wptr(uint32_t addr)
{
	return addr ? (void *)(wmem() + addr) : NULL;
}

static inline uint32_t wload32(uint32_t addr)
{
	uint32_t v;
	__builtin_memcpy(&v, wmem() + addr, 4);
	return v;
}

static inline void wstore32(uint32_t addr, uint32_t v)
{
	__builtin_memcpy(wmem() + addr, &v, 4);
}

// copies a string into a new block of the game's heap (game's malloc); returns its wasm address
uint32_t hostNewString(const char *s);

void hostLog(const char *fmt, ...);
void hostMkdirs(const char *path);
// ends the running game instance and starts the next one (a match, or back to the menu)
void hostRestart(void) __attribute__((noreturn));
void hostFatal(const char *fmt, ...);

// platform layer (platform_sdl.c, or a console's own)
int platInit(void);
double platNowUs(void);
void platSleepMs(uint32_t ms);
void *platGlProc(const char *name);
const char *platGlslVersion(void);   // replaces "300 es" in the game's shaders, eg. "330 core"

int32_t platGlCreate(int32_t depth, int32_t stencil);
void platGlSwap(void);
void platDrawableSize(int32_t *w, int32_t *h);
int32_t platPollEvent(int32_t *ev);
void platTextInput(int32_t on);
void platShowCursor(int32_t on);
int32_t platPadButton(int32_t id, int32_t button);
int32_t platPadAxis(int32_t id, int32_t axis);
const char *platPadName(int32_t id);
int32_t platPadType(int32_t id);
int32_t platPadRumble(int32_t id, int32_t lo, int32_t hi, int32_t ms);
int32_t platAudioOpen(int32_t freq, int32_t channels);
void platAudioQueue(const void *data, int32_t bytes);
int32_t platAudioQueued(void);
void platAudioPause(int32_t on);
void platMessage(const char *title, const char *text);
int platKeyHeld(int scancode);
void platResendPads(void);   // a new game instance must be told about the connected controllers

// net.c
int netPrepareLaunch(char *cfgGamePath, size_t size);
void netRequestJoin(const char *server, const char *room, const char *password, const char *name);

// gl.c
int glLoadFunctions(void);
void glReportErrors(const char *where);

#endif
