#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <ultra64.h>
#include "platform.h"
#include "system.h"
#include "lobby.h"
#include "constants.h"
#include "bss.h"
#include "data.h"
#include "types.h"

#ifdef PLATFORM_WEB
#include <emscripten.h>
#endif

// Online lobby client: see port/include/lobby.h and the protocol in web/net/lobby.js.

static char g_LobbyServerCfg[LOBBY_SERVER_LEN + 1]; // "" = default
static char g_LobbyServer[LOBBY_SERVER_LEN + 1];    // the one connected to
static s32 g_LobbyState = LOBBY_OFFLINE;
static char g_LobbyStatus[96];

static struct lobbyroom g_LobbyRooms[LOBBY_MAX_ROOMS];
static s32 g_LobbyNumRooms;

static char g_LobbyArenaNames[LOBBY_MAX_OPTIONS][24];
static s32 g_LobbyArenaIds[LOBBY_MAX_OPTIONS];
static s32 g_LobbyNumArenas;
static char g_LobbyScenarioNames[LOBBY_MAX_OPTIONS][24];
static s32 g_LobbyNumScenarios;
static char g_LobbyWeaponSetNames[LOBBY_MAX_OPTIONS][24];
static s32 g_LobbyNumWeaponSets;

static char g_LobbyPendingPassword[33];
static s32 g_LobbyCreating;
static s32 g_LobbyTransportWasOpen;     // to say hello once each time the transport opens

static char g_LobbyMsg[32 * 1024];

/* ------------------------------------------------------------------------
 * Platform layer: transport and handing over to a match
 * ------------------------------------------------------------------------ */

#define TRANSPORT_CLOSED 0
#define TRANSPORT_CONNECTING 1
#define TRANSPORT_OPEN 2

#ifdef PLATFORM_WEB

// the page's online client (web/net-client.js) owns the WebSocket and the match itself

EM_JS(s32, lobbyJsOpen, (const char *server), {
	if (typeof window === 'undefined' || !window.PDOnline) return 0;
	return window.PDOnline.lobbyOpen(UTF8ToString(server)) ? 1 : 0;
});

EM_JS(void, lobbyJsClose, (void), {
	if (typeof window !== 'undefined' && window.PDOnline) window.PDOnline.lobbyClose();
});

EM_JS(s32, lobbyJsState, (void), {
	if (typeof window === 'undefined' || !window.PDOnline) return 0;
	return window.PDOnline.lobbyState();
});

EM_JS(void, lobbyJsSend, (const char *text), {
	if (typeof window !== 'undefined' && window.PDOnline) window.PDOnline.lobbySend(UTF8ToString(text));
});

// copies the next received message into buf; 1 = got one, 0 = none waiting
EM_JS(s32, lobbyJsRecv, (char *buf, s32 size), {
	if (typeof window === 'undefined' || !window.PDOnline) return 0;
	for (;;) {
		const text = window.PDOnline.lobbyRecv();
		if (text === null || text === undefined) return 0;
		if (lengthBytesUTF8(text) < size) {
			stringToUTF8(text, buf, size);
			return 1;
		}
		// too big to be one we need; drop it
	}
});

EM_JS(void, lobbyJsGetString, (s32 which, char *buf, s32 size), {
	let text = "";
	if (typeof window !== 'undefined' && window.PDOnline) {
		const o = window.PDOnline;
		text = which === 0 ? o.defaultServer() : which === 1 ? (o.build() || "") : which === 2 ? o.clientId() : "";
	}
	stringToUTF8(text || "", buf, size);
});

EM_JS(void, lobbyJsEnterMatch, (const char *server, const char *roomid, const char *password, const char *name), {
	if (typeof window !== 'undefined' && window.PDOnline) {
		window.PDOnline.enterMatch({
			server: UTF8ToString(server),
			roomId: UTF8ToString(roomid),
			password: UTF8ToString(password),
			name: UTF8ToString(name),
		});
	}
});

EM_JS(s32, lobbyJsTakeReturnFlag, (void), {
	if (typeof window === 'undefined' || !window.PDOnline) return 0;
	return window.PDOnline.takeReturnToMenu() ? 1 : 0;
});

static s32 lobbyPlatformOpen(const char *server) { return lobbyJsOpen(server); }
static void lobbyPlatformClose(void) { lobbyJsClose(); }
static s32 lobbyPlatformState(void) { return lobbyJsState(); }
static void lobbyPlatformSend(const char *text) { lobbyJsSend(text); }
static s32 lobbyPlatformRecv(char *buf, s32 size) { return lobbyJsRecv(buf, size); }
static void lobbyPlatformDefaultServer(char *buf, s32 size) { lobbyJsGetString(0, buf, size); }
static void lobbyPlatformBuild(char *buf, s32 size) { lobbyJsGetString(1, buf, size); }
static void lobbyPlatformClientId(char *buf, s32 size) { lobbyJsGetString(2, buf, size); }
static s32 lobbyPlatformTakeReturnFlag(void) { return lobbyJsTakeReturnFlag(); }

static void lobbyPlatformEnterMatch(const char *server, const char *roomid, const char *password, const char *name)
{
	lobbyJsEnterMatch(server, roomid, password, name);
}

#else

// no transport on this platform yet (a native port would add a WebSocket client here)

static s32 lobbyPlatformOpen(const char *server) { return 0; }
static void lobbyPlatformClose(void) {}
static s32 lobbyPlatformState(void) { return TRANSPORT_CLOSED; }
static void lobbyPlatformSend(const char *text) {}
static s32 lobbyPlatformRecv(char *buf, s32 size) { return 0; }
static void lobbyPlatformDefaultServer(char *buf, s32 size) { snprintf(buf, size, "perfectdarklobby.m03.ca"); }
static void lobbyPlatformBuild(char *buf, s32 size) { buf[0] = '\0'; }
static void lobbyPlatformClientId(char *buf, s32 size) { snprintf(buf, size, "native"); }
static s32 lobbyPlatformTakeReturnFlag(void) { return 0; }
static void lobbyPlatformEnterMatch(const char *server, const char *roomid, const char *password, const char *name) {}

#endif

/* ------------------------------------------------------------------------
 * Minimal JSON reading: enough for the lobby's messages
 * ------------------------------------------------------------------------ */

static const char *jsonWs(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
		p++;
	}
	return p;
}

static const char *jsonSkipString(const char *p)
{
	// p at the opening quote
	p++;
	while (*p && *p != '"') {
		if (*p == '\\' && p[1]) {
			p++;
		}
		p++;
	}
	return *p ? p + 1 : p;
}

// returns the position after the value at p
static const char *jsonSkip(const char *p)
{
	p = jsonWs(p);

	if (*p == '"') {
		return jsonSkipString(p);
	}

	if (*p == '{' || *p == '[') {
		s32 depth = 0;

		while (*p) {
			if (*p == '"') {
				p = jsonSkipString(p);
				continue;
			}
			if (*p == '{' || *p == '[') {
				depth++;
			} else if (*p == '}' || *p == ']') {
				if (--depth == 0) {
					return p + 1;
				}
			}
			p++;
		}

		return p;
	}

	// number, true, false, null
	while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') {
		p++;
	}

	return p;
}

// reads a string value into out (ASCII only), returns false if the value isn't a string
static s32 jsonReadString(const char *p, char *out, s32 size)
{
	s32 len = 0;

	p = jsonWs(p);

	if (*p != '"') {
		if (size > 0) {
			out[0] = '\0';
		}
		return false;
	}

	p++;

	while (*p && *p != '"') {
		char c = *p++;

		if (c == '\\' && *p) {
			c = *p++;

			switch (c) {
			case 'n': case 'r': case 't': c = ' '; break;
			case 'u': // \uXXXX: not shown by the game's fonts
				for (s32 i = 0; i < 4 && *p; i++) {
					p++;
				}
				c = '?';
				break;
			default:
				break;
			}
		}

		if ((u8)c >= 0x80) {
			// skip the rest of a UTF-8 sequence
			while (((u8)*p & 0xc0) == 0x80) {
				p++;
			}
			c = '?';
		}

		if (len < size - 1) {
			out[len++] = c;
		}
	}

	if (size > 0) {
		out[len] = '\0';
	}

	return true;
}

static s32 jsonReadInt(const char *p, s32 def)
{
	p = jsonWs(p);

	if (*p == '-' || (*p >= '0' && *p <= '9')) {
		return strtol(p, NULL, 10);
	}

	if (strncmp(p, "true", 4) == 0) {
		return 1;
	}

	if (strncmp(p, "false", 5) == 0) {
		return 0;
	}

	if (*p == '"') {
		char tmp[16];
		jsonReadString(p, tmp, sizeof(tmp));
		return tmp[0] ? strtol(tmp, NULL, 10) : def;
	}

	return def;
}

static s32 jsonIsNull(const char *p)
{
	p = jsonWs(p);
	return *p == '\0' || strncmp(p, "null", 4) == 0;
}

// finds key in the object at p; returns its value or NULL
static const char *jsonGet(const char *p, const char *key)
{
	char name[32];

	p = jsonWs(p);

	if (*p != '{') {
		return NULL;
	}

	p = jsonWs(p + 1);

	while (*p == '"') {
		jsonReadString(p, name, sizeof(name));
		p = jsonWs(jsonSkipString(p));

		if (*p != ':') {
			return NULL;
		}

		p = jsonWs(p + 1);

		if (strcmp(name, key) == 0) {
			return p;
		}

		p = jsonWs(jsonSkip(p));

		if (*p == ',') {
			p = jsonWs(p + 1);
		}
	}

	return NULL;
}

// iterates the elements of an array or the values of an object:
// p = jsonFirst(container); while (p) { ...; p = jsonNext(p); }
// for objects, key receives each key
static const char *jsonFirst(const char *p, char *key, s32 keysize)
{
	if (!p) {
		return NULL;
	}

	p = jsonWs(p);

	if (*p != '[' && *p != '{') {
		return NULL;
	}

	const s32 isobject = *p == '{';
	p = jsonWs(p + 1);

	if (*p == ']' || *p == '}' || *p == '\0') {
		return NULL;
	}

	if (isobject) {
		jsonReadString(p, key, keysize);
		p = jsonWs(jsonSkipString(p));
		p = *p == ':' ? jsonWs(p + 1) : p;
	}

	return p;
}

static const char *jsonNext(const char *p, char *key, s32 keysize)
{
	p = jsonWs(jsonSkip(p));

	if (*p != ',') {
		return NULL;
	}

	p = jsonWs(p + 1);

	if (*p == '"' && key) {
		// object member: "key": value
		const char *q = jsonWs(jsonSkipString(p));

		if (*q == ':') {
			jsonReadString(p, key, keysize);
			return jsonWs(q + 1);
		}
	}

	return p;
}

// writes s as a JSON string literal (with quotes) at out
static s32 jsonWriteString(char *out, s32 size, const char *s)
{
	s32 len = 0;

	if (size < 3) {
		return 0;
	}

	out[len++] = '"';

	while (*s && len < size - 3) {
		const char c = *s++;

		if (c == '"' || c == '\\') {
			out[len++] = '\\';
			out[len++] = c;
		} else if ((u8)c >= 0x20 && (u8)c < 0x7f) {
			out[len++] = c;
		}
	}

	out[len++] = '"';
	out[len] = '\0';

	return len;
}

/* ------------------------------------------------------------------------ */

// keeps characters the game's fonts can draw
static void lobbySanitize(char *s)
{
	for (; *s; s++) {
		if (!isalnum((u8)*s) && !strchr(" .,:;!?'()-_+&#/*", *s)) {
			*s = ' ';
		}
	}
}

static void lobbySetStatus(const char *text)
{
	snprintf(g_LobbyStatus, sizeof(g_LobbyStatus), "%s", text);
}

static void lobbyAgentName(char *out, s32 size)
{
	s32 len = 0;

	for (s32 i = 0; i < (s32)sizeof(g_GameFile.name) && g_GameFile.name[i]; i++) {
		const char c = g_GameFile.name[i];

		if (c >= 0x20 && c < 0x7f && len < size - 1 && len < 12) {
			out[len++] = c;
		}
	}

	while (len > 0 && out[len - 1] == ' ') {
		len--;
	}

	out[len] = '\0';

	if (len == 0) {
		snprintf(out, size, "Agent");
	}
}

static void lobbySendHello(void)
{
	char name[16], build[48], clientid[80];
	char ename[40], ebuild[64], eclient[96];
	char msg[256];

	lobbyAgentName(name, sizeof(name));
	lobbyPlatformBuild(build, sizeof(build));
	lobbyPlatformClientId(clientid, sizeof(clientid));
	jsonWriteString(ename, sizeof(ename), name);
	jsonWriteString(ebuild, sizeof(ebuild), build);
	jsonWriteString(eclient, sizeof(eclient), clientid);

	snprintf(msg, sizeof(msg), "{\"type\":\"hello\",\"name\":%s,\"clientId\":%s,\"build\":%s,\"platform\":\"%s\"}",
			ename, eclient, ebuild,
#ifdef PLATFORM_WEB
			"web"
#else
			"native"
#endif
			);
	lobbyPlatformSend(msg);
}

static void lobbyReadOptions(const char *arr, char names[][24], s32 *count)
{
	char key[8];
	const char *p = jsonFirst(arr, key, sizeof(key));

	*count = 0;

	while (p && *count < LOBBY_MAX_OPTIONS) {
		jsonReadString(p, names[*count], 24);
		lobbySanitize(names[*count]);
		(*count)++;
		p = jsonNext(p, key, sizeof(key));
	}
}

static void lobbyOnWelcome(const char *msg)
{
	char key[12];
	const char *p;
	const char *v = jsonGet(msg, "enabled");

	// arenas: { "50": "Skedar", ... }, sorted by name for the menu
	g_LobbyNumArenas = 0;
	p = jsonFirst(jsonGet(msg, "arenas"), key, sizeof(key));

	while (p && g_LobbyNumArenas < LOBBY_MAX_OPTIONS) {
		s32 i = g_LobbyNumArenas;
		char name[24];

		jsonReadString(p, name, sizeof(name));
		lobbySanitize(name);

		while (i > 0 && strcmp(g_LobbyArenaNames[i - 1], name) > 0) {
			strcpy(g_LobbyArenaNames[i], g_LobbyArenaNames[i - 1]);
			g_LobbyArenaIds[i] = g_LobbyArenaIds[i - 1];
			i--;
		}

		strcpy(g_LobbyArenaNames[i], name);
		g_LobbyArenaIds[i] = strtol(key, NULL, 10);
		g_LobbyNumArenas++;
		p = jsonNext(p, key, sizeof(key));
	}

	lobbyReadOptions(jsonGet(msg, "scenarios"), g_LobbyScenarioNames, &g_LobbyNumScenarios);
	lobbyReadOptions(jsonGet(msg, "weaponSets"), g_LobbyWeaponSetNames, &g_LobbyNumWeaponSets);

	if (v && !jsonReadInt(v, 0)) {
		g_LobbyState = LOBBY_FAILED;
		lobbySetStatus("This server isn't hosting matches right now");
	} else {
		g_LobbyState = LOBBY_ONLINE;
		lobbySetStatus("Connected");
	}
}

static void lobbyOnRooms(const char *msg)
{
	char key[8];
	const char *p = jsonFirst(jsonGet(msg, "rooms"), key, sizeof(key));

	g_LobbyNumRooms = 0;

	while (p && g_LobbyNumRooms < LOBBY_MAX_ROOMS) {
		struct lobbyroom *r = &g_LobbyRooms[g_LobbyNumRooms];
		const char *v;
		char state[16];

		memset(r, 0, sizeof(*r));
		jsonReadString(jsonGet(p, "id"), r->id, sizeof(r->id));
		jsonReadString(jsonGet(p, "name"), r->name, sizeof(r->name));
		jsonReadString(jsonGet(p, "arena"), r->arena, sizeof(r->arena));
		jsonReadString(jsonGet(p, "scenarioName"), r->scenario, sizeof(r->scenario));
		lobbySanitize(r->name);
		lobbySanitize(r->arena);
		lobbySanitize(r->scenario);
		r->bots = jsonReadInt(jsonGet(p, "bots"), 0);
		r->locked = jsonReadInt(jsonGet(p, "locked"), 0);
		r->reserved = jsonReadInt(jsonGet(p, "reservedSlots"), 0);
		v = jsonGet(p, "timeLeft");
		r->timeleft = v && !jsonIsNull(v) ? jsonReadInt(v, -1) : -1;
		jsonReadString(jsonGet(p, "state"), state, sizeof(state));
		r->playing = strcmp(state, "playing") == 0;

		{
			char k2[8];
			const char *q = jsonFirst(jsonGet(p, "players"), k2, sizeof(k2));

			while (q && r->numslots < LOBBY_MAX_SLOTS) {
				if (!jsonIsNull(q)) {
					jsonReadString(q, r->players[r->numslots], sizeof(r->players[0]));
					lobbySanitize(r->players[r->numslots]);
					r->numplayers++;
				}
				r->numslots++;
				q = jsonNext(q, k2, sizeof(k2));
			}
		}

		v = jsonGet(p, "free");
		r->free = v ? jsonReadInt(v, 0) : r->numslots - r->numplayers;

		if (!r->playing) {
			r->free = 0;
		}

		if (r->id[0]) {
			g_LobbyNumRooms++;
		}

		p = jsonNext(p, key, sizeof(key));
	}
}

static void lobbyEnter(const char *roomid, const char *password)
{
	char name[16];

	lobbyAgentName(name, sizeof(name));
	g_LobbyState = LOBBY_ENTERING;
	lobbySetStatus("Joining the match...");
	lobbyPlatformEnterMatch(g_LobbyServer, roomid, password ? password : "", name);
}

static void lobbyOnMessage(const char *msg)
{
	char type[24];

	jsonReadString(jsonGet(msg, "type"), type, sizeof(type));

	if (strcmp(type, "welcome") == 0) {
		lobbyOnWelcome(msg);
	} else if (strcmp(type, "rooms") == 0) {
		lobbyOnRooms(msg);
	} else if (strcmp(type, "created") == 0) {
		char roomid[16];
		jsonReadString(jsonGet(msg, "roomId"), roomid, sizeof(roomid));
		g_LobbyCreating = false;

		if (roomid[0]) {
			lobbyEnter(roomid, g_LobbyPendingPassword);
		}
	} else if (strcmp(type, "error") == 0) {
		char code[16], text[96];

		jsonReadString(jsonGet(msg, "code"), code, sizeof(code));
		jsonReadString(jsonGet(msg, "message"), text, sizeof(text));
		lobbySanitize(text);
		g_LobbyCreating = false;

		if (strcmp(code, "build") == 0) {
			g_LobbyState = LOBBY_FAILED;
			lobbySetStatus("That server runs a different version of the game");
		} else {
			lobbySetStatus(text[0] ? text : "The server refused that");
		}
	}
}

/* ------------------------------------------------------------------------ */

const char *lobbyGetDefaultServer(void)
{
	static char def[LOBBY_SERVER_LEN + 1];
	lobbyPlatformDefaultServer(def, sizeof(def));
	return def;
}

const char *lobbyGetServer(void)
{
	return g_LobbyServerCfg;
}

const char *lobbyGetServerDisplay(void)
{
	static char display[LOBBY_SERVER_LEN + 1];
	const char *s = g_LobbyServerCfg[0] ? g_LobbyServerCfg : lobbyGetDefaultServer();

	// hide the scheme and path
	const char *scheme = strstr(s, "://");
	snprintf(display, sizeof(display), "%s", scheme ? scheme + 3 : s);

	char *slash = strchr(display, '/');

	if (slash) {
		*slash = '\0';
	}

	return display;
}

void lobbyConnect(const char *server)
{
	lobbyDisconnect();
	snprintf(g_LobbyServer, sizeof(g_LobbyServer), "%s", server && server[0] ? server : lobbyGetDefaultServer());

	g_LobbyNumRooms = 0;
	g_LobbyCreating = false;
	g_LobbyTransportWasOpen = false;

	if (!g_LobbyServer[0] || !lobbyPlatformOpen(g_LobbyServer)) {
		g_LobbyState = LOBBY_FAILED;
#ifdef PLATFORM_WEB
		lobbySetStatus("Online play isn't available");
#else
		lobbySetStatus("Online play isn't available on this platform yet");
#endif
		return;
	}

	g_LobbyState = LOBBY_CONNECTING;
	snprintf(g_LobbyStatus, sizeof(g_LobbyStatus), "Connecting to %s...", lobbyGetServerDisplay());
}

void lobbyDisconnect(void)
{
	if (g_LobbyState != LOBBY_OFFLINE) {
		lobbyPlatformClose();
	}

	g_LobbyState = LOBBY_OFFLINE;
	g_LobbyNumRooms = 0;
}

void lobbyTick(void)
{
	s32 transport;

	if (g_LobbyState == LOBBY_OFFLINE || g_LobbyState == LOBBY_ENTERING) {
		return;
	}

	transport = lobbyPlatformState();

	if (transport == TRANSPORT_OPEN && !g_LobbyTransportWasOpen) {
		lobbySendHello();
	}

	g_LobbyTransportWasOpen = transport == TRANSPORT_OPEN;

	while (lobbyPlatformRecv(g_LobbyMsg, sizeof(g_LobbyMsg)) > 0) {
		lobbyOnMessage(g_LobbyMsg);

		if (g_LobbyState == LOBBY_ENTERING) {
			return;
		}
	}

	if (transport == TRANSPORT_CLOSED && g_LobbyState != LOBBY_FAILED) {
		// the page's transport keeps retrying; show why there's no list meanwhile
		g_LobbyNumRooms = 0;
		g_LobbyState = LOBBY_CONNECTING;
		snprintf(g_LobbyStatus, sizeof(g_LobbyStatus), "Can't reach %s, retrying...", lobbyGetServerDisplay());
	}
}

s32 lobbyGetState(void)
{
	return g_LobbyState;
}

const char *lobbyGetStatus(void)
{
	return g_LobbyStatus;
}

s32 lobbyGetNumRooms(void)
{
	return g_LobbyNumRooms;
}

const struct lobbyroom *lobbyGetRoom(s32 index)
{
	return index >= 0 && index < g_LobbyNumRooms ? &g_LobbyRooms[index] : NULL;
}

const struct lobbyroom *lobbyFindRoom(const char *id)
{
	for (s32 i = 0; i < g_LobbyNumRooms; i++) {
		if (strcmp(g_LobbyRooms[i].id, id) == 0) {
			return &g_LobbyRooms[i];
		}
	}

	return NULL;
}

s32 lobbyGetNumArenas(void) { return g_LobbyNumArenas; }
const char *lobbyGetArenaName(s32 index) { return index >= 0 && index < g_LobbyNumArenas ? g_LobbyArenaNames[index] : ""; }
s32 lobbyGetArenaId(s32 index) { return index >= 0 && index < g_LobbyNumArenas ? g_LobbyArenaIds[index] : -1; }
s32 lobbyGetNumScenarios(void) { return g_LobbyNumScenarios; }
const char *lobbyGetScenarioName(s32 index) { return index >= 0 && index < g_LobbyNumScenarios ? g_LobbyScenarioNames[index] : ""; }
s32 lobbyGetNumWeaponSets(void) { return g_LobbyNumWeaponSets; }
const char *lobbyGetWeaponSetName(s32 index) { return index >= 0 && index < g_LobbyNumWeaponSets ? g_LobbyWeaponSetNames[index] : ""; }

void lobbyCreate(const struct lobbycreate *s)
{
	char ename[80], epass[80], stage[16];
	char msg[512];

	if (g_LobbyState != LOBBY_ONLINE || g_LobbyCreating) {
		return;
	}

	jsonWriteString(ename, sizeof(ename), s->name);
	jsonWriteString(epass, sizeof(epass), s->password);

	if (s->stage >= 0) {
		snprintf(stage, sizeof(stage), "%d", s->stage);
	} else {
		snprintf(stage, sizeof(stage), "\"\"");
	}

	snprintf(msg, sizeof(msg),
			"{\"type\":\"create\",\"join\":false,\"settings\":{\"name\":%s,\"stage\":%s,\"scenario\":%d,"
			"\"timelimit\":%d,\"scorelimit\":%d,\"bots\":%d,\"botDifficulty\":%d,\"weaponset\":%d,"
			"\"teams\":%s,\"password\":%s}}",
			ename, stage, s->scenario, s->timelimit, s->scorelimit, s->bots, s->botskill, s->weaponset,
			s->teams ? "true" : "false", epass);

	snprintf(g_LobbyPendingPassword, sizeof(g_LobbyPendingPassword), "%s", s->password);
	g_LobbyCreating = true;
	lobbySetStatus("Starting the match...");
	lobbyPlatformSend(msg);
}

void lobbyJoin(const char *roomid, const char *password)
{
	if (g_LobbyState != LOBBY_ONLINE) {
		return;
	}

	lobbyEnter(roomid, password);
}

s32 lobbyShouldReturnToMenu(void)
{
	return lobbyPlatformTakeReturnFlag();
}
