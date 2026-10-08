// Netplay and lobby imports of pd.wasm.
//
// For now the native host runs recorded matches (--replay, made with
// `node web/net/headless.js <rom> <ticks> --record <dir>`): it feeds the recorded tick packets to
// the game and prints the game's state hashes, which must equal the ones the browser build
// produced. Online play (the WebSocket client of web/net-client.js, in C) comes next.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host.h"

#define INPUT_SIZE 28
#define TICK_PACKET_SIZE (8 + 4 * INPUT_SIZE)

static FILE *replayFile;
static uint32_t replayTicks;

// the game waits here whenever it has no tick to run
void w2c_env_0x5F_asyncjs_0x5FnetJsWaitForTick(struct w2c_env *e)
{
	if (g_HostOpts.replay) {
		uint8_t packet[TICK_PACKET_SIZE];

		if (!replayFile) {
			replayFile = fopen(g_HostOpts.replay, "rb");
			if (!replayFile) {
				hostFatal("could not open %s", g_HostOpts.replay);
			}
		}

		if (fread(packet, 1, sizeof(packet), replayFile) != sizeof(packet)) {
			fprintf(stderr, "replay: %u ticks done\n", replayTicks);
			fflush(stdout);
			exit(0);
		}

		uint32_t tick, events;
		memcpy(&tick, packet, 4);
		memcpy(&events, packet + 4, 4);
		const uint32_t buf = w2c_pd_netGetPushBuffer(&g_pd);
		memcpy(wmem() + buf, packet + 8, 4 * INPUT_SIZE);
		if (!w2c_pd_netPushTick(&g_pd, tick, events)) {
			hostFatal("tick queue full");
		}
		replayTicks++;
		return;
	}

	platSleepMs(1);
}

void w2c_env_pdhost_net_hash(struct w2c_env *e, u32 tick, u32 hash)
{
	if (g_HostOpts.replay) {
		printf("%u %08x\n", tick, hash);
	}
}

void w2c_env_pdhost_net_local_input(struct w2c_env *e, u32 input)
{
}

/* ------------------------------------------------------------------------
 * lobby transport (port/src/lobby.c): not connected yet on native hosts
 * ------------------------------------------------------------------------ */

u32 w2c_env_lobbyJsOpen(struct w2c_env *e, u32 server)
{
	return 0;
}

void w2c_env_lobbyJsClose(struct w2c_env *e)
{
}

u32 w2c_env_lobbyJsState(struct w2c_env *e)
{
	return 0;
}

void w2c_env_lobbyJsSend(struct w2c_env *e, u32 text)
{
}

u32 w2c_env_lobbyJsRecv(struct w2c_env *e, u32 buf, u32 size)
{
	return 0;
}

void w2c_env_lobbyJsGetString(struct w2c_env *e, u32 which, u32 buf, u32 size)
{
	const char *text = which == 0 ? "perfectdarklobby.m03.ca" : which == 2 ? "native" : "";
	if (size > 0) {
		snprintf((char *)wptr(buf), size, "%s", text);
	}
}

void w2c_env_lobbyJsEnterMatch(struct w2c_env *e, u32 server, u32 room, u32 password, u32 name)
{
}

u32 w2c_env_lobbyJsTakeReturnFlag(struct w2c_env *e)
{
	return 0;
}
