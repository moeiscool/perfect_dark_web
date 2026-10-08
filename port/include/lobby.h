#ifndef _IN_LOBBY_H
#define _IN_LOBBY_H

#include <PR/ultratypes.h>

/**
 * Online lobby client, used by the in-game Online menu (port/src/onlinemenu.c).
 *
 * Speaks the lobby protocol of web/net/lobby.js (JSON messages over a WebSocket at /net) so every
 * platform sees the same list of matches. Only the transport and the step into a match are
 * platform specific (lobbyPlatform* in port/src/lobby.c): in the browser the page's WebSocket is
 * used and the match runs in web/net-client.js; other platforms don't have a transport yet.
 */

#define LOBBY_MAX_ROOMS 12
#define LOBBY_MAX_SLOTS 4
#define LOBBY_MAX_OPTIONS 24
#define LOBBY_SERVER_LEN 64

enum lobbystate {
	LOBBY_OFFLINE,     // not connected
	LOBBY_CONNECTING,  // waiting for the server
	LOBBY_ONLINE,      // connected, room list is live
	LOBBY_FAILED,      // couldn't connect or the server refused us (see lobbyGetStatus)
	LOBBY_ENTERING,    // handing over to the match
};

struct lobbyroom {
	char id[16];
	char name[33];
	char arena[24];
	char scenario[24];
	char players[LOBBY_MAX_SLOTS][13]; // "" for a free slot
	s32 numslots;
	s32 numplayers;
	s32 free;      // slots a new player can take
	s32 reserved;  // slots held for players reconnecting
	s32 bots;
	s32 locked;    // needs a password
	s32 playing;   // false once the match is over
	s32 timeleft;  // seconds, -1 for no limit
};

struct lobbycreate {
	char name[33];
	char password[33];
	s32 stage;       // arena id, -1 for random
	s32 scenario;
	s32 weaponset;
	s32 timelimit;   // minutes
	s32 scorelimit;  // 0 = none
	s32 bots;
	s32 botskill;
	s32 teams;
};

// server: host[:port], or a ws:// / wss:// URL; "" means the default (the game's own server)
void lobbyConnect(const char *server);
void lobbyDisconnect(void);
// pumps the connection; call every frame while the online menu is open
void lobbyTick(void);

s32 lobbyGetState(void);
const char *lobbyGetStatus(void);
// the server currently used ("" = default) and its display name
const char *lobbyGetServer(void);
const char *lobbyGetServerDisplay(void);
void lobbySetServer(const char *server);
const char *lobbyGetDefaultServer(void);

s32 lobbyGetNumRooms(void);
const struct lobbyroom *lobbyGetRoom(s32 index);
const struct lobbyroom *lobbyFindRoom(const char *id);

// choices the server offers for new matches
s32 lobbyGetNumArenas(void);
const char *lobbyGetArenaName(s32 index);
s32 lobbyGetArenaId(s32 index);
s32 lobbyGetNumScenarios(void);
const char *lobbyGetScenarioName(s32 index);
s32 lobbyGetNumWeaponSets(void);
const char *lobbyGetWeaponSetName(s32 index);

void lobbyCreate(const struct lobbycreate *settings);
void lobbyJoin(const char *roomid, const char *password);

// true if the game should open the online menu once the main menu shows (back from a match)
s32 lobbyShouldReturnToMenu(void);

#endif
