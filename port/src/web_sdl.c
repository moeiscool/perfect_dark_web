#include "platform.h"

#ifdef PLATFORM_WEB

/**
 * The part of SDL2 the port uses, built on the host interface (port/include/pdhost.h) instead of
 * Emscripten's SDL port. That keeps pd.wasm's imports small and identical for every host: the
 * browser (web/pd-host.js) and native hosts that run pd.wasm (eg. on PS5).
 *
 * Behaves like SDL where the port relies on it: events go through the event watchers and the
 * queue, controllers use the GameController layout, the keyboard uses SDL scancodes and names.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <SDL.h>
#include <GLES3/gl3.h>
#include "pdhost.h"

struct _SDL_GameController {
	int id;
};

struct SDL_Window {
	int unused;
};

static struct _SDL_GameController padObjs[PDHOST_MAX_PADS];
static int padConnected[PDHOST_MAX_PADS];
static char padNames[PDHOST_MAX_PADS][64];

static struct SDL_Window theWindow;
static int theContext = 1;
static int glAttrs[SDL_GL_FLOATBUFFERS + 1];

static Uint32 initFlags;
static Uint8 keyState[SDL_NUM_SCANCODES];
static SDL_Keymod modState;
static int mouseX, mouseY, mouseRelX, mouseRelY;
static Uint32 mouseButtons;
static int cursorShown = 1;

#define MAX_WATCHERS 4
static struct { SDL_EventFilter fn; void *data; } watchers[MAX_WATCHERS];

#define QUEUE_SIZE 256
static SDL_Event queue[QUEUE_SIZE];
static int queueHead, queueTail;

/* ------------------------------------------------------------------------
 * events
 * ------------------------------------------------------------------------ */

static void pushEvent(SDL_Event *ev)
{
	ev->common.timestamp = (Uint32)(pdhost_now_us() / 1000.0);

	for (int i = 0; i < MAX_WATCHERS; ++i) {
		if (watchers[i].fn) {
			watchers[i].fn(watchers[i].data, ev);
		}
	}

	const int next = (queueTail + 1) % QUEUE_SIZE;
	if (next != queueHead) {
		queue[queueTail] = *ev;
		queueTail = next;
	}
}

// device index of a pad: its position among the connected pads
static int padDeviceIndex(int id)
{
	int idx = 0;
	for (int i = 0; i < id; ++i) {
		idx += padConnected[i];
	}
	return idx;
}

static int padIdForDeviceIndex(int devidx)
{
	for (int i = 0; i < PDHOST_MAX_PADS; ++i) {
		if (padConnected[i] && devidx-- == 0) {
			return i;
		}
	}
	return -1;
}

static void pumpEvents(void)
{
	int32_t ev[PDHOST_EV_WORDS];

	while (pdhost_poll_event(ev)) {
		SDL_Event e;
		memset(&e, 0, sizeof(e));

		switch (ev[0]) {
		case PDHOST_EV_KEYDOWN:
		case PDHOST_EV_KEYUP:
			if (ev[1] <= 0 || ev[1] >= SDL_NUM_SCANCODES) {
				break;
			}
			keyState[ev[1]] = ev[0] == PDHOST_EV_KEYDOWN;
			modState = (SDL_Keymod)ev[2];
			e.type = ev[0] == PDHOST_EV_KEYDOWN ? SDL_KEYDOWN : SDL_KEYUP;
			e.key.windowID = 1;
			e.key.state = ev[0] == PDHOST_EV_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
			e.key.repeat = ev[3] ? 1 : 0;
			e.key.keysym.scancode = (SDL_Scancode)ev[1];
			e.key.keysym.sym = SDL_SCANCODE_TO_KEYCODE(ev[1]);
			e.key.keysym.mod = (Uint16)ev[2];
			pushEvent(&e);
			break;

		case PDHOST_EV_TEXT:
			e.type = SDL_TEXTINPUT;
			e.text.windowID = 1;
			e.text.text[0] = (char)ev[1];
			pushEvent(&e);
			break;

		case PDHOST_EV_MOUSEMOVE:
			e.type = SDL_MOUSEMOTION;
			e.motion.windowID = 1;
			e.motion.xrel = ev[1] - mouseX;
			e.motion.yrel = ev[2] - mouseY;
			mouseRelX += e.motion.xrel;
			mouseRelY += e.motion.yrel;
			mouseX = e.motion.x = ev[1];
			mouseY = e.motion.y = ev[2];
			e.motion.state = mouseButtons;
			pushEvent(&e);
			break;

		case PDHOST_EV_MOUSEDOWN:
		case PDHOST_EV_MOUSEUP:
			if (ev[1] < 1 || ev[1] > 5) {
				break;
			}
			if (ev[0] == PDHOST_EV_MOUSEDOWN) {
				mouseButtons |= SDL_BUTTON(ev[1]);
			} else {
				mouseButtons &= ~SDL_BUTTON(ev[1]);
			}
			e.type = ev[0] == PDHOST_EV_MOUSEDOWN ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
			e.button.windowID = 1;
			e.button.button = (Uint8)ev[1];
			e.button.state = ev[0] == PDHOST_EV_MOUSEDOWN ? SDL_PRESSED : SDL_RELEASED;
			e.button.clicks = 1;
			mouseX = e.button.x = ev[2];
			mouseY = e.button.y = ev[3];
			pushEvent(&e);
			break;

		case PDHOST_EV_WHEEL:
			e.type = SDL_MOUSEWHEEL;
			e.wheel.windowID = 1;
			e.wheel.x = ev[1];
			e.wheel.y = ev[2];
			e.wheel.preciseX = (float)ev[1];
			e.wheel.preciseY = (float)ev[2];
			pushEvent(&e);
			break;

		case PDHOST_EV_PADADDED:
			if (ev[1] < 0 || ev[1] >= PDHOST_MAX_PADS || padConnected[ev[1]]) {
				break;
			}
			padConnected[ev[1]] = 1;
			padNames[ev[1]][0] = '\0';
			pdhost_pad_name(ev[1], padNames[ev[1]], sizeof(padNames[ev[1]]));
			// SDL reports the joystick, then the game controller, both by device index
			e.type = SDL_JOYDEVICEADDED;
			e.jdevice.which = padDeviceIndex(ev[1]);
			pushEvent(&e);
			e.type = SDL_CONTROLLERDEVICEADDED;
			e.cdevice.which = padDeviceIndex(ev[1]);
			pushEvent(&e);
			break;

		case PDHOST_EV_PADREMOVED:
			if (ev[1] < 0 || ev[1] >= PDHOST_MAX_PADS || !padConnected[ev[1]]) {
				break;
			}
			// removal events carry the instance id, and the pad must still be found by it
			e.type = SDL_CONTROLLERDEVICEREMOVED;
			e.cdevice.which = ev[1] + 1;
			pushEvent(&e);
			padConnected[ev[1]] = 0;
			e.type = SDL_JOYDEVICEREMOVED;
			e.jdevice.which = ev[1] + 1;
			pushEvent(&e);
			break;

		case PDHOST_EV_PADBUTTONDOWN:
		case PDHOST_EV_PADBUTTONUP:
			e.type = ev[0] == PDHOST_EV_PADBUTTONDOWN ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
			e.cbutton.which = ev[1] + 1;
			e.cbutton.button = (Uint8)ev[2];
			e.cbutton.state = ev[0] == PDHOST_EV_PADBUTTONDOWN ? SDL_PRESSED : SDL_RELEASED;
			pushEvent(&e);
			break;

		case PDHOST_EV_PADAXIS:
			e.type = SDL_CONTROLLERAXISMOTION;
			e.caxis.which = ev[1] + 1;
			e.caxis.axis = (Uint8)ev[2];
			e.caxis.value = (Sint16)ev[3];
			pushEvent(&e);
			break;

		case PDHOST_EV_RESIZE:
			e.type = SDL_WINDOWEVENT;
			e.window.windowID = 1;
			e.window.event = SDL_WINDOWEVENT_SIZE_CHANGED;
			e.window.data1 = ev[1];
			e.window.data2 = ev[2];
			pushEvent(&e);
			e.window.event = SDL_WINDOWEVENT_RESIZED;
			pushEvent(&e);
			break;

		case PDHOST_EV_QUIT:
			e.type = SDL_QUIT;
			pushEvent(&e);
			break;

		default:
			break;
		}
	}
}

void SDL_PumpEvents(void)
{
	pumpEvents();
}

int SDL_PollEvent(SDL_Event *event)
{
	pumpEvents();

	if (queueHead == queueTail) {
		return 0;
	}

	if (event) {
		*event = queue[queueHead];
		queueHead = (queueHead + 1) % QUEUE_SIZE;
	}

	return 1;
}

void SDL_AddEventWatch(SDL_EventFilter filter, void *userdata)
{
	for (int i = 0; i < MAX_WATCHERS; ++i) {
		if (!watchers[i].fn) {
			watchers[i].fn = filter;
			watchers[i].data = userdata;
			return;
		}
	}
}

/* ------------------------------------------------------------------------
 * init, hints, errors, time, memory, paths
 * ------------------------------------------------------------------------ */

int SDL_Init(Uint32 flags)
{
	initFlags |= flags;
	return 0;
}

int SDL_InitSubSystem(Uint32 flags)
{
	initFlags |= flags;
	return 0;
}

Uint32 SDL_WasInit(Uint32 flags)
{
	return flags ? (initFlags & flags) : initFlags;
}

void SDL_Quit(void)
{
	initFlags = 0;
}

SDL_bool SDL_SetHint(const char *name, const char *value)
{
	return SDL_TRUE;
}

const char *SDL_GetError(void)
{
	return "";
}

Uint64 SDL_GetPerformanceCounter(void)
{
	return (Uint64)pdhost_now_us();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
	return 1000000;
}

Uint32 SDL_GetTicks(void)
{
	return (Uint32)(pdhost_now_us() / 1000.0);
}

void SDL_free(void *mem)
{
	free(mem);
}

void *SDL_memset(void *dst, int c, size_t len)
{
	return memset(dst, c, len);
}

char *SDL_GetBasePath(void)
{
	return strdup("/");
}

char *SDL_GetPrefPath(const char *org, const char *app)
{
	return strdup("/");
}

char *SDL_GetClipboardText(void)
{
	return strdup("");
}

int SDL_ShowSimpleMessageBox(Uint32 flags, const char *title, const char *message, SDL_Window *window)
{
	pdhost_fatal(message);
	return 0;
}

SDL_RWops *SDL_RWFromFile(const char *file, const char *mode)
{
	return NULL;
}

/* ------------------------------------------------------------------------
 * keyboard and mouse
 * ------------------------------------------------------------------------ */

// SDL's own key names (SDL_keyboard.c, SDL 2.32, zlib license): pd.ini stores key binds by these names
static const char *SDL_scancode_names[SDL_NUM_SCANCODES] = {
	/* 0 */ NULL,
	/* 1 */ NULL,
	/* 2 */ NULL,
	/* 3 */ NULL,
	/* 4 */ "A",
	/* 5 */ "B",
	/* 6 */ "C",
	/* 7 */ "D",
	/* 8 */ "E",
	/* 9 */ "F",
	/* 10 */ "G",
	/* 11 */ "H",
	/* 12 */ "I",
	/* 13 */ "J",
	/* 14 */ "K",
	/* 15 */ "L",
	/* 16 */ "M",
	/* 17 */ "N",
	/* 18 */ "O",
	/* 19 */ "P",
	/* 20 */ "Q",
	/* 21 */ "R",
	/* 22 */ "S",
	/* 23 */ "T",
	/* 24 */ "U",
	/* 25 */ "V",
	/* 26 */ "W",
	/* 27 */ "X",
	/* 28 */ "Y",
	/* 29 */ "Z",
	/* 30 */ "1",
	/* 31 */ "2",
	/* 32 */ "3",
	/* 33 */ "4",
	/* 34 */ "5",
	/* 35 */ "6",
	/* 36 */ "7",
	/* 37 */ "8",
	/* 38 */ "9",
	/* 39 */ "0",
	/* 40 */ "Return",
	/* 41 */ "Escape",
	/* 42 */ "Backspace",
	/* 43 */ "Tab",
	/* 44 */ "Space",
	/* 45 */ "-",
	/* 46 */ "=",
	/* 47 */ "[",
	/* 48 */ "]",
	/* 49 */ "\\",
	/* 50 */ "#",
	/* 51 */ ";",
	/* 52 */ "'",
	/* 53 */ "`",
	/* 54 */ ",",
	/* 55 */ ".",
	/* 56 */ "/",
	/* 57 */ "CapsLock",
	/* 58 */ "F1",
	/* 59 */ "F2",
	/* 60 */ "F3",
	/* 61 */ "F4",
	/* 62 */ "F5",
	/* 63 */ "F6",
	/* 64 */ "F7",
	/* 65 */ "F8",
	/* 66 */ "F9",
	/* 67 */ "F10",
	/* 68 */ "F11",
	/* 69 */ "F12",
	/* 70 */ "PrintScreen",
	/* 71 */ "ScrollLock",
	/* 72 */ "Pause",
	/* 73 */ "Insert",
	/* 74 */ "Home",
	/* 75 */ "PageUp",
	/* 76 */ "Delete",
	/* 77 */ "End",
	/* 78 */ "PageDown",
	/* 79 */ "Right",
	/* 80 */ "Left",
	/* 81 */ "Down",
	/* 82 */ "Up",
	/* 83 */ "Numlock",
	/* 84 */ "Keypad /",
	/* 85 */ "Keypad *",
	/* 86 */ "Keypad -",
	/* 87 */ "Keypad +",
	/* 88 */ "Keypad Enter",
	/* 89 */ "Keypad 1",
	/* 90 */ "Keypad 2",
	/* 91 */ "Keypad 3",
	/* 92 */ "Keypad 4",
	/* 93 */ "Keypad 5",
	/* 94 */ "Keypad 6",
	/* 95 */ "Keypad 7",
	/* 96 */ "Keypad 8",
	/* 97 */ "Keypad 9",
	/* 98 */ "Keypad 0",
	/* 99 */ "Keypad .",
	/* 100 */ NULL,
	/* 101 */ "Application",
	/* 102 */ "Power",
	/* 103 */ "Keypad =",
	/* 104 */ "F13",
	/* 105 */ "F14",
	/* 106 */ "F15",
	/* 107 */ "F16",
	/* 108 */ "F17",
	/* 109 */ "F18",
	/* 110 */ "F19",
	/* 111 */ "F20",
	/* 112 */ "F21",
	/* 113 */ "F22",
	/* 114 */ "F23",
	/* 115 */ "F24",
	/* 116 */ "Execute",
	/* 117 */ "Help",
	/* 118 */ "Menu",
	/* 119 */ "Select",
	/* 120 */ "Stop",
	/* 121 */ "Again",
	/* 122 */ "Undo",
	/* 123 */ "Cut",
	/* 124 */ "Copy",
	/* 125 */ "Paste",
	/* 126 */ "Find",
	/* 127 */ "Mute",
	/* 128 */ "VolumeUp",
	/* 129 */ "VolumeDown",
	/* 130 */ NULL,
	/* 131 */ NULL,
	/* 132 */ NULL,
	/* 133 */ "Keypad ,",
	/* 134 */ "Keypad = (AS400)",
	/* 135 */ NULL,
	/* 136 */ NULL,
	/* 137 */ NULL,
	/* 138 */ NULL,
	/* 139 */ NULL,
	/* 140 */ NULL,
	/* 141 */ NULL,
	/* 142 */ NULL,
	/* 143 */ NULL,
	/* 144 */ NULL,
	/* 145 */ NULL,
	/* 146 */ NULL,
	/* 147 */ NULL,
	/* 148 */ NULL,
	/* 149 */ NULL,
	/* 150 */ NULL,
	/* 151 */ NULL,
	/* 152 */ NULL,
	/* 153 */ "AltErase",
	/* 154 */ "SysReq",
	/* 155 */ "Cancel",
	/* 156 */ "Clear",
	/* 157 */ "Prior",
	/* 158 */ "Return",
	/* 159 */ "Separator",
	/* 160 */ "Out",
	/* 161 */ "Oper",
	/* 162 */ "Clear / Again",
	/* 163 */ "CrSel",
	/* 164 */ "ExSel",
	/* 165 */ NULL,
	/* 166 */ NULL,
	/* 167 */ NULL,
	/* 168 */ NULL,
	/* 169 */ NULL,
	/* 170 */ NULL,
	/* 171 */ NULL,
	/* 172 */ NULL,
	/* 173 */ NULL,
	/* 174 */ NULL,
	/* 175 */ NULL,
	/* 176 */ "Keypad 00",
	/* 177 */ "Keypad 000",
	/* 178 */ "ThousandsSeparator",
	/* 179 */ "DecimalSeparator",
	/* 180 */ "CurrencyUnit",
	/* 181 */ "CurrencySubUnit",
	/* 182 */ "Keypad (",
	/* 183 */ "Keypad )",
	/* 184 */ "Keypad {",
	/* 185 */ "Keypad }",
	/* 186 */ "Keypad Tab",
	/* 187 */ "Keypad Backspace",
	/* 188 */ "Keypad A",
	/* 189 */ "Keypad B",
	/* 190 */ "Keypad C",
	/* 191 */ "Keypad D",
	/* 192 */ "Keypad E",
	/* 193 */ "Keypad F",
	/* 194 */ "Keypad XOR",
	/* 195 */ "Keypad ^",
	/* 196 */ "Keypad %",
	/* 197 */ "Keypad <",
	/* 198 */ "Keypad >",
	/* 199 */ "Keypad &",
	/* 200 */ "Keypad &&",
	/* 201 */ "Keypad |",
	/* 202 */ "Keypad ||",
	/* 203 */ "Keypad :",
	/* 204 */ "Keypad #",
	/* 205 */ "Keypad Space",
	/* 206 */ "Keypad @",
	/* 207 */ "Keypad !",
	/* 208 */ "Keypad MemStore",
	/* 209 */ "Keypad MemRecall",
	/* 210 */ "Keypad MemClear",
	/* 211 */ "Keypad MemAdd",
	/* 212 */ "Keypad MemSubtract",
	/* 213 */ "Keypad MemMultiply",
	/* 214 */ "Keypad MemDivide",
	/* 215 */ "Keypad +/-",
	/* 216 */ "Keypad Clear",
	/* 217 */ "Keypad ClearEntry",
	/* 218 */ "Keypad Binary",
	/* 219 */ "Keypad Octal",
	/* 220 */ "Keypad Decimal",
	/* 221 */ "Keypad Hexadecimal",
	/* 222 */ NULL,
	/* 223 */ NULL,
	/* 224 */ "Left Ctrl",
	/* 225 */ "Left Shift",
	/* 226 */ "Left Alt",
	/* 227 */ "Left GUI",
	/* 228 */ "Right Ctrl",
	/* 229 */ "Right Shift",
	/* 230 */ "Right Alt",
	/* 231 */ "Right GUI",
	/* 232 */ NULL,
	/* 233 */ NULL,
	/* 234 */ NULL,
	/* 235 */ NULL,
	/* 236 */ NULL,
	/* 237 */ NULL,
	/* 238 */ NULL,
	/* 239 */ NULL,
	/* 240 */ NULL,
	/* 241 */ NULL,
	/* 242 */ NULL,
	/* 243 */ NULL,
	/* 244 */ NULL,
	/* 245 */ NULL,
	/* 246 */ NULL,
	/* 247 */ NULL,
	/* 248 */ NULL,
	/* 249 */ NULL,
	/* 250 */ NULL,
	/* 251 */ NULL,
	/* 252 */ NULL,
	/* 253 */ NULL,
	/* 254 */ NULL,
	/* 255 */ NULL,
	/* 256 */ NULL,
	/* 257 */ "ModeSwitch",
	/* 258 */ "AudioNext",
	/* 259 */ "AudioPrev",
	/* 260 */ "AudioStop",
	/* 261 */ "AudioPlay",
	/* 262 */ "AudioMute",
	/* 263 */ "MediaSelect",
	/* 264 */ "WWW",
	/* 265 */ "Mail",
	/* 266 */ "Calculator",
	/* 267 */ "Computer",
	/* 268 */ "AC Search",
	/* 269 */ "AC Home",
	/* 270 */ "AC Back",
	/* 271 */ "AC Forward",
	/* 272 */ "AC Stop",
	/* 273 */ "AC Refresh",
	/* 274 */ "AC Bookmarks",
	/* 275 */ "BrightnessDown",
	/* 276 */ "BrightnessUp",
	/* 277 */ "DisplaySwitch",
	/* 278 */ "KBDIllumToggle",
	/* 279 */ "KBDIllumDown",
	/* 280 */ "KBDIllumUp",
	/* 281 */ "Eject",
	/* 282 */ "Sleep",
	/* 283 */ "App1",
	/* 284 */ "App2",
	/* 285 */ "AudioRewind",
	/* 286 */ "AudioFastForward",
	/* 287 */ "SoftLeft",
	/* 288 */ "SoftRight",
	/* 289 */ "Call",
	/* 290 */ "EndCall",
};

const Uint8 *SDL_GetKeyboardState(int *numkeys)
{
	if (numkeys) {
		*numkeys = SDL_NUM_SCANCODES;
	}
	return keyState;
}

SDL_Keymod SDL_GetModState(void)
{
	return modState;
}

const char *SDL_GetScancodeName(SDL_Scancode scancode)
{
	if (scancode < 0 || scancode >= SDL_NUM_SCANCODES || !SDL_scancode_names[scancode]) {
		return "";
	}
	return SDL_scancode_names[scancode];
}

void SDL_StartTextInput(void)
{
	pdhost_text_input(1);
}

void SDL_StopTextInput(void)
{
	pdhost_text_input(0);
}

Uint32 SDL_GetMouseState(int *x, int *y)
{
	if (x) {
		*x = mouseX;
	}
	if (y) {
		*y = mouseY;
	}
	return mouseButtons;
}

Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
	if (x) {
		*x = mouseRelX;
	}
	if (y) {
		*y = mouseRelY;
	}
	mouseRelX = mouseRelY = 0;
	return mouseButtons;
}

int SDL_SetRelativeMouseMode(SDL_bool enabled)
{
	// the cursor is never captured (the game uses free aim in the browser)
	return enabled ? -1 : 0;
}

int SDL_ShowCursor(int toggle)
{
	if (toggle == SDL_ENABLE || toggle == SDL_DISABLE) {
		cursorShown = toggle == SDL_ENABLE;
		pdhost_show_cursor(cursorShown);
	}
	return cursorShown ? SDL_ENABLE : SDL_DISABLE;
}

/* ------------------------------------------------------------------------
 * game controllers
 * ------------------------------------------------------------------------ */

static SDL_GameController *padFromId(int id)
{
	return (id >= 0 && id < PDHOST_MAX_PADS && padConnected[id]) ? &padObjs[id] : NULL;
}

int SDL_NumJoysticks(void)
{
	int n = 0;
	for (int i = 0; i < PDHOST_MAX_PADS; ++i) {
		n += padConnected[i];
	}
	return n;
}

SDL_bool SDL_IsGameController(int joystick_index)
{
	return padIdForDeviceIndex(joystick_index) >= 0 ? SDL_TRUE : SDL_FALSE;
}

SDL_GameController *SDL_GameControllerOpen(int joystick_index)
{
	const int id = padIdForDeviceIndex(joystick_index);
	if (id < 0) {
		return NULL;
	}
	padObjs[id].id = id;
	return &padObjs[id];
}

void SDL_GameControllerClose(SDL_GameController *gamecontroller)
{
}

SDL_GameController *SDL_GameControllerFromInstanceID(SDL_JoystickID joyid)
{
	return padFromId(joyid - 1);
}

SDL_Joystick *SDL_GameControllerGetJoystick(SDL_GameController *gamecontroller)
{
	return (SDL_Joystick *)gamecontroller;
}

SDL_JoystickID SDL_JoystickInstanceID(SDL_Joystick *joystick)
{
	return joystick ? ((SDL_GameController *)joystick)->id + 1 : -1;
}

SDL_JoystickID SDL_JoystickGetDeviceInstanceID(int device_index)
{
	const int id = padIdForDeviceIndex(device_index);
	return id >= 0 ? id + 1 : -1;
}

static SDL_JoystickGUID padGuid(int id)
{
	// stable per controller model: a hash of its name
	SDL_JoystickGUID guid;
	Uint32 h = 2166136261u;
	memset(&guid, 0, sizeof(guid));
	for (const char *p = (id >= 0 && id < PDHOST_MAX_PADS) ? padNames[id] : ""; *p; ++p) {
		h = (h ^ (Uint8)*p) * 16777619u;
	}
	guid.data[0] = 0x05; // "USB"
	memcpy(&guid.data[4], &h, sizeof(h));
	guid.data[14] = 'p';
	guid.data[15] = 'd';
	return guid;
}

SDL_JoystickGUID SDL_JoystickGetDeviceGUID(int device_index)
{
	return padGuid(padIdForDeviceIndex(device_index));
}

SDL_JoystickGUID SDL_JoystickGetGUID(SDL_Joystick *joystick)
{
	return padGuid(joystick ? ((SDL_GameController *)joystick)->id : -1);
}

void SDL_JoystickGetGUIDString(SDL_JoystickGUID guid, char *pszGUID, int cbGUID)
{
	static const char hex[] = "0123456789abcdef";
	int pos = 0;
	for (int i = 0; i < 16 && pos + 2 < cbGUID; ++i) {
		pszGUID[pos++] = hex[guid.data[i] >> 4];
		pszGUID[pos++] = hex[guid.data[i] & 15];
	}
	if (cbGUID > 0) {
		pszGUID[pos < cbGUID ? pos : cbGUID - 1] = '\0';
	}
}

const char *SDL_JoystickNameForIndex(int device_index)
{
	const int id = padIdForDeviceIndex(device_index);
	return id >= 0 ? padNames[id] : NULL;
}

const char *SDL_GameControllerNameForIndex(int joystick_index)
{
	return SDL_JoystickNameForIndex(joystick_index);
}

const char *SDL_GameControllerName(SDL_GameController *gamecontroller)
{
	return gamecontroller ? padNames[gamecontroller->id] : NULL;
}

SDL_GameControllerType SDL_GameControllerTypeForIndex(int joystick_index)
{
	const int id = padIdForDeviceIndex(joystick_index);
	return id >= 0 ? (SDL_GameControllerType)pdhost_pad_type(id) : SDL_CONTROLLER_TYPE_UNKNOWN;
}

SDL_GameControllerType SDL_GameControllerGetType(SDL_GameController *gamecontroller)
{
	return gamecontroller ? (SDL_GameControllerType)pdhost_pad_type(gamecontroller->id) : SDL_CONTROLLER_TYPE_UNKNOWN;
}

int SDL_JoystickIsHaptic(SDL_Joystick *joystick)
{
	return SDL_FALSE;
}

void SDL_GameControllerUpdate(void)
{
	pumpEvents();
}

Sint16 SDL_GameControllerGetAxis(SDL_GameController *gamecontroller, SDL_GameControllerAxis axis)
{
	return (gamecontroller && padConnected[gamecontroller->id]) ? (Sint16)pdhost_pad_axis(gamecontroller->id, axis) : 0;
}

Uint8 SDL_GameControllerGetButton(SDL_GameController *gamecontroller, SDL_GameControllerButton button)
{
	return (gamecontroller && padConnected[gamecontroller->id]) ? (Uint8)pdhost_pad_button(gamecontroller->id, button) : 0;
}

void SDL_GameControllerSetPlayerIndex(SDL_GameController *gamecontroller, int player_index)
{
}

SDL_bool SDL_GameControllerHasRumble(SDL_GameController *gamecontroller)
{
	return gamecontroller ? SDL_TRUE : SDL_FALSE;
}

int SDL_GameControllerRumble(SDL_GameController *gamecontroller, Uint16 low, Uint16 high, Uint32 ms)
{
	return gamecontroller ? pdhost_pad_rumble(gamecontroller->id, low, high, ms) : -1;
}

// the host already presents every pad in the GameController layout
int SDL_GameControllerAddMapping(const char *mappingString)
{
	return 0;
}

int SDL_GameControllerAddMappingsFromRW(SDL_RWops *rw, int freerw)
{
	return 0;
}

/* ------------------------------------------------------------------------
 * window and GL
 * ------------------------------------------------------------------------ */

SDL_Window *SDL_CreateWindow(const char *title, int x, int y, int w, int h, Uint32 flags)
{
	return &theWindow;
}

void SDL_DestroyWindow(SDL_Window *window)
{
}

void SDL_ShowWindow(SDL_Window *window)
{
}

void SDL_SetWindowTitle(SDL_Window *window, const char *title)
{
}

void SDL_SetWindowSize(SDL_Window *window, int w, int h)
{
}

void SDL_SetWindowPosition(SDL_Window *window, int x, int y)
{
}

void SDL_GetWindowPosition(SDL_Window *window, int *x, int *y)
{
	if (x) {
		*x = 0;
	}
	if (y) {
		*y = 0;
	}
}

int SDL_SetWindowFullscreen(SDL_Window *window, Uint32 flags)
{
	// the page handles fullscreen (and installing as an app)
	return 0;
}

void SDL_MaximizeWindow(SDL_Window *window)
{
}

void SDL_RestoreWindow(SDL_Window *window)
{
}

Uint32 SDL_GetWindowID(SDL_Window *window)
{
	return 1;
}

Uint32 SDL_GetWindowFlags(SDL_Window *window)
{
	return SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
}

int SDL_GetWindowDisplayIndex(SDL_Window *window)
{
	return 0;
}

static int currentMode(SDL_DisplayMode *mode)
{
	int32_t w = 0, h = 0;
	pdhost_drawable_size(&w, &h);
	memset(mode, 0, sizeof(*mode));
	mode->format = SDL_PIXELFORMAT_RGB888;
	mode->w = w;
	mode->h = h;
	mode->refresh_rate = 60;
	return 0;
}

int SDL_GetNumDisplayModes(int displayIndex)
{
	return 1;
}

int SDL_GetDisplayMode(int displayIndex, int modeIndex, SDL_DisplayMode *mode)
{
	return modeIndex == 0 ? currentMode(mode) : -1;
}

int SDL_GetCurrentDisplayMode(int displayIndex, SDL_DisplayMode *mode)
{
	return currentMode(mode);
}

int SDL_GetDesktopDisplayMode(int displayIndex, SDL_DisplayMode *mode)
{
	return currentMode(mode);
}

SDL_DisplayMode *SDL_GetClosestDisplayMode(int displayIndex, const SDL_DisplayMode *mode, SDL_DisplayMode *closest)
{
	currentMode(closest);
	return closest;
}

int SDL_SetWindowDisplayMode(SDL_Window *window, const SDL_DisplayMode *mode)
{
	return 0;
}

int SDL_GL_SetAttribute(SDL_GLattr attr, int value)
{
	if (attr >= 0 && attr < (int)(sizeof(glAttrs) / sizeof(*glAttrs))) {
		glAttrs[attr] = value;
	}
	return 0;
}

int SDL_GL_GetAttribute(SDL_GLattr attr, int *value)
{
	if (attr == SDL_GL_CONTEXT_PROFILE_MASK) {
		*value = SDL_GL_CONTEXT_PROFILE_ES;
	} else if (attr == SDL_GL_CONTEXT_MAJOR_VERSION) {
		*value = 3;
	} else if (attr == SDL_GL_CONTEXT_MINOR_VERSION) {
		*value = 0;
	} else if (attr >= 0 && attr < (int)(sizeof(glAttrs) / sizeof(*glAttrs))) {
		*value = glAttrs[attr];
	} else {
		*value = 0;
	}
	return 0;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window *window)
{
	if (!pdhost_gl_create(glAttrs[SDL_GL_DEPTH_SIZE], glAttrs[SDL_GL_STENCIL_SIZE])) {
		return NULL;
	}
	return &theContext;
}

int SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context)
{
	return 0;
}

void SDL_GL_SwapWindow(SDL_Window *window)
{
	pdhost_gl_swap();
}

int SDL_GL_SetSwapInterval(int interval)
{
	return 0;
}

int SDL_GL_GetSwapInterval(void)
{
	return 1;
}

void SDL_GL_GetDrawableSize(SDL_Window *window, int *w, int *h)
{
	int32_t ww = 0, hh = 0;
	pdhost_drawable_size(&ww, &hh);
	if (w) {
		*w = ww;
	}
	if (h) {
		*h = hh;
	}
}

// only the GL functions the renderer uses (port/fast3d), so they are all a host has to provide
#define GLFN(name) { #name, (void *)name }
static const struct { const char *name; void *fn; } glFunctions[] = {
	GLFN(glActiveTexture), GLFN(glAttachShader), GLFN(glBindBuffer), GLFN(glBindFramebuffer),
	GLFN(glBindRenderbuffer), GLFN(glBindTexture), GLFN(glBindVertexArray), GLFN(glBlendFunc),
	GLFN(glBlitFramebuffer), GLFN(glBufferData), GLFN(glCheckFramebufferStatus), GLFN(glClear),
	GLFN(glClearColor), GLFN(glCompileShader), GLFN(glCreateProgram), GLFN(glCreateShader),
	GLFN(glDeleteBuffers), GLFN(glDeleteFramebuffers), GLFN(glDeleteProgram),
	GLFN(glDeleteRenderbuffers), GLFN(glDeleteShader), GLFN(glDeleteTextures),
	GLFN(glDeleteVertexArrays), GLFN(glDepthFunc), GLFN(glDepthMask), GLFN(glDepthRangef),
	GLFN(glDetachShader), GLFN(glDisable), GLFN(glDisableVertexAttribArray), GLFN(glDrawArrays),
	GLFN(glEnable), GLFN(glEnableVertexAttribArray), GLFN(glFinish), GLFN(glFlush),
	GLFN(glFramebufferRenderbuffer), GLFN(glFramebufferTexture2D), GLFN(glGenBuffers),
	GLFN(glGenFramebuffers), GLFN(glGenRenderbuffers), GLFN(glGenTextures), GLFN(glGenVertexArrays),
	GLFN(glGenerateMipmap), GLFN(glGetAttribLocation), GLFN(glGetError), GLFN(glGetFloatv),
	GLFN(glGetIntegerv), GLFN(glGetProgramInfoLog), GLFN(glGetProgramiv), GLFN(glGetShaderInfoLog),
	GLFN(glGetShaderiv), GLFN(glGetString), GLFN(glGetStringi), GLFN(glGetUniformLocation),
	GLFN(glLinkProgram), GLFN(glPixelStorei), GLFN(glPolygonOffset), GLFN(glReadBuffer),
	GLFN(glReadPixels), GLFN(glRenderbufferStorage), GLFN(glRenderbufferStorageMultisample),
	GLFN(glScissor), GLFN(glShaderSource), GLFN(glTexImage2D), GLFN(glTexParameterf),
	GLFN(glTexParameteri), GLFN(glTexSubImage2D), GLFN(glUniform1f), GLFN(glUniform1i),
	GLFN(glUseProgram), GLFN(glVertexAttribPointer), GLFN(glViewport),
};
#undef GLFN

void *SDL_GL_GetProcAddress(const char *proc)
{
	for (size_t i = 0; i < sizeof(glFunctions) / sizeof(*glFunctions); ++i) {
		if (!strcmp(glFunctions[i].name, proc)) {
			return glFunctions[i].fn;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------
 * audio: one output device, queued
 * ------------------------------------------------------------------------ */

SDL_AudioDeviceID SDL_OpenAudioDevice(const char *device, int iscapture, const SDL_AudioSpec *desired,
		SDL_AudioSpec *obtained, int allowed_changes)
{
	if (iscapture || !desired) {
		return 0;
	}
	const int rate = pdhost_audio_open(desired->freq, desired->channels);
	if (rate <= 0) {
		return 0;
	}
	if (obtained) {
		*obtained = *desired;
		obtained->format = AUDIO_S16SYS;
		// the host resamples, so the game keeps the rate it asked for
		obtained->size = desired->samples * desired->channels * 2;
	}
	return 2;
}

int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
	return SDL_OpenAudioDevice(NULL, 0, desired, obtained, 0) ? 0 : -1;
}

void SDL_PauseAudioDevice(SDL_AudioDeviceID dev, int pause_on)
{
	pdhost_audio_pause(pause_on);
}

int SDL_QueueAudio(SDL_AudioDeviceID dev, const void *data, Uint32 len)
{
	pdhost_audio_queue(data, (int32_t)len);
	return 0;
}

Uint32 SDL_GetQueuedAudioSize(SDL_AudioDeviceID dev)
{
	return (Uint32)pdhost_audio_queued();
}

#endif
