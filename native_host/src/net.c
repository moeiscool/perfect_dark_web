// Online play for native hosts: the lobby transport of the game's Online menu (port/src/lobby.c)
// and the match client, a C port of the match half of web/net-client.js.
//
// Like the browser, the host can't switch a running game into a match: picking a match restarts
// the game instance (see hostRestart in main.c) with the match config from the server, and
// leaving restarts it normally with the Online menu open. --replay feeds recorded tick packets
// instead (testing determinism against the browser build).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "host.h"
#include "json.h"
#include "ws.h"
#include "pdbuild.h"

#define INPUT_SIZE 28
#define TICK_PACKET_SIZE (8 + 4 * INPUT_SIZE)
#define MSG_TICK 1
#define MSG_SNAPSHOT 2
#define MSG_INPUT 3
#define MSG_SNAPSHOT_REPLY 4
#define HASH_REPORT_TICKS 120
#define QUEUE_AHEAD 600
#define SNAP_MAGIC 0x31535044u

static char g_ClientId[32];

/* ------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------ */

static void loadClientId(void)
{
	char path[1024];
	snprintf(path, sizeof(path), "%s/clientid.txt", g_HostOpts.saveDir);
	FILE *f = fopen(path, "r");
	if (f) {
		if (fgets(g_ClientId, sizeof(g_ClientId), f)) {
			g_ClientId[strcspn(g_ClientId, "\r\n")] = '\0';
		}
		fclose(f);
	}
	if (strlen(g_ClientId) < 16) {
		srand((unsigned)(platNowUs()));
		for (int i = 0; i < 24; i++) {
			g_ClientId[i] = "0123456789abcdef"[rand() & 15];
		}
		g_ClientId[24] = '\0';
		f = fopen(path, "w");
		if (f) {
			fprintf(f, "%s\n", g_ClientId);
			fclose(f);
		}
	}
}

static const char *clientId(void)
{
	if (!g_ClientId[0]) {
		loadClientId();
	}
	return g_ClientId;
}

// "host", "host:port", or a ws:// / wss:// URL -> WebSocket URL of the lobby (/net)
static void lobbyUrl(const char *server, char *out, size_t size)
{
	if (!server || !*server) {
		server = PDHOST_DEFAULT_LOBBY;
	}
	if (!strncmp(server, "ws://", 5) || !strncmp(server, "wss://", 6)) {
		const size_t n = strlen(server);
		snprintf(out, size, "%s%s", server, (n > 4 && !strcmp(server + n - 4, "/net")) ? "" : "/net");
		return;
	}
	char host[256];
	snprintf(host, sizeof(host), "%s", server);
	host[strcspn(host, "/")] = '\0';
	const char *colon = strrchr(host, ':');
	// a host name alone is served over HTTPS (eg. through Cloudflare); servers on other ports
	// (a LAN server's HTTP port) are plain, except the usual HTTPS ones
	int secure = 1;
	if (colon) {
		const int port = atoi(colon + 1);
		secure = port == 443 || port == 8443 || port == 9443;
	}
	snprintf(out, size, "%s://%s/net", secure ? "wss" : "ws", host);
}

static void sendHello(struct wsconn *c, const char *name)
{
	char ename[64], eclient[64], msg[256];
	jsonQuote(ename, sizeof(ename), name && *name ? name : "Agent");
	jsonQuote(eclient, sizeof(eclient), clientId());
	snprintf(msg, sizeof(msg), "{\"type\":\"hello\",\"name\":%s,\"clientId\":%s,\"build\":\"%s\",\"platform\":\"%s\"}",
		ename, eclient, PDHOST_BUILD_ID, PDHOST_PLATFORM_NAME);
	wsSendText(c, msg);
}

/* ------------------------------------------------------------------------
 * lobby transport for the game's Online menu
 * ------------------------------------------------------------------------ */

static struct wsconn *g_Lobby;
static char g_LobbyServer[256];
static uint32_t g_LobbyRetryAt;
static int g_ReturnToMenu;

u32 w2c_env_lobbyJsOpen(struct w2c_env *e, u32 server)
{
	char url[512];
	wsClose(g_Lobby);
	snprintf(g_LobbyServer, sizeof(g_LobbyServer), "%s", (const char *)wptr(server));
	lobbyUrl(g_LobbyServer, url, sizeof(url));
	g_Lobby = wsOpen(url);
	hostLog("lobby: connecting to %s", url);
	return 1;
}

void w2c_env_lobbyJsClose(struct w2c_env *e)
{
	wsClose(g_Lobby);
	g_Lobby = NULL;
	g_LobbyServer[0] = '\0';
}

u32 w2c_env_lobbyJsState(struct w2c_env *e)
{
	if (!g_Lobby) {
		return 0;
	}
	const int st = wsState(g_Lobby);
	if (st == WS_CLOSED && g_LobbyServer[0]) {
		// keep retrying, like the page does
		const uint32_t now = (uint32_t)(platNowUs() / 1000);
		if (!g_LobbyRetryAt) {
			hostLog("lobby: %s", wsError(g_Lobby));
			g_LobbyRetryAt = now + 3000;
		} else if ((int32_t)(now - g_LobbyRetryAt) >= 0) {
			char url[512];
			g_LobbyRetryAt = 0;
			lobbyUrl(g_LobbyServer, url, sizeof(url));
			wsClose(g_Lobby);
			g_Lobby = wsOpen(url);
			return 1;
		}
	}
	return (u32)st;
}

void w2c_env_lobbyJsSend(struct w2c_env *e, u32 text)
{
	wsSendText(g_Lobby, (const char *)wptr(text));
}

u32 w2c_env_lobbyJsRecv(struct w2c_env *e, u32 buf, u32 size)
{
	int binary;
	const uint8_t *data;
	size_t len;
	while (wsRecv(g_Lobby, &binary, &data, &len)) {
		if (!binary && len < size) {
			memcpy(wmem() + buf, data, len + 1);
			return 1;
		}
	}
	return 0;
}

void w2c_env_lobbyJsGetString(struct w2c_env *e, u32 which, u32 buf, u32 size)
{
	const char *text = which == 0 ? PDHOST_DEFAULT_LOBBY : which == 1 ? PDHOST_BUILD_ID : which == 2 ? clientId() : "";
	if (size > 0) {
		snprintf((char *)wptr(buf), size, "%s", text);
	}
}

u32 w2c_env_lobbyJsTakeReturnFlag(struct w2c_env *e)
{
	const int r = g_ReturnToMenu;
	g_ReturnToMenu = 0;
	return (u32)r;
}

/* ------------------------------------------------------------------------
 * the match
 * ------------------------------------------------------------------------ */

struct name {
	uint32_t tick;
	int slot;
	char name[16];
};

static struct {
	int active;            // this game instance is an online match
	struct wsconn *ws;
	char server[256];
	char roomId[32];
	char password[64];
	char name[16];
	char token[64];
	int slot;
	int live;
	int fresh;
	int awaitingSnapshot;
	int64_t lastTick;      // -1: none yet
	uint8_t *buffer;       // tick packets that arrived while waiting for a snapshot
	size_t bufferLen, bufferCap;
	uint8_t *backlog;      // tick packets waiting for room in the game's queue
	size_t backlogLen, backlogCap;
	struct name names[64];
	int numNames;
	int ended;
	int overSent;
	double endedAt;
	double reconnectAt;
	uint8_t lastInput[INPUT_SIZE];
	double lastInputAt;
	double leaveHeldSince;
} g_Match;

// the match the menu picked, joined after the restart
static struct {
	int pending;
	char server[256];
	char roomId[32];
	char password[64];
	char name[16];
} g_Join;

static void append(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
	if (*len + n > *cap) {
		*cap = (*len + n) * 2 + 4096;
		*buf = realloc(*buf, *cap);
	}
	memcpy(*buf + *len, data, n);
	*len += n;
}

static void queueNamesUpTo(uint32_t tick)
{
	for (int i = 0; i < g_Match.numNames;) {
		if (g_Match.names[i].tick <= tick) {
			const uint32_t buf = w2c_pd_netGetNameBuffer(&g_pd);
			memset(wmem() + buf, 0, 32);
			memcpy(wmem() + buf, g_Match.names[i].name, strnlen(g_Match.names[i].name, 12));
			w2c_pd_netQueueName(&g_pd, g_Match.names[i].tick, (u32)g_Match.names[i].slot);
			g_Match.names[i] = g_Match.names[--g_Match.numNames];
		} else {
			i++;
		}
	}
}

static void pushPacket(const uint8_t *packet)
{
	uint32_t tick, events;
	memcpy(&tick, packet, 4);
	memcpy(&events, packet + 4, 4);
	queueNamesUpTo(tick);
	const uint32_t buf = w2c_pd_netGetPushBuffer(&g_pd);
	memcpy(wmem() + buf, packet + 8, 4 * INPUT_SIZE);
	w2c_pd_netPushTick(&g_pd, tick, events);
}

static void drainBacklog(void)
{
	size_t used = 0;
	while (used + TICK_PACKET_SIZE <= g_Match.backlogLen && w2c_pd_netGetQueuedTicks(&g_pd) < QUEUE_AHEAD) {
		pushPacket(g_Match.backlog + used);
		used += TICK_PACKET_SIZE;
	}
	memmove(g_Match.backlog, g_Match.backlog + used, g_Match.backlogLen - used);
	g_Match.backlogLen -= used;
}

static void takeTick(const uint8_t *packet)
{
	uint32_t tick;
	memcpy(&tick, packet, 4);
	if (g_Match.lastTick >= 0 && (int64_t)tick <= g_Match.lastTick) {
		return; // part of the snapshot already
	}
	g_Match.lastTick = tick;
	append(&g_Match.backlog, &g_Match.backlogLen, &g_Match.backlogCap, packet, TICK_PACKET_SIZE);
	drainBacklog();
}

static void goLive(void)
{
	g_Match.awaitingSnapshot = 0;
	g_Match.live = 1;
	for (size_t i = 0; i + TICK_PACKET_SIZE <= g_Match.bufferLen; i += TICK_PACKET_SIZE) {
		takeTick(g_Match.buffer + i);
	}
	g_Match.bufferLen = 0;
}

/* ---- snapshots (web/net/nethost.js makeSnapshot / restoreSnapshot) ---- */

static int inflateRaw(const uint8_t *in, size_t inLen, uint8_t **out, size_t *outLen)
{
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	if (inflateInit2(&zs, -15) != Z_OK) {
		return -1;
	}
	size_t cap = inLen * 20 + 65536;
	uint8_t *buf = malloc(cap);
	zs.next_in = (Bytef *)in;
	zs.avail_in = (uInt)inLen;
	int r;
	do {
		if (zs.total_out == cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		zs.next_out = buf + zs.total_out;
		zs.avail_out = (uInt)(cap - zs.total_out);
		r = inflate(&zs, Z_NO_FLUSH);
	} while (r == Z_OK);
	inflateEnd(&zs);
	if (r != Z_STREAM_END) {
		free(buf);
		return -1;
	}
	*out = buf;
	*outLen = zs.total_out;
	return 0;
}

static void applySnapshot(const uint8_t *packed, size_t len)
{
	uint8_t *raw;
	size_t rawLen;
	if (inflateRaw(packed, len, &raw, &rawLen) != 0 || rawLen < 16) {
		hostLog("net: bad snapshot");
		return;
	}
	uint32_t magic, tick, highWater, count;
	memcpy(&magic, raw, 4);
	memcpy(&tick, raw + 4, 4);
	memcpy(&highWater, raw + 8, 4);
	memcpy(&count, raw + 12, 4);
	if (magic != SNAP_MAGIC) {
		free(raw);
		hostLog("net: not a game snapshot");
		return;
	}

	w2c_pd_netClearQueue(&g_pd);
	g_Match.backlogLen = 0;
	size_t pos = 16 + (size_t)count * 8;
	for (uint32_t i = 0; i < count && pos <= rawLen; i++) {
		uint32_t start, n;
		memcpy(&start, raw + 16 + i * 8, 4);
		memcpy(&n, raw + 20 + i * 8, 4);
		if (pos + n > rawLen || (uint64_t)start + n > wmemSize()) {
			break;
		}
		memcpy(wmem() + start, raw + pos, n);
		pos += n;
	}
	free(raw);
	w2c_pd_netAfterRestore(&g_pd, tick, highWater);
	g_Match.lastTick = tick == 0xffffffffu ? -1 : (int64_t)tick;
	hostLog("net: in the match at tick %lld", (long long)g_Match.lastTick);
	goLive();
}

static void answerSnapshotRequest(uint32_t reqId)
{
	if (!g_Match.live || g_Match.awaitingSnapshot) {
		char msg[96];
		snprintf(msg, sizeof(msg), "{\"type\":\"snapshot-failed\",\"reqId\":%u}", reqId);
		wsSendText(g_Match.ws, msg);
		return;
	}

	// the game's ranges (pd.snap.json) plus the arena, minus the ROM image inside it
	const uint32_t infoPtr = w2c_pd_netGetSnapshotInfo(&g_pd);
	uint32_t info[5];
	memcpy(info, wmem() + infoPtr, sizeof(info));
	const uint32_t arenaBase = info[0], highWater = info[1], romStart = info[2], romSize = info[3], tick = info[4];
	uint32_t ranges[PDHOST_SNAP_RANGE_COUNT + 2][2];
	uint32_t count = 0;
	for (int i = 0; i < PDHOST_SNAP_RANGE_COUNT; i++) {
		if (g_SnapRanges[i][1]) {
			ranges[count][0] = g_SnapRanges[i][0];
			ranges[count++][1] = g_SnapRanges[i][1];
		}
	}
	const uint32_t arenaEnd = arenaBase + highWater;
	if (romStart >= arenaBase && romStart + romSize <= arenaEnd) {
		if (romStart > arenaBase) {
			ranges[count][0] = arenaBase;
			ranges[count++][1] = romStart - arenaBase;
		}
		if (arenaEnd > romStart + romSize) {
			ranges[count][0] = romStart + romSize;
			ranges[count++][1] = arenaEnd - (romStart + romSize);
		}
	} else if (highWater) {
		ranges[count][0] = arenaBase;
		ranges[count++][1] = highWater;
	}

	size_t size = 16 + (size_t)count * 8;
	for (uint32_t i = 0; i < count; i++) {
		size += ranges[i][1];
	}
	uint8_t *raw = malloc(size);
	const uint32_t hdr[4] = { SNAP_MAGIC, tick, highWater, count };
	memcpy(raw, hdr, 16);
	size_t pos = 16 + (size_t)count * 8;
	for (uint32_t i = 0; i < count; i++) {
		memcpy(raw + 16 + i * 8, ranges[i], 8);
		memcpy(raw + pos, wmem() + ranges[i][0], ranges[i][1]);
		pos += ranges[i][1];
	}

	// [4][u32 request][u32 tick][deflate-raw snapshot]
	uLongf bound = compressBound((uLong)size) + 64;
	uint8_t *msg = malloc(9 + bound);
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
	zs.next_in = raw;
	zs.avail_in = (uInt)size;
	zs.next_out = msg + 9;
	zs.avail_out = (uInt)bound;
	deflate(&zs, Z_FINISH);
	const size_t packed = zs.total_out;
	deflateEnd(&zs);
	free(raw);

	msg[0] = MSG_SNAPSHOT_REPLY;
	memcpy(msg + 1, &reqId, 4);
	memcpy(msg + 5, &tick, 4);
	wsSendBinary(g_Match.ws, msg, 9 + packed);
	free(msg);
}

static void sendResults(void)
{
	const uint32_t count = w2c_pd_netGetResults(&g_pd);
	const uint32_t ptr = w2c_pd_netGetResultsBuffer(&g_pd);
	char *msg = malloc(4096);
	size_t len = (size_t)snprintf(msg, 4096, "{\"type\":\"over\",\"tick\":%lld,\"results\":[", (long long)g_Match.lastTick);
	for (uint32_t i = 0; i < count && len < 3800; i++) {
		const uint8_t *r = wmem() + ptr + i * 36;
		char name[17], ename[40];
		int32_t slot, kills, suicides, deaths;
		memcpy(name, r, 16);
		name[16] = '\0';
		memcpy(&slot, r + 16, 4);
		memcpy(&kills, r + 20, 4);
		memcpy(&suicides, r + 24, 4);
		memcpy(&deaths, r + 28, 4);
		jsonQuote(ename, sizeof(ename), name);
		len += (size_t)snprintf(msg + len, 4096 - len, "%s{\"name\":%s,\"bot\":%s,\"kills\":%d,\"suicides\":%d,\"deaths\":%d}",
			i ? "," : "", ename, slot < 0 ? "true" : "false", kills, suicides, deaths);
	}
	snprintf(msg + len, 4096 - len, "]}");
	wsSendText(g_Match.ws, msg);
	free(msg);
}

/* ---- messages ---- */

static void onJson(const char *msg)
{
	char type[32];
	jsonString(jsonGet(msg, "type"), type, sizeof(type));

	if (!strcmp(type, "joined")) {
		// back in after a reconnect: a snapshot follows
		jsonString(jsonGet(msg, "token"), g_Match.token, sizeof(g_Match.token));
		g_Match.live = 0;
		g_Match.awaitingSnapshot = 1;
		g_Match.bufferLen = 0;
		hostLog("net: rejoined, waiting for the match state");
	} else if (!strcmp(type, "name")) {
		if (g_Match.numNames < (int)(sizeof(g_Match.names) / sizeof(*g_Match.names))) {
			struct name *n = &g_Match.names[g_Match.numNames++];
			n->tick = (uint32_t)jsonInt(jsonGet(msg, "tick"), 0);
			n->slot = (int)jsonInt(jsonGet(msg, "slot"), 0);
			jsonString(jsonGet(msg, "name"), n->name, sizeof(n->name));
		}
	} else if (!strcmp(type, "resync")) {
		g_Match.live = 0;
		g_Match.awaitingSnapshot = 1;
		g_Match.bufferLen = 0;
		hostLog("net: resynchronizing");
	} else if (!strcmp(type, "snapshot-request")) {
		answerSnapshotRequest((uint32_t)jsonInt(jsonGet(msg, "reqId"), 0));
	} else if (!strcmp(type, "ended")) {
		g_Match.ended = 1;
		g_Match.live = 0;
		g_Match.endedAt = platNowUs();
		hostLog("net: match over");
		for (const char *r = jsonFirst(jsonGet(msg, "results")); r; r = jsonNext(r)) {
			char name[32];
			jsonString(jsonGet(r, "name"), name, sizeof(name));
			hostLog("  %-16s kills %lld deaths %lld suicides %lld", name, (long long)jsonInt(jsonGet(r, "kills"), 0),
				(long long)jsonInt(jsonGet(r, "deaths"), 0), (long long)jsonInt(jsonGet(r, "suicides"), 0));
		}
	} else if (!strcmp(type, "notice")) {
		char text[128];
		jsonString(jsonGet(msg, "text"), text, sizeof(text));
		hostLog("net: %s", text);
	} else if (!strcmp(type, "error")) {
		char text[160];
		jsonString(jsonGet(msg, "message"), text, sizeof(text));
		hostLog("net: server says: %s", text);
	}
}

static void pumpMatch(void)
{
	int binary;
	const uint8_t *data;
	size_t len;

	if (wsState(g_Match.ws) == WS_CLOSED && !g_Match.ended) {
		// reconnect and rejoin with our token (the server keeps the slot for a minute)
		const double now = platNowUs();
		if (g_Match.reconnectAt == 0) {
			hostLog("net: connection lost, reconnecting");
			g_Match.reconnectAt = now + 2e6;
			g_Match.live = 0;
		} else if (now >= g_Match.reconnectAt) {
			char url[512], msg[256], ep[96];
			g_Match.reconnectAt = 0;
			lobbyUrl(g_Match.server, url, sizeof(url));
			wsClose(g_Match.ws);
			g_Match.ws = wsOpen(url);
			while (wsState(g_Match.ws) == WS_CONNECTING) {
				platSleepMs(10);
			}
			if (wsState(g_Match.ws) == WS_OPEN) {
				sendHello(g_Match.ws, g_Match.name);
				jsonQuote(ep, sizeof(ep), g_Match.token);
				snprintf(msg, sizeof(msg), "{\"type\":\"join\",\"roomId\":\"%s\",\"token\":%s}", g_Match.roomId, ep);
				wsSendText(g_Match.ws, msg);
			}
		}
		return;
	}

	while (wsRecv(g_Match.ws, &binary, &data, &len)) {
		if (!binary) {
			onJson((const char *)data);
		} else if (len >= 1 + TICK_PACKET_SIZE && data[0] == MSG_TICK) {
			if (g_Match.awaitingSnapshot) {
				append(&g_Match.buffer, &g_Match.bufferLen, &g_Match.bufferCap, data + 1, TICK_PACKET_SIZE);
			} else {
				takeTick(data + 1);
			}
		} else if (len >= 5 && data[0] == MSG_SNAPSHOT) {
			applySnapshot(data + 5, len - 5);
		}
	}
	drainBacklog();
}

/* ------------------------------------------------------------------------
 * starting and leaving a match (called by main.c around game instances)
 * ------------------------------------------------------------------------ */

// a match picked in the Online menu: connect, join and write its config; returns the game
// arguments' slot (and the config path) or -1 to go back to the menu
int netPrepareLaunch(char *cfgGamePath, size_t size)
{
	if (!g_Join.pending) {
		return -1;
	}
	g_Join.pending = 0;

	char url[512], msg[512], epw[96];
	lobbyUrl(g_Join.server, url, sizeof(url));
	hostLog("net: joining match %s on %s", g_Join.roomId, url);

	memset(&g_Match, 0, sizeof(g_Match));
	g_Match.lastTick = -1;
	snprintf(g_Match.server, sizeof(g_Match.server), "%s", g_Join.server);
	snprintf(g_Match.roomId, sizeof(g_Match.roomId), "%s", g_Join.roomId);
	snprintf(g_Match.name, sizeof(g_Match.name), "%s", g_Join.name);

	g_Match.ws = wsOpen(url);
	while (wsState(g_Match.ws) == WS_CONNECTING) {
		platSleepMs(10);
	}
	if (wsState(g_Match.ws) != WS_OPEN) {
		hostLog("net: %s", wsError(g_Match.ws));
		goto fail;
	}

	sendHello(g_Match.ws, g_Join.name);
	jsonQuote(epw, sizeof(epw), g_Join.password);
	snprintf(msg, sizeof(msg), "{\"type\":\"join\",\"roomId\":\"%s\",\"password\":%s}", g_Join.roomId, epw);
	wsSendText(g_Match.ws, msg);

	// wait for "joined" (ticks for us may follow it; they stay in the socket until the game runs)
	const double deadline = platNowUs() + 15e6;
	while (platNowUs() < deadline && wsState(g_Match.ws) == WS_OPEN) {
		int binary;
		const uint8_t *data;
		size_t len;
		if (!wsRecv(g_Match.ws, &binary, &data, &len)) {
			platSleepMs(5);
			continue;
		}
		if (binary) {
			continue;
		}
		const char *m = (const char *)data;
		char type[32];
		jsonString(jsonGet(m, "type"), type, sizeof(type));
		if (!strcmp(type, "error")) {
			char text[160];
			jsonString(jsonGet(m, "message"), text, sizeof(text));
			hostLog("net: %s", text);
			goto fail;
		}
		if (!strcmp(type, "name")) {
			onJson(m);
			continue;
		}
		if (strcmp(type, "joined") != 0) {
			continue;
		}

		g_Match.slot = (int)jsonInt(jsonGet(m, "slot"), 0);
		g_Match.fresh = jsonBool(jsonGet(m, "fresh"));
		jsonString(jsonGet(m, "token"), g_Match.token, sizeof(g_Match.token));
		g_Match.awaitingSnapshot = !g_Match.fresh;
		g_Match.live = g_Match.fresh;

		// the match config (web/net/nethost.js matchConfigText)
		const char *cfg = jsonGet(m, "match");
		char hostDir[1024], hostFile[1100];
		snprintf(hostDir, sizeof(hostDir), "%s/net", g_HostOpts.tmpDir);
		hostMkdirs(hostDir);
		snprintf(hostFile, sizeof(hostFile), "%s/match.cfg", hostDir);
		FILE *f = fopen(hostFile, "w");
		if (!f) {
			hostLog("net: can't write %s", hostFile);
			goto fail;
		}
		static const char *keys[] = { "seed", "stage", "scenario", "timelimit", "scorelimit", "teamscorelimit", "options", "weaponset", "occupied" };
		for (size_t k = 0; k < sizeof(keys) / sizeof(*keys); k++) {
			const char *v = jsonGet(cfg, keys[k]);
			if (v && !jsonIsNull(v)) {
				fprintf(f, "%s=%lld\n", keys[k], (long long)jsonInt(v, 0));
			}
		}
		int i = 0;
		for (const char *s = jsonFirst(jsonGet(cfg, "slots")); s; s = jsonNext(s), i++) {
			if (jsonIsNull(s)) {
				continue;
			}
			char name[32];
			if (jsonString(jsonGet(s, "name"), name, sizeof(name))) {
				name[strcspn(name, "\r\n=")] = '\0';
				name[12] = '\0';
				fprintf(f, "slot%d.name=%s\n", i, name);
			}
			static const char *skeys[] = { "body", "head", "team" };
			for (size_t k = 0; k < 3; k++) {
				const char *v = jsonGet(s, skeys[k]);
				if (v && !jsonIsNull(v)) {
					fprintf(f, "slot%d.%s=%lld\n", i, skeys[k], (long long)jsonInt(v, 0));
				}
			}
			fprintf(f, "slot%d.occupied=%d\n", i, jsonBool(jsonGet(s, "occupied")) ? 1 : 0);
		}
		int nbots = 0;
		for (const char *b = jsonFirst(jsonGet(cfg, "bots")); b; b = jsonNext(b)) {
			nbots++;
		}
		fprintf(f, "bots=%d\n", nbots);
		i = 0;
		for (const char *b = jsonFirst(jsonGet(cfg, "bots")); b; b = jsonNext(b), i++) {
			static const char *bkeys[] = { "type", "difficulty", "team" };
			for (size_t k = 0; k < 3; k++) {
				const char *v = jsonGet(b, bkeys[k]);
				if (v && !jsonIsNull(v)) {
					fprintf(f, "bot%d.%s=%lld\n", i, bkeys[k], (long long)jsonInt(v, 0));
				}
			}
		}
		fclose(f);

		snprintf(cfgGamePath, size, "/tmp/net/match.cfg");
		g_Match.active = 1;
		hostLog("net: joined slot %d%s", g_Match.slot, g_Match.fresh ? " (starting the match)" : "");
		return g_Match.slot;
	}
	hostLog("net: no answer from the server");

fail:
	wsClose(g_Match.ws);
	g_Match.ws = NULL;
	g_ReturnToMenu = 1;
	return -1;
}

void netRequestJoin(const char *server, const char *room, const char *password, const char *name)
{
	snprintf(g_Join.server, sizeof(g_Join.server), "%s", server ? server : "");
	snprintf(g_Join.roomId, sizeof(g_Join.roomId), "%s", room);
	snprintf(g_Join.password, sizeof(g_Join.password), "%s", password ? password : "");
	snprintf(g_Join.name, sizeof(g_Join.name), "%s", name && *name ? name : "Agent");
	g_Join.pending = 1;
}

// the menu picked a match: restart the game into it
void w2c_env_lobbyJsEnterMatch(struct w2c_env *e, u32 server, u32 room, u32 password, u32 name)
{
	snprintf(g_Join.server, sizeof(g_Join.server), "%s", (const char *)wptr(server));
	snprintf(g_Join.roomId, sizeof(g_Join.roomId), "%s", (const char *)wptr(room));
	snprintf(g_Join.password, sizeof(g_Join.password), "%s", (const char *)wptr(password));
	snprintf(g_Join.name, sizeof(g_Join.name), "%s", (const char *)wptr(name));
	g_Join.pending = 1;
	wsClose(g_Lobby);
	g_Lobby = NULL;
	g_LobbyServer[0] = '\0';
	w2c_pd_pdWebSaveConfig(&g_pd);
	hostRestart();
}

static void leaveMatch(int explicit)
{
	if (explicit) {
		wsSendText(g_Match.ws, "{\"type\":\"leave\"}");
	}
	wsClose(g_Match.ws);
	g_Match.ws = NULL;
	g_Match.active = 0;
	free(g_Match.buffer);
	free(g_Match.backlog);
	memset(&g_Match, 0, sizeof(g_Match));
	g_ReturnToMenu = 1;
	hostRestart();
}

// leaving: hold Back and Start on a controller (or F10) for a second and a half
static void checkLeave(void)
{
	int held = platKeyHeld(67 /* F10 */);
	for (int i = 0; i < 8 && !held; i++) {
		held = platPadButton(i, 4 /* back */) && platPadButton(i, 6 /* start */);
	}
	const double now = platNowUs();
	if (!held) {
		g_Match.leaveHeldSince = 0;
	} else if (!g_Match.leaveHeldSince) {
		g_Match.leaveHeldSince = now;
	} else if (now - g_Match.leaveHeldSince > 1.5e6) {
		hostLog("net: leaving the match");
		leaveMatch(1);
	}
}

/* ------------------------------------------------------------------------
 * imports used during a match
 * ------------------------------------------------------------------------ */

static FILE *replayFile;
static uint32_t replayTicks;

static void replayNextTick(void)
{
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
	pushPacket(packet);
	replayTicks++;
}

// the game waits here whenever it has no tick to run
void w2c_env_0x5F_asyncjs_0x5FnetJsWaitForTick(struct w2c_env *e)
{
	if (g_HostOpts.replay) {
		replayNextTick();
		return;
	}
	if (!g_Match.active) {
		platSleepMs(1);
		return;
	}

	for (;;) {
		pumpMatch();
		checkLeave();

		if (g_Match.live && !g_Match.overSent && w2c_pd_netGetMatchOver(&g_pd)) {
			g_Match.overSent = 1;
			sendResults();
		}
		if (g_Match.ended && platNowUs() - g_Match.endedAt > 8e6) {
			leaveMatch(0);
		}
		if (w2c_pd_netGetQueuedTicks(&g_pd) > 0) {
			return;
		}
		platSleepMs(1);
	}
}

void w2c_env_pdhost_net_hash(struct w2c_env *e, u32 tick, u32 hash)
{
	if (g_HostOpts.replay) {
		printf("%u %08x\n", tick, hash);
	} else if (g_Match.active && g_Match.live && tick % HASH_REPORT_TICKS == 0) {
		char msg[96];
		snprintf(msg, sizeof(msg), "{\"type\":\"hash\",\"tick\":%u,\"hash\":%u}", tick, hash);
		wsSendText(g_Match.ws, msg);
	}
}

// the local player's input after each tick: sent when it changes, plus a heartbeat
void w2c_env_pdhost_net_local_input(struct w2c_env *e, u32 input)
{
	if (!g_Match.active || !g_Match.live) {
		return;
	}
	const uint8_t *in = wmem() + input;
	const double now = platNowUs();
	if (now - g_Match.lastInputAt < 1e6 && !memcmp(in, g_Match.lastInput, INPUT_SIZE)) {
		return;
	}
	memcpy(g_Match.lastInput, in, INPUT_SIZE);
	g_Match.lastInputAt = now;
	uint8_t msg[1 + INPUT_SIZE];
	msg[0] = MSG_INPUT;
	memcpy(msg + 1, in, INPUT_SIZE);
	wsSendBinary(g_Match.ws, msg, sizeof(msg));
}
