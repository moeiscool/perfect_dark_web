#ifndef _IN_NET_H
#define _IN_NET_H

#include <PR/ultratypes.h>

/**
 * Netplay: deterministic lockstep multiplayer.
 *
 * Every machine in a match (each player's browser and the server's headless instance) runs the
 * same simulation from the same match config and seed, and advances one tick only when the
 * server's input packet for that tick is available. All 4 player slots are always present; a
 * slot without a human is "vacant" and its player is parked out of the game.
 *
 * During a simulated tick (netIsSimulating()), every input the game reads (controller pads,
 * mouse, ESC) comes from that tick's network inputs for the player being simulated. Outside of
 * it, the input layer reads the local devices, which is how the local player's input is captured.
 */

#define NET_MAX_SLOTS 4

// every player's view in an online match is full screen with this aspect
#define NET_VIEW_ASPECT (16.f / 9.f)

// display list marker (G_NOOP tag) at the start of each player's part of the frame; fast3d skips
// drawing the parts that aren't the local player's
#define NET_GFX_VIEW_MAGIC 0x4e560000u
#define NET_GFX_VIEW_ALL 0xff
#define NET_GFX_VIEW_TAG(playernum) (NET_GFX_VIEW_MAGIC | ((u32)(playernum) & 0xff))

// a throwaway Game Pak, so this machine's saves and unlocks can't affect the match
#define NET_EEPROM_PATH "/tmp/pd-net-eeprom.bin"

#define NETINPUT_FLAG_ESC     0x0001 // ESC was pressed this tick (acts as START in gameplay)
#define NETINPUT_FLAG_FREEAIM 0x0002 // free-aim mouse is driving the crosshair (aimx/aimy valid)

struct netinput {
	u32 buttons;
	s8 stickx;
	s8 sticky;
	s8 rstickx;
	s8 rsticky;
	f32 aimx;     // free-aim crosshair position, -1..1
	f32 aimy;
	f32 mousedx;  // scaled mouse deltas (inputMouseGetScaledDelta)
	f32 mousedy;
	u32 flags;
};

#define NETEVENT_OCCUPY(slot) (1u << (slot))
#define NETEVENT_VACATE(slot) (1u << ((slot) + 8))

// true when the game was started with --net-match
s32 netIsActive(void);

// true when running without video/audio/input (the server's authoritative instance)
s32 netIsHeadless(void);

// true while a networked tick is being simulated (inputs come from the network)
s32 netIsSimulating(void);

// the slot this machine's player controls, or -1 (server)
s32 netGetLocalSlot(void);

// parse --net-* arguments; called early from main()
void netInit(void);

// overrides pd.ini values that the simulation reads; called right after the config is loaded
void netApplyCanonicalSettings(void);

// set up the multiplayer match from the match config; called at the start of mainLoop
void netConfigureMatch(void);

// blocks (yielding to the host) until the next tick's inputs have arrived
void netWaitForTick(void);

// called by mainTick before the joy samples are consumed: feeds this tick's inputs to the game
void netBeginTick(void);

// called by mainTick after the tick has been simulated
void netEndTick(void);

// fills the 4 controller pads from the current tick's inputs (osContGetReadData in netplay)
void netReadPads(void *pads); // OSContPad[4]

// the network input of the player currently being simulated
const struct netinput *netGetCurrentInput(void);

// returns true once per tick if the current player pressed ESC this tick
s32 netConsumeEsc(void);

// true when this tick should be drawn on screen (false while catching up on queued ticks)
s32 netShouldPresent(void);

// hash of the simulation state, used to detect desyncs
u32 netStateHash(void);

// netsim.c: slot vacancy (part of the simulation state)
s32 netSlotIsVacant(s32 slot);
s32 netIsParkingPlayer(void); // true while a vacant slot's player is being removed (no death penalty)
void netSimSetInitialSlots(u32 occupiedmask);
void netSimQueueName(u32 tick, s32 slot, const char *name);
void netSimApplyTick(u32 tick, u32 events);

#endif
