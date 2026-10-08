#include <string.h>
#include <ultra64.h>
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "system.h"
#include "net.h"

/**
 * Netplay state that is part of the simulation, so it's included in snapshots (unlike net.c,
 * which holds this machine's connection state). See web/net/snapranges.js.
 *
 * Every match has 4 player slots. A vacant slot's player is parked: killed without it counting
 * as a death, and never respawned, because a vacant slot only ever receives neutral input.
 * Changes happen through tick events, so they apply at the same tick on every machine.
 */

u8 g_NetSlotVacant[NET_MAX_SLOTS];
static u8 g_NetParking = 0;

// pending name changes, applied when their tick runs
static struct {
	u32 tick;
	s8 slot;
	char name[16];
} g_NetPendingNames[16];

s32 netSlotIsVacant(s32 slot)
{
	return slot >= 0 && slot < NET_MAX_SLOTS && g_NetSlotVacant[slot];
}

s32 netIsParkingPlayer(void)
{
	return g_NetParking;
}

static void netParkPlayer(s32 slot)
{
	struct player *player = g_Vars.players[slot];
	const s32 prevplayernum = g_Vars.currentplayernum;

	if (!player || player->isdead || !player->prop) {
		return;
	}

	setCurrentPlayerNum(slot);
	g_NetParking = 1;
	playerDieByShooter(slot, true);
	g_NetParking = 0;
	setCurrentPlayerNum(prevplayernum);
}

void netSimSetInitialSlots(u32 occupiedmask)
{
	for (s32 i = 0; i < NET_MAX_SLOTS; i++) {
		g_NetSlotVacant[i] = !(occupiedmask & (1 << i));
	}
}

void netSimQueueName(u32 tick, s32 slot, const char *name)
{
	for (s32 i = 0; i < (s32)ARRAYCOUNT(g_NetPendingNames); i++) {
		if (g_NetPendingNames[i].name[0] == '\0') {
			g_NetPendingNames[i].tick = tick;
			g_NetPendingNames[i].slot = (s8)slot;
			snprintf(g_NetPendingNames[i].name, sizeof(g_NetPendingNames[i].name), "%.12s", name);
			return;
		}
	}
}

// called at the start of every networked tick, before the game reads any input
void netSimApplyTick(u32 tick, u32 events)
{
	for (s32 i = 0; i < (s32)ARRAYCOUNT(g_NetPendingNames); i++) {
		if (g_NetPendingNames[i].name[0] && g_NetPendingNames[i].tick <= tick) {
			const s32 slot = g_NetPendingNames[i].slot;
			if (slot >= 0 && slot < NET_MAX_SLOTS) {
				snprintf(g_PlayerConfigsArray[slot].base.name, sizeof(g_PlayerConfigsArray[slot].base.name), "%s\n", g_NetPendingNames[i].name);
			}
			memset(&g_NetPendingNames[i], 0, sizeof(g_NetPendingNames[i]));
		}
	}

	for (s32 slot = 0; slot < NET_MAX_SLOTS; slot++) {
		if (events & NETEVENT_VACATE(slot)) {
			g_NetSlotVacant[slot] = 1;
		}
		if (events & NETEVENT_OCCUPY(slot)) {
			g_NetSlotVacant[slot] = 0;
		}
	}

	// keep vacant players parked (this also parks them at the start of the match)
	if (g_Vars.players[0]) {
		for (s32 slot = 0; slot < NET_MAX_SLOTS; slot++) {
			if (g_NetSlotVacant[slot]) {
				netParkPlayer(slot);
			}
		}
	}
}
