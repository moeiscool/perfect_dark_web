#ifndef PDHOST_TOUCH_H
#define PDHOST_TOUCH_H

// On-screen touch controls, a virtual game controller (touch.c, built with PDHOST_TOUCH)

#include <stdint.h>
#include <SDL.h>

// feed SDL finger events; freeSlot picks the pad id when the controls first appear
void touchHandleEvent(const SDL_Event *e, int w, int h, int (*freeSlot)(void));
int touchPollEvent(int32_t *ev);      // pad events for the game
int touchSlot(void);                  // the virtual pad's id, or -1
void touchResend(void);               // a new game instance: announce the pad again
void touchHide(void);                 // a physical controller or the keyboard is in use
int touchPadButton(int button);
int touchPadAxis(int axis);
void touchDraw(int w, int h);         // over the finished frame, before the swap

#endif
