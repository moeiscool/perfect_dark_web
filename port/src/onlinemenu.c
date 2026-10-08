#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "bss.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "system.h"
#include "lobby.h"

/**
 * Online menu: lists the matches on the lobby server and creates or joins them. It's opened from
 * the Perfect Menu (src/game/mainmenu.c), so players on any platform with a lobby transport see
 * the same matches. The lobby protocol lives in port/src/lobby.c.
 */

#define ONLINE_ROOM_ROWS 8
#define ONLINE_FIRST_ROOM_ITEM 3

extern struct menudialogdef g_OnlineRoomMenuDialog;
extern struct menudialogdef g_OnlineCreateMenuDialog;
extern struct menudialogdef g_OnlineJoinPasswordMenuDialog;
extern struct menudialogdef g_OnlineMatchNameMenuDialog;
extern struct menudialogdef g_OnlineCreatePasswordMenuDialog;

static char g_OnlineRoomId[16];          // the match shown in the details dialog
static char g_OnlineJoinPassword[33];
static struct lobbycreate g_OnlineCreate = {
	"", "", -1, 0, 1, 10, 0, 4, 2, false,
};
static s32 g_OnlineCreateArena = 0;      // index into the arena dropdown (0 = random)
static char g_OnlineText[8][64];         // scratch for dynamic menu text

static char *onlineText(s32 slot, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vsnprintf(g_OnlineText[slot], sizeof(g_OnlineText[slot]), fmt, args);
	va_end(args);
	return g_OnlineText[slot];
}

static const struct lobbyroom *onlineSelectedRoom(void)
{
	return lobbyFindRoom(g_OnlineRoomId);
}

/* ------------------------------------------------------------------------
 * Online Matches (room list)
 * ------------------------------------------------------------------------ */

static char *onlineMenuTextStatus(struct menuitem *item)
{
	if (lobbyGetState() == LOBBY_ONLINE) {
		const s32 n = lobbyGetNumRooms();
		s32 open = 0;

		for (s32 i = 0; i < n; i++) {
			if (lobbyGetRoom(i)->free > 0) {
				open++;
			}
		}

		if (n == 0) {
			return onlineText(0, "No matches running. Create one!\n");
		}

		return onlineText(0, "%d match%s, %d with free slots\n", n, n == 1 ? "" : "es", open);
	}

	return onlineText(0, "%s\n", lobbyGetStatus());
}

static char *onlineMenuTextServer(struct menuitem *item)
{
	return onlineText(1, "Server: %s\n", lobbyGetServerDisplay());
}

static s32 onlineRoomIndex(struct menuitem *item);

static char *onlineMenuTextRoom(struct menuitem *item)
{
	const struct lobbyroom *room = lobbyGetRoom(onlineRoomIndex(item));

	if (!room) {
		return "";
	}

	return onlineText(2 + (onlineRoomIndex(item) & 1), "%s%.20s\n", room->locked ? "* " : "", room->name);
}

static char *onlineMenuTextRoomRight(struct menuitem *item)
{
	const struct lobbyroom *room = lobbyGetRoom(onlineRoomIndex(item));

	if (!room) {
		return "";
	}

	if (!room->playing) {
		return "Over\n";
	}

	if (room->free <= 0) {
		return "Full\n";
	}

	return onlineText(4 + (onlineRoomIndex(item) & 1), "%d/%d free\n", room->free, room->numslots);
}

static MenuItemHandlerResult onlineMenuHandlerRoom(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const struct lobbyroom *room = lobbyGetRoom(onlineRoomIndex(item));

	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return room == NULL;
	case MENUOP_SET:
		if (room) {
			snprintf(g_OnlineRoomId, sizeof(g_OnlineRoomId), "%s", room->id);
			g_OnlineJoinPassword[0] = '\0';
			menuPushDialog(&g_OnlineRoomMenuDialog);
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineMenuHandlerCreate(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKPREFOCUSED:
		// the list fills in after the dialog opens; start on Create rather than Refresh
		return true;
	case MENUOP_CHECKDISABLED:
		// (stays enabled while connecting, so it keeps the initial focus)
		return lobbyGetState() == LOBBY_FAILED;
	case MENUOP_SET:
		if (lobbyGetState() != LOBBY_ONLINE || lobbyGetNumArenas() == 0) {
			break;
		}
		if (!g_OnlineCreate.name[0]) {
			char name[16];
			s32 len = 0;

			for (s32 i = 0; i < (s32)sizeof(g_GameFile.name) && g_GameFile.name[i] >= 0x20; i++) {
				name[len++] = g_GameFile.name[i];
			}

			name[len] = '\0';
			snprintf(g_OnlineCreate.name, sizeof(g_OnlineCreate.name), "%s's Match", len ? name : "Agent");
		}
		menuPushDialog(&g_OnlineCreateMenuDialog);
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineMenuHandlerRefresh(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		lobbyConnect(lobbyGetServer());
	}

	return 0;
}

static MenuDialogHandlerResult onlineMenuDialog(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_OPEN:
		lobbyConnect(lobbyGetServer());
		break;
	case MENUOP_CLOSE:
		lobbyDisconnect();
		break;
	case MENUOP_TICK:
		if (g_Menus[g_MpPlayerNum].curdialog && g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
			lobbyTick();
		}
		break;
	}

	return false;
}

struct menuitem g_OnlineMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&onlineMenuTextStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SELECTABLE_CENTRE,
		(uintptr_t)&onlineMenuTextServer,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
#define ROOMROW { MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&onlineMenuTextRoom, (uintptr_t)&onlineMenuTextRoomRight, onlineMenuHandlerRoom }
	ROOMROW, ROOMROW, ROOMROW, ROOMROW, ROOMROW, ROOMROW, ROOMROW, ROOMROW,
#undef ROOMROW
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Create Match...\n",
		0,
		onlineMenuHandlerCreate,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Refresh\n",
		0,
		onlineMenuHandlerRefresh,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		(uintptr_t)"Back\n",
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static s32 onlineRoomIndex(struct menuitem *item)
{
	return (s32)(item - g_OnlineMenuItems) - ONLINE_FIRST_ROOM_ITEM;
}

struct menudialogdef g_OnlineMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Online Matches",
	g_OnlineMenuItems,
	onlineMenuDialog,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

/* ------------------------------------------------------------------------
 * Match details
 * ------------------------------------------------------------------------ */

static char *onlineRoomTitle(struct menudialogdef *dialogdef)
{
	const struct lobbyroom *room = onlineSelectedRoom();
	return onlineText(0, "%.24s", room ? room->name : "Match");
}

static char *onlineRoomTextScenario(struct menuitem *item)
{
	const struct lobbyroom *room = onlineSelectedRoom();
	return onlineText(1, "Scenario: %s\n", room ? room->scenario : "-");
}

static char *onlineRoomTextArena(struct menuitem *item)
{
	const struct lobbyroom *room = onlineSelectedRoom();
	return onlineText(2, "Arena: %s\n", room ? room->arena : "-");
}

static char *onlineRoomTextSlots(struct menuitem *item)
{
	const struct lobbyroom *room = onlineSelectedRoom();

	if (!room) {
		return "Free Slots: -\n";
	}

	if (!room->playing) {
		return "Match over\n";
	}

	if (room->reserved) {
		return onlineText(3, "Free Slots: %d of %d (%d held)\n", room->free, room->numslots, room->reserved);
	}

	return onlineText(3, "Free Slots: %d of %d\n", room->free, room->numslots);
}

static char *onlineRoomTextBots(struct menuitem *item)
{
	const struct lobbyroom *room = onlineSelectedRoom();
	return onlineText(4, "Bots: %d\n", room ? room->bots : 0);
}

static char *onlineRoomTextTime(struct menuitem *item)
{
	const struct lobbyroom *room = onlineSelectedRoom();
	const s32 secs = room ? room->timeleft : -1;

	if (secs < 0) {
		return "Time Left: No limit\n";
	}

	return onlineText(5, "Time Left: %d:%02d\n", secs / 60, secs % 60);
}

static char *onlineRoomTextPlayers(struct menuitem *item)
{
	const struct lobbyroom *room = onlineSelectedRoom();
	char *out = g_OnlineText[6];
	s32 len = 0;

	out[0] = '\0';

	if (!room || room->numplayers == 0) {
		return "Nobody playing yet\n";
	}

	len = snprintf(out, sizeof(g_OnlineText[6]), "Playing: ");

	for (s32 i = 0; i < room->numslots; i++) {
		if (room->players[i][0]) {
			len += snprintf(out + len, sizeof(g_OnlineText[6]) - len, "%s%s", len > 9 ? ", " : "", room->players[i]);

			if (len >= (s32)sizeof(g_OnlineText[6]) - 2) {
				break;
			}
		}
	}

	snprintf(out + len, sizeof(g_OnlineText[6]) - len, "\n");
	return out;
}

static char *onlineRoomTextJoin(struct menuitem *item)
{
	const struct lobbyroom *room = onlineSelectedRoom();

	if (lobbyGetState() == LOBBY_ENTERING) {
		return "Joining...\n";
	}

	if (!room || !room->playing) {
		return "Match Over\n";
	}

	if (room->free <= 0) {
		return "Match Full\n";
	}

	return room->locked ? "Join (Password)...\n" : "Join Match\n";
}

static MenuItemHandlerResult onlineRoomHandlerJoin(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const struct lobbyroom *room = onlineSelectedRoom();

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !room || !room->playing || room->free <= 0 || lobbyGetState() != LOBBY_ONLINE;
	case MENUOP_SET:
		if (room) {
			if (room->locked) {
				menuPushDialog(&g_OnlineJoinPasswordMenuDialog);
			} else {
				lobbyJoin(room->id, "");
			}
		}
		break;
	}

	return 0;
}

static MenuDialogHandlerResult onlineSubDialog(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_TICK && g_Menus[g_MpPlayerNum].curdialog
			&& g_Menus[g_MpPlayerNum].curdialog->definition == dialogdef) {
		lobbyTick();
	}

	return false;
}

struct menuitem g_OnlineRoomMenuItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&onlineRoomTextScenario, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&onlineRoomTextArena, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&onlineRoomTextSlots, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&onlineRoomTextBots, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)&onlineRoomTextTime, 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SELECTABLE_CENTRE, (uintptr_t)&onlineRoomTextPlayers, 0, NULL },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_CENTRE, (uintptr_t)&onlineRoomTextJoin, 0, onlineRoomHandlerJoin },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_OnlineRoomMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)&onlineRoomTitle,
	g_OnlineRoomMenuItems,
	onlineSubDialog,
	0,
	NULL,
};

/* ------------------------------------------------------------------------
 * Keyboard dialogs: join password, match name, create password
 * ------------------------------------------------------------------------ */

static MenuItemHandlerResult onlineKbJoinPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		strcpy(data->keyboard.string, g_OnlineJoinPassword);
		break;
	case MENUOP_SETTEXT:
		snprintf(g_OnlineJoinPassword, sizeof(g_OnlineJoinPassword), "%s", data->keyboard.string);
		break;
	case MENUOP_SET:
		lobbyJoin(g_OnlineRoomId, g_OnlineJoinPassword);
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineKbMatchName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		strcpy(data->keyboard.string, g_OnlineCreate.name);
		break;
	case MENUOP_SETTEXT:
		snprintf(g_OnlineCreate.name, sizeof(g_OnlineCreate.name), "%s", data->keyboard.string);
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineKbCreatePassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETTEXT:
		strcpy(data->keyboard.string, g_OnlineCreate.password);
		break;
	case MENUOP_SETTEXT:
		snprintf(g_OnlineCreate.password, sizeof(g_OnlineCreate.password), "%s", data->keyboard.string);
		break;
	}

	return 0;
}

// the keyboard item's param is the maximum length; param3 = 1 allows text wider than an agent name
struct menuitem g_OnlineJoinPasswordMenuItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)"Enter the match password:\n", 0, NULL },
	{ MENUITEMTYPE_KEYBOARD, 16, 0, 0, 1, onlineKbJoinPassword },
	{ MENUITEMTYPE_END },
};

struct menuitem g_OnlineMatchNameMenuItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)"Enter a name for the match:\n", 0, NULL },
	{ MENUITEMTYPE_KEYBOARD, 24, 0, 0, 1, onlineKbMatchName },
	{ MENUITEMTYPE_END },
};

struct menuitem g_OnlineCreatePasswordMenuItems[] = {
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING, (uintptr_t)"Players will need this to join:\n", 0, NULL },
	{ MENUITEMTYPE_KEYBOARD, 16, 0, 0, 1, onlineKbCreatePassword },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_OnlineJoinPasswordMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Password", g_OnlineJoinPasswordMenuItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_DISABLEBANNER, NULL,
};

struct menudialogdef g_OnlineMatchNameMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Match Name", g_OnlineMatchNameMenuItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_DISABLEBANNER, NULL,
};

struct menudialogdef g_OnlineCreatePasswordMenuDialog = {
	MENUDIALOGTYPE_DEFAULT, (uintptr_t)"Match Password", g_OnlineCreatePasswordMenuItems, NULL, MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_DISABLEBANNER, NULL,
};

/* ------------------------------------------------------------------------
 * Create Match
 * ------------------------------------------------------------------------ */

static char *onlineCreateTextName(struct menuitem *item)
{
	return onlineText(1, "%.16s\n", g_OnlineCreate.name);
}

static char *onlineCreateTextPassword(struct menuitem *item)
{
	return g_OnlineCreate.password[0] ? "Set\n" : "None\n";
}

static MenuItemHandlerResult onlineCreateHandlerArena(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = lobbyGetNumArenas() + 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)(data->dropdown.value == 0 ? "Random" : lobbyGetArenaName(data->dropdown.value - 1));
	case MENUOP_SET:
		g_OnlineCreateArena = data->dropdown.value;
		g_OnlineCreate.stage = g_OnlineCreateArena ? lobbyGetArenaId(g_OnlineCreateArena - 1) : -1;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_OnlineCreateArena <= lobbyGetNumArenas() ? g_OnlineCreateArena : 0;
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerScenario(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = lobbyGetNumScenarios();
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)lobbyGetScenarioName(data->dropdown.value);
	case MENUOP_SET:
		g_OnlineCreate.scenario = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_OnlineCreate.scenario;
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerWeapons(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = lobbyGetNumWeaponSets();
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)lobbyGetWeaponSetName(data->dropdown.value);
	case MENUOP_SET:
		g_OnlineCreate.weaponset = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_OnlineCreate.weaponset;
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerBotSkill(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Meat", "Easy", "Normal", "Hard", "Perfect", "Dark" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_OnlineCreate.botskill = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_OnlineCreate.botskill;
		break;
	}

	return 0;
}

// slider value 0..19 = 1..20 minutes
static MenuItemHandlerResult onlineCreateHandlerTime(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_OnlineCreate.timelimit - 1;
		break;
	case MENUOP_SET:
		g_OnlineCreate.timelimit = data->slider.value + 1;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d min", data->slider.value + 1);
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerKills(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_OnlineCreate.scorelimit;
		break;
	case MENUOP_SET:
		g_OnlineCreate.scorelimit = data->slider.value;
		break;
	case MENUOP_GETSLIDERLABEL:
		if (data->slider.value == 0) {
			sprintf(data->slider.label, "None");
		} else {
			sprintf(data->slider.label, "%d", data->slider.value);
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerBots(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_OnlineCreate.bots;
		break;
	case MENUOP_SET:
		g_OnlineCreate.bots = data->slider.value;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", data->slider.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerTeams(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		// King of the Hill and Capture the Case are always team games
		return g_OnlineCreate.teams || g_OnlineCreate.scenario >= 4;
	case MENUOP_SET:
		g_OnlineCreate.teams = data->checkbox.value;
		break;
	case MENUOP_CHECKDISABLED:
		return g_OnlineCreate.scenario >= 4;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_OnlineMatchNameMenuDialog);
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		menuPushDialog(&g_OnlineCreatePasswordMenuDialog);
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerNoPassword(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return g_OnlineCreate.password[0] == '\0';
	case MENUOP_SET:
		g_OnlineCreate.password[0] = '\0';
		break;
	}

	return 0;
}

static char *onlineCreateTextStatus(struct menuitem *item)
{
	return onlineText(7, "%s\n", lobbyGetStatus());
}

static MenuItemHandlerResult onlineCreateHandlerStatus(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		// only worth showing when something is happening or went wrong
		return lobbyGetState() == LOBBY_ONLINE && strcmp(lobbyGetStatus(), "Connected") == 0;
	}

	return 0;
}

static MenuItemHandlerResult onlineCreateHandlerStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return lobbyGetState() != LOBBY_ONLINE;
	case MENUOP_SET:
		if (g_OnlineCreate.scenario >= 4) {
			g_OnlineCreate.teams = true;
		}
		lobbyCreate(&g_OnlineCreate);
		break;
	}

	return 0;
}

struct menuitem g_OnlineCreateMenuItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Name\n", (uintptr_t)&onlineCreateTextName, onlineCreateHandlerName },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Arena", 0, onlineCreateHandlerArena },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Scenario", 0, onlineCreateHandlerScenario },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Weapons", 0, onlineCreateHandlerWeapons },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Time Limit", 19, onlineCreateHandlerTime },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Kill Limit", 50, onlineCreateHandlerKills },
	{ MENUITEMTYPE_SLIDER, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Bots", 8, onlineCreateHandlerBots },
	{ MENUITEMTYPE_DROPDOWN, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Bot Skill", 0, onlineCreateHandlerBotSkill },
	{ MENUITEMTYPE_CHECKBOX, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Teams", 0, onlineCreateHandlerTeams },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Password\n", (uintptr_t)&onlineCreateTextPassword, onlineCreateHandlerPassword },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Remove Password\n", 0, onlineCreateHandlerNoPassword },
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SELECTABLE_CENTRE, (uintptr_t)&onlineCreateTextStatus, 0, onlineCreateHandlerStatus },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CENTRE, (uintptr_t)"Start Match\n", 0, onlineCreateHandlerStart },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE, (uintptr_t)"Back\n", 0, NULL },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_OnlineCreateMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Create Match",
	g_OnlineCreateMenuItems,
	onlineSubDialog,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/* ------------------------------------------------------------------------ */

// called by the Perfect Menu every frame; opens the online menu once after returning from a match
void onlineMenuCheckReturn(void)
{
	static s32 checked = false;

	if (!checked) {
		checked = true;

		if (lobbyShouldReturnToMenu()) {
			menuPushDialog(&g_OnlineMenuDialog);
		}
	}
}
