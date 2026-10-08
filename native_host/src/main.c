#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "host.h"

w2c_pd g_pd;
struct hostopts g_HostOpts;

static struct w2c_env g_Env;
static struct w2c_wasi__snapshot__preview1 g_Wasi;

void hostLog(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void hostFatal(const char *fmt, ...)
{
	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(stderr, "FATAL: %s\n", msg);
	platMessage("Perfect Dark", msg);
	exit(1);
}

uint32_t hostNewString(const char *s)
{
	const size_t len = strlen(s) + 1;
	const uint32_t addr = w2c_pd_malloc(&g_pd, (u32)len);
	if (addr) {
		memcpy(wmem() + addr, s, len);
	}
	return addr;
}

static void usage(void)
{
	fprintf(stderr,
		"usage: pdhost [--data DIR] [--save DIR] [--tmp DIR] [--fullscreen] [--replay TICKS] [game options...]\n"
		"  --data DIR     directory with the ROM, pd.ntsc-final.z64 (default: data)\n"
		"  --save DIR     where saves and settings go (default: save)\n"
		"  --tmp DIR      scratch files (default: DIR of --save + /tmp)\n"
		"  --replay FILE  run a recorded match (web/net/headless.js --record) and print its state hashes\n"
		"Game options are passed on, eg. --skip-intro, --headless, --net-match /data/match.cfg\n");
}

int main(int argc, char **argv)
{
	// game arguments: the same layout the browser uses (web/pd-web.js)
	const char *gameArgs[64] = { "pd", "--basedir", "/data", "--savedir", "/save" };
	int ngame = 5;
	static char tmpDefault[1024];

	g_HostOpts.dataDir = "data";
	g_HostOpts.saveDir = "save";

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--data") && i + 1 < argc) {
			g_HostOpts.dataDir = argv[++i];
		} else if (!strcmp(argv[i], "--save") && i + 1 < argc) {
			g_HostOpts.saveDir = argv[++i];
		} else if (!strcmp(argv[i], "--tmp") && i + 1 < argc) {
			g_HostOpts.tmpDir = argv[++i];
		} else if (!strcmp(argv[i], "--replay") && i + 1 < argc) {
			g_HostOpts.replay = argv[++i];
		} else if (!strcmp(argv[i], "--fullscreen")) {
			g_HostOpts.fullscreen = 1;
		} else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			usage();
			return 0;
		} else if (ngame < (int)(sizeof(gameArgs) / sizeof(*gameArgs)) - 1) {
			gameArgs[ngame++] = argv[i];
		}
	}

	if (!g_HostOpts.tmpDir) {
		snprintf(tmpDefault, sizeof(tmpDefault), "%s/tmp", g_HostOpts.saveDir);
		g_HostOpts.tmpDir = tmpDefault;
	}

	if (platInit() != 0) {
		return 1;
	}

	wasm_rt_init();
	wasm2c_pd_instantiate(&g_pd, &g_Env, &g_Wasi);

	// what Emscripten's runtime does before main: static constructors, then main(argc, argv)
	// with the arguments on the wasm stack
	w2c_pd_0x5F_wasm_call_ctors(&g_pd);

	uint32_t argvAddr = w2c_pd_0x5Femscripten_stack_alloc(&g_pd, (u32)((ngame + 1) * 4));
	for (int i = 0; i < ngame; i++) {
		const size_t len = strlen(gameArgs[i]) + 1;
		const uint32_t s = w2c_pd_0x5Femscripten_stack_alloc(&g_pd, (u32)len);
		memcpy(wmem() + s, gameArgs[i], len);
		wstore32(argvAddr + i * 4, s);
	}
	wstore32(argvAddr + ngame * 4, 0);

	const int ret = (int)w2c_pd_0x5F_main_argc_argv(&g_pd, (u32)ngame, argvAddr);

	wasm2c_pd_free(&g_pd);
	wasm_rt_free();
	return ret;
}

/* ------------------------------------------------------------------------
 * pdhost_* imports that map straight to the platform layer
 * ------------------------------------------------------------------------ */

f64 w2c_env_pdhost_now_us(struct w2c_env *e)
{
	return platNowUs();
}

u32 w2c_env_pdhost_poll_event(struct w2c_env *e, u32 ptr)
{
	int32_t ev[8] = { 0 };
	if (!platPollEvent(ev)) {
		return 0;
	}
	memcpy(wmem() + ptr, ev, sizeof(ev));
	return 1;
}

void w2c_env_pdhost_text_input(struct w2c_env *e, u32 on)
{
	platTextInput((int32_t)on);
}

void w2c_env_pdhost_show_cursor(struct w2c_env *e, u32 on)
{
	platShowCursor((int32_t)on);
}

u32 w2c_env_pdhost_gl_create(struct w2c_env *e, u32 depth, u32 stencil)
{
	if (!platGlCreate((int32_t)depth, (int32_t)stencil)) {
		return 0;
	}
	return glLoadFunctions();
}

void w2c_env_pdhost_gl_swap(struct w2c_env *e)
{
	platGlSwap();
}

u32 w2c_env_pdhost_gl_context_lost(struct w2c_env *e)
{
	return 0;
}

void w2c_env_pdhost_drawable_size(struct w2c_env *e, u32 w, u32 h)
{
	int32_t ww = 0, hh = 0;
	platDrawableSize(&ww, &hh);
	wstore32(w, (uint32_t)ww);
	wstore32(h, (uint32_t)hh);
}

u32 w2c_env_pdhost_pad_button(struct w2c_env *e, u32 id, u32 button)
{
	return (u32)platPadButton((int32_t)id, (int32_t)button);
}

u32 w2c_env_pdhost_pad_axis(struct w2c_env *e, u32 id, u32 axis)
{
	return (u32)platPadAxis((int32_t)id, (int32_t)axis);
}

u32 w2c_env_pdhost_pad_name(struct w2c_env *e, u32 id, u32 buf, u32 size)
{
	const char *name = platPadName((int32_t)id);
	if (!name) {
		name = "";
	}
	if (size > 0) {
		snprintf((char *)wptr(buf), size, "%s", name);
	}
	return (u32)strlen(name);
}

u32 w2c_env_pdhost_pad_type(struct w2c_env *e, u32 id)
{
	return (u32)platPadType((int32_t)id);
}

u32 w2c_env_pdhost_pad_rumble(struct w2c_env *e, u32 id, u32 lo, u32 hi, u32 ms)
{
	return (u32)platPadRumble((int32_t)id, (int32_t)lo, (int32_t)hi, (int32_t)ms);
}

u32 w2c_env_pdhost_audio_open(struct w2c_env *e, u32 freq, u32 channels)
{
	return (u32)platAudioOpen((int32_t)freq, (int32_t)channels);
}

void w2c_env_pdhost_audio_queue(struct w2c_env *e, u32 ptr, u32 bytes)
{
	platAudioQueue(wptr(ptr), (int32_t)bytes);
}

u32 w2c_env_pdhost_audio_queued(struct w2c_env *e)
{
	return (u32)platAudioQueued();
}

void w2c_env_pdhost_audio_pause(struct w2c_env *e, u32 on)
{
	platAudioPause((int32_t)on);
}

void w2c_env_pdhost_fatal(struct w2c_env *e, u32 msg)
{
	hostFatal("%s", (const char *)wptr(msg));
}

// the browser has to flush saves to IndexedDB; here they're on disk already
void w2c_env_pdhost_save_written(struct w2c_env *e)
{
}

// the frame wait of the browser build (requestAnimationFrame); the swap already waited for vsync
void w2c_env_0x5F_asyncjs_0x5FvideoWebWaitForFrame(struct w2c_env *e)
{
}
