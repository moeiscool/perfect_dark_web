// Platform layer on SDL2: window and GL context, keyboard, mouse, game controllers, audio.
// Used on Linux and on PS5 (SDL2 port from ps5-payload-dev). The game's own SDL code runs inside
// pd.wasm on port/src/web_sdl.c, which speaks the same SDL semantics, so this mostly forwards.
#include <stdio.h>
#include <string.h>
#include <SDL.h>
#include "host.h"
#include "../../port/include/pdhost.h"

static SDL_Window *window;
static SDL_GLContext glctx;
static SDL_AudioDeviceID audioDev;
static Uint64 perfFreq;
static int resendPads;

// controllers by pad id (the game's "device index" order); a slot is freed when its pad goes away
static SDL_GameController *pads[PDHOST_MAX_PADS];
static SDL_JoystickID padInstance[PDHOST_MAX_PADS];

#ifndef PDHOST_GLSL_VERSION
#define PDHOST_GLSL_VERSION "330 core"
#endif

int platInit(void)
{
#ifdef __PROSPERO__
	SDL_SetMainReady();
#endif
	SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
	if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS | SDL_INIT_GAMECONTROLLER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return -1;
	}
	perfFreq = SDL_GetPerformanceFrequency();
	return 0;
}

double platNowUs(void)
{
	return (double)SDL_GetPerformanceCounter() * 1e6 / (double)perfFreq;
}

void platSleepMs(uint32_t ms)
{
	SDL_Delay(ms);
}

void *platGlProc(const char *name)
{
	return SDL_GL_GetProcAddress(name);
}

const char *platGlslVersion(void)
{
	return PDHOST_GLSL_VERSION;
}

void platMessage(const char *title, const char *text)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, window);
}

/* ------------------------------------------------------------------------
 * video
 * ------------------------------------------------------------------------ */

int32_t platGlCreate(int32_t depth, int32_t stencil)
{
	if (glctx) {
		return 1;
	}
	if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
		hostLog("SDL video: %s", SDL_GetError());
		return 0;
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, depth > 0 ? depth : 24);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, stencil > 0 ? stencil : 8);
	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
	if (g_HostOpts.fullscreen) {
		flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
	}

	window = SDL_CreateWindow("Perfect Dark", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, flags);
	if (!window) {
		hostLog("SDL window: %s", SDL_GetError());
		return 0;
	}
	glctx = SDL_GL_CreateContext(window);
	if (!glctx) {
		hostLog("SDL GL context: %s", SDL_GetError());
		return 0;
	}
	SDL_GL_MakeCurrent(window, glctx);
	SDL_GL_SetSwapInterval(1);
	return 1;
}

void platGlSwap(void)
{
	if (window) {
		SDL_GL_SwapWindow(window);
	}
}

void platDrawableSize(int32_t *w, int32_t *h)
{
	int ww = 0, hh = 0;
	if (window) {
		SDL_GL_GetDrawableSize(window, &ww, &hh);
	}
	*w = ww;
	*h = hh;
}

/* ------------------------------------------------------------------------
 * events
 * ------------------------------------------------------------------------ */

static void toDrawable(int x, int y, int32_t *ox, int32_t *oy)
{
	int ww = 1, wh = 1, dw = 1, dh = 1;
	if (window) {
		SDL_GetWindowSize(window, &ww, &wh);
		SDL_GL_GetDrawableSize(window, &dw, &dh);
	}
	*ox = ww ? x * dw / ww : x;
	*oy = wh ? y * dh / wh : y;
}

static int padSlot(SDL_JoystickID inst)
{
	for (int i = 0; i < PDHOST_MAX_PADS; i++) {
		if (pads[i] && padInstance[i] == inst) {
			return i;
		}
	}
	return -1;
}

void platResendPads(void)
{
	resendPads = 0;
	for (int i = 0; i < PDHOST_MAX_PADS; i++) {
		if (pads[i]) {
			resendPads |= 1 << i;
		}
	}
}

int platKeyHeld(int scancode)
{
	int n = 0;
	const Uint8 *keys = SDL_GetKeyboardState(&n);
	return scancode >= 0 && scancode < n && keys[scancode];
}

int32_t platPollEvent(int32_t *ev)
{
	SDL_Event e;

	for (int i = 0; resendPads && i < PDHOST_MAX_PADS; i++) {
		if (resendPads & (1 << i)) {
			resendPads &= ~(1 << i);
			memset(ev, 0, 8 * sizeof(*ev));
			ev[0] = PDHOST_EV_PADADDED;
			ev[1] = i;
			return 1;
		}
	}

	while (SDL_PollEvent(&e)) {
		memset(ev, 0, 8 * sizeof(*ev));

		switch (e.type) {
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			ev[0] = e.type == SDL_KEYDOWN ? PDHOST_EV_KEYDOWN : PDHOST_EV_KEYUP;
			ev[1] = e.key.keysym.scancode;
			ev[2] = e.key.keysym.mod;
			ev[3] = e.key.repeat;
			return 1;

		case SDL_TEXTINPUT:
			if ((unsigned char)e.text.text[0] >= 0x80 || !e.text.text[0]) {
				break;
			}
			ev[0] = PDHOST_EV_TEXT;
			ev[1] = e.text.text[0];
			return 1;

		case SDL_MOUSEMOTION:
			ev[0] = PDHOST_EV_MOUSEMOVE;
			toDrawable(e.motion.x, e.motion.y, &ev[1], &ev[2]);
			return 1;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			ev[0] = e.type == SDL_MOUSEBUTTONDOWN ? PDHOST_EV_MOUSEDOWN : PDHOST_EV_MOUSEUP;
			ev[1] = e.button.button;
			toDrawable(e.button.x, e.button.y, &ev[2], &ev[3]);
			return 1;

		case SDL_MOUSEWHEEL:
			ev[0] = PDHOST_EV_WHEEL;
			ev[1] = e.wheel.x;
			ev[2] = e.wheel.y;
			return 1;

		case SDL_CONTROLLERDEVICEADDED: {
			SDL_GameController *gc = SDL_GameControllerOpen(e.cdevice.which);
			if (!gc) {
				break;
			}
			const SDL_JoystickID inst = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc));
			if (padSlot(inst) >= 0) {
				SDL_GameControllerClose(gc);
				break;
			}
			for (int i = 0; i < PDHOST_MAX_PADS; i++) {
				if (!pads[i]) {
					pads[i] = gc;
					padInstance[i] = inst;
					ev[0] = PDHOST_EV_PADADDED;
					ev[1] = i;
					hostLog("controller %d: %s", i, SDL_GameControllerName(gc));
					return 1;
				}
			}
			SDL_GameControllerClose(gc);
			break;
		}

		case SDL_CONTROLLERDEVICEREMOVED: {
			const int i = padSlot(e.cdevice.which);
			if (i < 0) {
				break;
			}
			SDL_GameControllerClose(pads[i]);
			pads[i] = NULL;
			ev[0] = PDHOST_EV_PADREMOVED;
			ev[1] = i;
			return 1;
		}

		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP: {
			const int i = padSlot(e.cbutton.which);
			if (i < 0) {
				break;
			}
			ev[0] = e.type == SDL_CONTROLLERBUTTONDOWN ? PDHOST_EV_PADBUTTONDOWN : PDHOST_EV_PADBUTTONUP;
			ev[1] = i;
			ev[2] = e.cbutton.button;
			return 1;
		}

		case SDL_CONTROLLERAXISMOTION: {
			const int i = padSlot(e.caxis.which);
			if (i < 0) {
				break;
			}
			ev[0] = PDHOST_EV_PADAXIS;
			ev[1] = i;
			ev[2] = e.caxis.axis;
			ev[3] = e.caxis.value;
			return 1;
		}

		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
				ev[0] = PDHOST_EV_RESIZE;
				platDrawableSize(&ev[1], &ev[2]);
				return 1;
			}
			break;

		case SDL_QUIT:
			ev[0] = PDHOST_EV_QUIT;
			return 1;

		default:
			break;
		}
	}

	return 0;
}

void platTextInput(int32_t on)
{
	if (on) {
		SDL_StartTextInput();
	} else {
		SDL_StopTextInput();
	}
}

void platShowCursor(int32_t on)
{
	SDL_ShowCursor(on ? SDL_ENABLE : SDL_DISABLE);
}

/* ------------------------------------------------------------------------
 * controllers
 * ------------------------------------------------------------------------ */

int32_t platPadButton(int32_t id, int32_t button)
{
	if (id < 0 || id >= PDHOST_MAX_PADS || !pads[id] || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX) {
		return 0;
	}
	return SDL_GameControllerGetButton(pads[id], (SDL_GameControllerButton)button);
}

int32_t platPadAxis(int32_t id, int32_t axis)
{
	if (id < 0 || id >= PDHOST_MAX_PADS || !pads[id] || axis < 0 || axis >= SDL_CONTROLLER_AXIS_MAX) {
		return 0;
	}
	return SDL_GameControllerGetAxis(pads[id], (SDL_GameControllerAxis)axis);
}

const char *platPadName(int32_t id)
{
	return (id >= 0 && id < PDHOST_MAX_PADS && pads[id]) ? SDL_GameControllerName(pads[id]) : "";
}

int32_t platPadType(int32_t id)
{
	return (id >= 0 && id < PDHOST_MAX_PADS && pads[id]) ? (int32_t)SDL_GameControllerGetType(pads[id]) : 0;
}

int32_t platPadRumble(int32_t id, int32_t lo, int32_t hi, int32_t ms)
{
	if (id < 0 || id >= PDHOST_MAX_PADS || !pads[id]) {
		return -1;
	}
	return SDL_GameControllerRumble(pads[id], (Uint16)lo, (Uint16)hi, (Uint32)ms);
}

/* ------------------------------------------------------------------------
 * audio
 * ------------------------------------------------------------------------ */

int32_t platAudioOpen(int32_t freq, int32_t channels)
{
	if (audioDev) {
		return freq;
	}
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		hostLog("SDL audio: %s", SDL_GetError());
		return 0;
	}
	SDL_AudioSpec want, have;
	SDL_zero(want);
	want.freq = freq;
	want.format = AUDIO_S16SYS;
	want.channels = (Uint8)channels;
	want.samples = 512;
	// no allowed changes: SDL converts to whatever the device plays
	audioDev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!audioDev) {
		hostLog("SDL audio device: %s", SDL_GetError());
		return 0;
	}
	SDL_PauseAudioDevice(audioDev, 0);
	return freq;
}

void platAudioQueue(const void *data, int32_t bytes)
{
	if (audioDev && data && bytes > 0) {
		SDL_QueueAudio(audioDev, data, (Uint32)bytes);
	}
}

int32_t platAudioQueued(void)
{
	return audioDev ? (int32_t)SDL_GetQueuedAudioSize(audioDev) : 0;
}

void platAudioPause(int32_t on)
{
	if (audioDev) {
		SDL_PauseAudioDevice(audioDev, on);
	}
}
