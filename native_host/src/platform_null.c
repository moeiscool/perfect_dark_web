// Platform layer without a window, input or audio: for running the game headless (--headless),
// eg. to replay recorded matches and compare state hashes with the browser build.
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "host.h"

int platInit(void)
{
	return 0;
}

double platNowUs(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

void platSleepMs(uint32_t ms)
{
	usleep(ms * 1000);
}

void *platGlProc(const char *name) { return NULL; }
const char *platGlslVersion(void) { return NULL; }
void platMessage(const char *title, const char *text) { }
int32_t platGlCreate(int32_t depth, int32_t stencil) { return 0; }
void platGlSwap(void) { }
void platDrawableSize(int32_t *w, int32_t *h) { *w = 640; *h = 480; }
int32_t platPollEvent(int32_t *ev) { return 0; }
void platTextInput(int32_t on) { }
void platShowCursor(int32_t on) { }
int32_t platPadButton(int32_t id, int32_t button) { return 0; }
int32_t platPadAxis(int32_t id, int32_t axis) { return 0; }
const char *platPadName(int32_t id) { return ""; }
int32_t platPadType(int32_t id) { return 0; }
int32_t platPadRumble(int32_t id, int32_t lo, int32_t hi, int32_t ms) { return -1; }
int32_t platAudioOpen(int32_t freq, int32_t channels) { return 0; }
void platAudioQueue(const void *data, int32_t bytes) { }
int32_t platAudioQueued(void) { return 0; }
void platAudioPause(int32_t on) { }
