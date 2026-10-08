#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ultra64.h>
#include "platform.h"
#include "system.h"
#include "input.h"
#include "net.h"
#include "simarena.h"
#include "video.h"

#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"
#include "game/title.h"
#include "game/mplayer/mplayer.h"
#include "game/mplayer/scenarios.h"
#include "lib/rng.h"
#include "lib/joy.h"

#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

/**
 * See net.h for the overview.
 *
 * Tick flow (both in the browser and in the server's headless instance):
 *  - the host (JS) receives a tick packet {tick, inputs[4], events} and pushes it with netPushTick
 *  - the stage loop waits in netWaitForTick until a tick is queued, then runs mainTick once
 *  - netBeginTick feeds that tick's inputs into the joy layer, so the game reads them as pads
 *  - netEndTick reports a state hash every NET_HASH_INTERVAL ticks and, on a player's machine,
 *    captures the local player's input and hands it to the host to send to the server
 */

extern u64 g_RngSeed;
extern s32 g_StageNum;
void rngSetSeed(u64 seed);

#define NET_QUEUE_SIZE 1024
#define NET_HASH_INTERVAL 60
#define NET_MAX_KV 128

struct nettick {
	u32 tick;
	u32 events;
	struct netinput inputs[NET_MAX_SLOTS];
};

static s32 netActive = 0;
static s32 netHeadless = 0;
static s32 netLocalSlot = -1;
static u32 netSeed = 1;
static char netMatchPath[512];

static struct nettick netQueue[NET_QUEUE_SIZE];
static u32 netQueueHead = 0; // next to run
static u32 netQueueTail = 0; // next free

static struct nettick netCurrent;
static struct netinput netPushBuffer[NET_MAX_SLOTS];
static struct netinput netLocalInput;
static u32 netEscConsumed = 0; // per-slot bits, cleared every tick
static u32 netTicksRun = 0;
static s32 netSimulating = 0;

static struct extplayerconfig netExtCfgDefaults[MAX_PLAYERS];

/* ------------------------------------------------------------------------- */
/* host bridge                                                                */

#ifdef PLATFORM_WEB
// resolves when the host has queued at least one more tick (see web/net-client.js, web/net/match-worker.js)
EM_ASYNC_JS(void, netJsWaitForTick, (void), {
	if (Module.netWaitForTick) {
		await Module.netWaitForTick();
	} else {
		await new Promise((resolve) => setTimeout(resolve, 16));
	}
});
#endif

static void netHostReportHash(u32 tick, u32 hash)
{
#ifdef PLATFORM_WEB
	EM_ASM({
		if (Module.onNetHash) {
			Module.onNetHash($0 >>> 0, $1 >>> 0);
		}
	}, tick, hash);
#else
	sysLogPrintf(LOG_NOTE, "net: tick %u hash %08x", tick, hash);
#endif
}

static void netHostReportLocalInput(void)
{
#ifdef PLATFORM_WEB
	EM_ASM({
		if (Module.onNetLocalInput) {
			Module.onNetLocalInput($0);
		}
	}, &netLocalInput);
#endif
}

// the host writes NET_MAX_SLOTS netinputs here, then calls netPushTick
#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
struct netinput *netGetPushBuffer(void)
{
	return netPushBuffer;
}

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
s32 netPushTick(u32 tick, u32 events)
{
	const u32 next = (netQueueTail + 1) % NET_QUEUE_SIZE;

	if (next == netQueueHead) {
		sysLogPrintf(LOG_ERROR, "net: tick queue full, dropping tick %u", tick);
		return 0;
	}

	netQueue[netQueueTail].tick = tick;
	netQueue[netQueueTail].events = events;
	memcpy(netQueue[netQueueTail].inputs, netPushBuffer, sizeof(netPushBuffer));
	netQueueTail = next;

	return 1;
}

static char netNameBuffer[32];

// the host writes a UTF-8 name here, then calls netQueueName
#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
char *netGetNameBuffer(void)
{
	return netNameBuffer;
}

// sets a slot's player name when the given tick runs (the server sends this ahead of that tick)
#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
void netQueueName(u32 tick, s32 slot)
{
	netNameBuffer[sizeof(netNameBuffer) - 1] = '\0';
	netSimQueueName(tick, slot, netNameBuffer);
}

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
u32 netGetQueuedTicks(void)
{
	return (netQueueTail + NET_QUEUE_SIZE - netQueueHead) % NET_QUEUE_SIZE;
}

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
u32 netGetTicksRun(void)
{
	return netTicksRun;
}

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
u32 netGetSizeofInput(void)
{
	return sizeof(struct netinput);
}

/* ------------------------------------------------------------------------- */
/* snapshots (see web/net/nethost.js makeSnapshot / restoreSnapshot)          */

extern u8 *g_RomFile;
extern u32 g_RomFileSize;

static u32 netLastTick = 0xffffffff; // none yet

// [arena base, arena high water mark, ROM start, ROM size, last simulated tick]
#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
u32 *netGetSnapshotInfo(void)
{
	static u32 info[5];
	info[0] = (u32)(uintptr_t)simArenaBase();
	info[1] = simArenaHighWater();
	info[2] = (u32)(uintptr_t)g_RomFile;
	info[3] = g_RomFileSize;
	info[4] = netLastTick;
	return info;
}

// called after the host has written a snapshot's memory over this instance's game state
#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
void netAfterRestore(u32 tick, u32 arenahighwater)
{
	simArenaSetHighWater(arenahighwater);
	netLastTick = tick;

	// fast3d caches textures by their address in game memory, which now holds different data
	videoResetTextureCache();

	sysLogPrintf(LOG_NOTE, "net: restored snapshot at tick %u", tick);
}

// drops ticks that haven't run yet (used before a resync snapshot is restored)
#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
void netClearQueue(void)
{
	netQueueHead = netQueueTail;
}

/* ------------------------------------------------------------------------- */
/* match state for the server                                                 */

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
s32 netGetMatchOver(void)
{
	return g_MpSetup.paused == MPPAUSEMODE_GAMEOVER || g_MainIsEndscreen;
}

struct netresult {
	char name[16];
	s32 slot;      // 0-3 for players, -1 for simulants
	s32 kills;     // others killed
	s32 suicides;
	s32 deaths;
	s32 points;
};

static struct netresult netResults[MAX_MPCHRS];

// fills the results table; returns the number of entries (see netGetResultsBuffer)
#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
s32 netGetResults(void)
{
	s32 count = 0;

	for (s32 i = 0; i < g_MpNumChrs && i < MAX_MPCHRS; i++) {
		struct mpchrconfig *mpchr = g_MpAllChrConfigPtrs[i];
		struct netresult *r = &netResults[count];
		s32 kills = 0;

		if (!mpchr) {
			continue;
		}

		memset(r, 0, sizeof(*r));
		snprintf(r->name, sizeof(r->name), "%s", mpchr->name);
		r->name[strcspn(r->name, "\n")] = '\0';
		r->slot = (mpchr >= &g_PlayerConfigsArray[0].base && mpchr <= &g_PlayerConfigsArray[MAX_PLAYERS - 1].base)
			? (s32)(((struct mpplayerconfig *)mpchr) - g_PlayerConfigsArray) : -1;

		for (s32 j = 0; j < MAX_MPCHRS; j++) {
			if (j != i) {
				kills += mpchr->killcounts[j];
			}
		}

		r->kills = kills;
		r->suicides = mpchr->killcounts[i];
		r->deaths = mpchr->numdeaths;
		r->points = mpchr->numpoints;
		count++;
	}

	return count;
}

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
struct netresult *netGetResultsBuffer(void)
{
	return netResults;
}

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE
#endif
s32 netGetSlotVacant(s32 slot)
{
	return netSlotIsVacant(slot);
}

/* ------------------------------------------------------------------------- */
/* match config                                                               */

struct netkv {
	char key[48];
	char value[80];
};

static struct netkv netConfig[NET_MAX_KV];
static s32 netConfigCount = 0;

static void netLoadConfig(const char *path)
{
	FILE *f = fopen(path, "rb");
	char line[256];

	if (!f) {
		sysFatalError("Could not open net match config %s", path);
	}

	while (fgets(line, sizeof(line), f) && netConfigCount < NET_MAX_KV) {
		char *eq = strchr(line, '=');
		char *end;

		if (!eq || line[0] == '#') {
			continue;
		}

		*eq = '\0';
		end = eq + 1 + strcspn(eq + 1, "\r\n");
		*end = '\0';

		strncpy(netConfig[netConfigCount].key, line, sizeof(netConfig[0].key) - 1);
		strncpy(netConfig[netConfigCount].value, eq + 1, sizeof(netConfig[0].value) - 1);
		netConfigCount++;
	}

	fclose(f);
}

static const char *netCfgStr(const char *key, const char *def)
{
	for (s32 i = 0; i < netConfigCount; i++) {
		if (!strcmp(netConfig[i].key, key)) {
			return netConfig[i].value;
		}
	}
	return def;
}

static s32 netCfgInt(const char *key, s32 def)
{
	const char *v = netCfgStr(key, NULL);
	return v ? (s32)strtol(v, NULL, 0) : def;
}

static s32 netCfgSlotInt(s32 slot, const char *field, s32 def)
{
	char key[48];
	snprintf(key, sizeof(key), "slot%d.%s", slot, field);
	return netCfgInt(key, def);
}

static s32 netCfgBotInt(s32 bot, const char *field, s32 def)
{
	char key[48];
	snprintf(key, sizeof(key), "bot%d.%s", bot, field);
	return netCfgInt(key, def);
}

/* ------------------------------------------------------------------------- */

s32 netIsActive(void)
{
	return netActive;
}

s32 netIsHeadless(void)
{
	return netHeadless;
}

s32 netIsSimulating(void)
{
	return netSimulating;
}

s32 netGetLocalSlot(void)
{
	return netLocalSlot;
}

void netInit(void)
{
	const char *path = sysArgGetString("--net-match");

	// remember the built-in per-player settings before pd.ini overrides them, so every machine
	// can use the same values in a match
	memcpy(netExtCfgDefaults, g_PlayerExtCfg, sizeof(netExtCfgDefaults));

	if (!path || !path[0]) {
		return;
	}

	strncpy(netMatchPath, path, sizeof(netMatchPath) - 1);
	netLoadConfig(netMatchPath);

	netActive = 1;
	netHeadless = sysArgCheck("--headless");
	netLocalSlot = netHeadless ? -1 : sysArgGetInt("--net-slot", 0);
	netSeed = (u32)netCfgInt("seed", 1);

	sysLogPrintf(LOG_NOTE, "net: match config %s, seed %u, local slot %d%s",
		netMatchPath, netSeed, netLocalSlot, netHeadless ? ", headless" : "");
}

extern s32 g_OsMemSizeMb;
extern s32 g_MaxExplosions;
extern s32 g_BgunGeMuzzleFlashes;
extern f32 g_ViShakeIntensityMult;
extern s32 g_TickRateDiv;
extern s32 g_TickExtraSleep;

void netApplyCanonicalSettings(void)
{
	g_OsMemSizeMb = 16;
	g_MaxExplosions = MAX_EXPLOSIONS_DEFAULT;
	g_BgunGeMuzzleFlashes = false;
	g_ViShakeIntensityMult = 1.f;
	g_TickRateDiv = 1;
	g_TickExtraSleep = false;

	// the Game Pak (saves, unlocks) must not differ between players; use a fresh one
	remove(NET_EEPROM_PATH);
}

void netConfigureMatch(void)
{
	s32 i;
	s32 numbots;
	u16 botbits = 0;

	if (!netActive) {
		return;
	}

	// everything below may consume random numbers; make that identical everywhere
	rngSetSeed(netSeed);

	g_MpSetup.stagenum = netCfgInt("stage", STAGE_MP_SKEDAR);
	g_MpSetup.scenario = netCfgInt("scenario", MPSCENARIO_COMBAT);
	g_MpSetup.timelimit = netCfgInt("timelimit", 9);          // minutes - 1
	g_MpSetup.scorelimit = netCfgInt("scorelimit", 100);      // kills - 1, >= 100 = none
	g_MpSetup.teamscorelimit = netCfgInt("teamscorelimit", 400);
	g_MpSetup.options = (u32)netCfgInt("options", g_MpSetup.options);
	g_MpSetup.paused = 0;

	// 4 human slots, always; vacant ones are parked by the netplay code
	g_MpSetup.chrslots = 0x000f;

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct mpplayerconfig *cfg = &g_PlayerConfigsArray[i];
		const char *name;
		char key[32];

		mpPlayerSetDefaults(i, true);

		snprintf(key, sizeof(key), "slot%d.name", i);
		name = netCfgStr(key, NULL);
		if (name && name[0]) {
			// names are newline-terminated in PD
			snprintf(cfg->base.name, sizeof(cfg->base.name), "%.13s\n", name);
		}

		cfg->base.mpbodynum = netCfgSlotInt(i, "body", cfg->base.mpbodynum);
		cfg->base.mpheadnum = netCfgSlotInt(i, "head", mpGetMpheadnumByMpbodynum(cfg->base.mpbodynum));
		cfg->base.team = netCfgSlotInt(i, "team", i);
		cfg->handicap = netCfgSlotInt(i, "handicap", 0x80);
		cfg->controlmode = CONTROLMODE_PC;

		// per-player settings that change how input is interpreted come from the match,
		// never from this machine's pd.ini
		g_PlayerExtCfg[i] = netExtCfgDefaults[i];
		g_PlayerExtCfg[i].fovy = 60.f;
		g_PlayerExtCfg[i].fovzoommult = 1.f;
		g_PlayerExtCfg[i].extcontrols = true;
	}

	numbots = netCfgInt("bots", 0);
	if (numbots > MAX_BOTS) {
		numbots = MAX_BOTS;
	}

	for (i = 0; i < numbots; i++) {
		mpCreateBotFromProfile(i, 0);
		g_BotConfigsArray[i].type = netCfgBotInt(i, "type", BOTTYPE_GENERAL);
		g_BotConfigsArray[i].difficulty = netCfgBotInt(i, "difficulty", BOTDIFF_NORMAL);
		g_BotConfigsArray[i].base.team = netCfgBotInt(i, "team", g_BotConfigsArray[i].base.team);
		for (s32 j = 0; j < MAX_PLAYERS; j++) {
			g_MpSimulantDifficultiesPerNumPlayers[i][j] = g_BotConfigsArray[i].difficulty;
		}
		botbits |= 1 << (i + 4);
	}

	g_MpSetup.chrslots = 0x000f | botbits;

	// which slots have a player when the match starts (default: all, for testing)
	netSimSetInitialSlots((u32)netCfgInt("occupied", 0xf));

	// named here rather than by mpGenerateBotNames, which needs a text bank that isn't loaded yet
	// (it produced "(null):2"). Same style as the game: NormalSim, or NormalSim:2 when repeated
	for (i = 0; i < numbots; i++) {
		static const char *diffnames[] = { "MeatSim", "EasySim", "NormalSim", "HardSim", "PerfectSim", "DarkSim" };
		const s32 diff = g_BotConfigsArray[i].difficulty < 6 ? g_BotConfigsArray[i].difficulty : 2;
		s32 same = 0, nth = 0;
		for (s32 j = 0; j < numbots; j++) {
			if (g_BotConfigsArray[j].difficulty == g_BotConfigsArray[i].difficulty) {
				same++;
				if (j <= i) {
					nth++;
				}
			}
		}
		if (same > 1) {
			snprintf(g_BotConfigsArray[i].base.name, sizeof(g_BotConfigsArray[i].base.name), "%s:%d\n", diffnames[diff], nth);
		} else {
			snprintf(g_BotConfigsArray[i].base.name, sizeof(g_BotConfigsArray[i].base.name), "%s\n", diffnames[diff]);
		}
	}

	mpSetWeaponSet(netCfgInt("weaponset", 0));
	scenarioInit();

	// same as mpStartMatch, without the menu round trip
	g_Vars.coopplayernum = -1;
	g_Vars.antiplayernum = -1;
	g_Vars.mpquickteam = MPQUICKTEAM_NONE;
	g_StageNum = g_MpSetup.stagenum;
	setNumPlayers(MAX_PLAYERS);
	g_Vars.perfectbuddynum = 1;

	sysLogPrintf(LOG_NOTE, "net: match on stage 0x%02x, scenario %d, %d bots", g_StageNum, g_MpSetup.scenario, numbots);
}

/* ------------------------------------------------------------------------- */
/* ticking                                                                    */

void netWaitForTick(void)
{
	while (netQueueHead == netQueueTail) {
#ifdef PLATFORM_WEB
		netJsWaitForTick();
#else
		sysSleep(10000);
#endif
	}
}

void netBeginTick(void)
{
	netCurrent = netQueue[netQueueHead];
	netQueueHead = (netQueueHead + 1) % NET_QUEUE_SIZE;
	netEscConsumed = 0;

	// joins, leaves and name changes take effect here, at the same tick on every machine
	netSimApplyTick(netCurrent.tick, netCurrent.events);

	// one joy sample per tick, taken from the network inputs (see netReadPads)
	netSimulating = 1;
	joyStartReadData(&g_PiMesgQueue);
	joyReadData();
}

void netEndTick(void)
{
	netSimulating = 0;
	netTicksRun++;
	netLastTick = netCurrent.tick;

	if ((netCurrent.tick % NET_HASH_INTERVAL) == 0) {
		netHostReportHash(netCurrent.tick, netStateHash());
	}

	if (netLocalSlot >= 0) {
		static s32 menuwasopen = 0;
		const s32 menuopen = g_Menus[netLocalSlot].curdialog != NULL;
		OSContPad pad = { 0 };

		// the local cursor is only used for aiming while this player has no menu open
		// (the game's own calls for this are ignored during the simulation, see inputAutoLockMouse)
		if (menuopen != menuwasopen) {
			inputAutoLockMouse(!menuopen);
			menuwasopen = menuopen;
		}

		// capture the local player's input (outside of the simulation, so the input layer
		// reads this machine's devices) and give it to the host to send to the server
		memset(&netLocalInput, 0, sizeof(netLocalInput));
		inputReadController(0, &pad);
		netLocalInput.buttons = pad.button;
		netLocalInput.stickx = pad.stick_x;
		netLocalInput.sticky = pad.stick_y;
		netLocalInput.rstickx = pad.rstick_x;
		netLocalInput.rsticky = pad.rstick_y;

		if (inputMouseIsFreeAim()) {
			netLocalInput.flags |= NETINPUT_FLAG_FREEAIM;
			inputMouseGetFreeAimPos(&netLocalInput.aimx, &netLocalInput.aimy);
		}

		inputMouseGetScaledDelta(&netLocalInput.mousedx, &netLocalInput.mousedy);

		if (inputKeyJustPressed(VK_ESCAPE)) {
			netLocalInput.flags |= NETINPUT_FLAG_ESC;
		}

		netHostReportLocalInput();
	}
}

void netReadPads(void *padsptr)
{
	OSContPad *pads = padsptr;

	for (s32 i = 0; i < NET_MAX_SLOTS; i++) {
		const struct netinput *in = &netCurrent.inputs[i];
		pads[i].button = in->buttons;
		pads[i].stick_x = in->stickx;
		pads[i].stick_y = in->sticky;
		pads[i].rstick_x = in->rstickx;
		pads[i].rstick_y = in->rsticky;
		pads[i].errnum = 0;
	}
}

const struct netinput *netGetCurrentInput(void)
{
	s32 slot = g_Vars.currentplayernum;

	if (slot < 0 || slot >= NET_MAX_SLOTS) {
		slot = 0;
	}

	return &netCurrent.inputs[slot];
}

s32 netConsumeEsc(void)
{
	s32 slot = g_Vars.currentplayernum;

	if (slot < 0 || slot >= NET_MAX_SLOTS || (netEscConsumed & (1 << slot))) {
		return 0;
	}

	if (netCurrent.inputs[slot].flags & NETINPUT_FLAG_ESC) {
		netEscConsumed |= 1 << slot;
		return 1;
	}

	return 0;
}

s32 netShouldPresent(void)
{
	// draw only the newest tick; older queued ticks are simulated without drawing.
	// called at the start of a frame, while the tick about to run is still queued
	return !netHeadless && netGetQueuedTicks() <= 1;
}

/* ------------------------------------------------------------------------- */
/* desync detection                                                           */

static u32 netHashBytes(u32 h, const void *data, u32 len)
{
	const u8 *p = data;

	// FNV-1a
	for (u32 i = 0; i < len; i++) {
		h ^= p[i];
		h *= 16777619u;
	}

	return h;
}

#define HASHV(h, v) h = netHashBytes(h, &(v), sizeof(v))

u32 netStateHash(void)
{
	u32 h = 2166136261u;
	s32 i;

	HASHV(h, g_RngSeed);
	HASHV(h, g_Vars.lvframenum);

	for (i = 0; i < MAX_PLAYERS; i++) {
		struct player *player = g_Vars.players[i];

		if (player) {
			HASHV(h, player->bondhealth);
			HASHV(h, player->isdead);
			if (player->prop) {
				HASHV(h, player->prop->pos);
			}
		}
	}

	if (g_ChrSlots) {
		for (i = 0; i < g_NumChrSlots; i++) {
			struct chrdata *chr = &g_ChrSlots[i];

			if (chr->chrnum >= 0 && chr->prop) {
				HASHV(h, chr->chrnum);
				HASHV(h, chr->actiontype);
				HASHV(h, chr->damage);
				HASHV(h, chr->prop->pos);
			}
		}
	}

	for (i = 0; i < g_MpNumChrs && i < MAX_MPCHRS; i++) {
		struct mpchrconfig *mpchr = g_MpAllChrConfigPtrs[i];

		if (mpchr) {
			HASHV(h, mpchr->killcounts);
			HASHV(h, mpchr->numdeaths);
			HASHV(h, mpchr->numpoints);
		}
	}

	for (struct prop *prop = g_Vars.activeprops; prop; prop = prop->next) {
		HASHV(h, prop->type);
		HASHV(h, prop->pos);
	}

	return h;
}
