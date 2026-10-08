#ifndef _IN_PDHOST_H
#define _IN_PDHOST_H

/**
 * The host interface of the WebAssembly build.
 *
 * pd.wasm doesn't talk to the browser directly: everything platform specific goes through these
 * imports (module "env"), plus the GL functions fast3d calls and the usual libc/WASI ones. The
 * browser implements them in web/pd-host.js (through web/pdhost-lib.js); a native host that runs
 * pd.wasm (eg. on PS5) implements the same set, so every platform runs the identical game binary,
 * which online matches require.
 *
 * port/src/web_sdl.c builds the subset of SDL2 the port uses on top of this.
 */

#include <stdint.h>

#define PDHOST_VERSION 1

// events (pdhost_poll_event): ev[0] = type, ev[1..] = arguments
enum {
	PDHOST_EV_NONE = 0,
	PDHOST_EV_KEYDOWN,       // scancode (SDL), modifiers (SDL KMOD_*), repeat
	PDHOST_EV_KEYUP,         // scancode, modifiers
	PDHOST_EV_TEXT,          // character (ASCII)
	PDHOST_EV_MOUSEMOVE,     // x, y (drawable pixels)
	PDHOST_EV_MOUSEDOWN,     // button (1 left, 2 middle, 3 right, 4, 5), x, y
	PDHOST_EV_MOUSEUP,       // button, x, y
	PDHOST_EV_WHEEL,         // x, y (positive y = away from the user)
	PDHOST_EV_PADADDED,      // pad id
	PDHOST_EV_PADREMOVED,    // pad id
	PDHOST_EV_PADBUTTONDOWN, // pad id, button (SDL_GameControllerButton)
	PDHOST_EV_PADBUTTONUP,   // pad id, button
	PDHOST_EV_PADAXIS,       // pad id, axis (SDL_GameControllerAxis), value (-32768..32767)
	PDHOST_EV_RESIZE,        // width, height (drawable pixels)
	PDHOST_EV_QUIT,
};

#define PDHOST_MAX_PADS 8
#define PDHOST_EV_WORDS 8

// pad types (pdhost_pad_type), the same values as SDL_GameControllerType
#define PDHOST_PAD_UNKNOWN 0
#define PDHOST_PAD_XBOX 1
#define PDHOST_PAD_PS4 4
#define PDHOST_PAD_SWITCHPRO 5
#define PDHOST_PAD_PS5 7

// time
double pdhost_now_us(void);

// events: fills ev[PDHOST_EV_WORDS], returns 0 when there are no more
int32_t pdhost_poll_event(int32_t *ev);
void pdhost_text_input(int32_t on);
void pdhost_show_cursor(int32_t on);

// video: the GL context is created by the host (WebGL2 / GLES 3.0 functionality)
int32_t pdhost_gl_create(int32_t depth, int32_t stencil);
void pdhost_gl_swap(void);
int32_t pdhost_gl_context_lost(void);
void pdhost_drawable_size(int32_t *w, int32_t *h);

// gamepads, by id (0..PDHOST_MAX_PADS-1), in the SDL GameController layout
int32_t pdhost_pad_button(int32_t id, int32_t button);
int32_t pdhost_pad_axis(int32_t id, int32_t axis);
int32_t pdhost_pad_name(int32_t id, char *buf, int32_t size);
int32_t pdhost_pad_type(int32_t id);
int32_t pdhost_pad_rumble(int32_t id, int32_t lo, int32_t hi, int32_t ms);

// audio: signed 16-bit interleaved; returns the rate the host plays at (it resamples if needed)
int32_t pdhost_audio_open(int32_t freq, int32_t channels);
void pdhost_audio_queue(const void *data, int32_t bytes);
int32_t pdhost_audio_queued(void);
void pdhost_audio_pause(int32_t on);

// notifications to the host
void pdhost_fatal(const char *msg);
void pdhost_save_written(void);
void pdhost_net_hash(uint32_t tick, uint32_t hash);
void pdhost_net_local_input(const void *input);

#endif
