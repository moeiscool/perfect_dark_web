// The lobby: one Durable Object that holds every player's WebSocket, the list of matches and the
// matches themselves (relay-room.js). It speaks the same protocol as web/net/lobby.js, so the game's
// Online menu (port/src/lobby.c) and the page (web/net-client.js) work with either server.
import { RelayRoom, MSG_INPUT, MSG_SNAPSHOT_REPLY } from './relay-room.js';
import settingsModule from '../../web/net/settings.js';
import { BUILD_ID } from './build-info.js';

const { ARENAS, SCENARIOS, WEAPON_SETS, validateSettings } = settingsModule;

const ROOM_IDLE_CLOSE_MS = 60 * 1000;   // nobody connected: the match only lives in players' games
const ROOM_ENDED_CLOSE_MS = 60 * 1000;
const ROOM_WAITING_CLOSE_MS = 2 * 60 * 1000; // created but nobody arrived

function randomHex(bytes) {
  return Array.from(crypto.getRandomValues(new Uint8Array(bytes)), (b) => b.toString(16).padStart(2, '0')).join('');
}

export class LobbyDO {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.maxRooms = parseInt(env.MAX_ROOMS || '4', 10);
    this.rooms = new Map();
    this.clients = new Set();
    this.broadcastTimer = null;
    this.housekeepingTimer = null;
  }

  log(text) {
    console.log(text);
  }

  async fetch(request) {
    if (request.headers.get('Upgrade') !== 'websocket') {
      return new Response('Expected a WebSocket', { status: 426 });
    }
    const pair = new WebSocketPair();
    const [client, server] = Object.values(pair);
    server.accept();
    this.onConnection(server);
    return new Response(null, { status: 101, webSocket: client });
  }

  onConnection(ws) {
    const client = { ws, name: 'Agent', clientId: null, room: null, player: null };
    this.clients.add(client);
    this.ensureHousekeeping();

    ws.addEventListener('message', (ev) => {
      try {
        if (typeof ev.data !== 'string') {
          const bytes = new Uint8Array(ev.data);
          const room = client.room;
          if (room && client.player) {
            if (bytes[0] === MSG_INPUT) {
              room.onInput(client.player, bytes);
            } else if (bytes[0] === MSG_SNAPSHOT_REPLY) {
              room.onSnapshotReply(client.player, bytes);
            }
          }
          return;
        }
        this.onMessage(client, JSON.parse(ev.data));
      } catch (e) {
        this.send(client, { type: 'error', message: e.message || String(e) });
      }
    });

    const closed = () => {
      if (!this.clients.has(client)) {
        return;
      }
      this.clients.delete(client);
      if (client.room && client.player) {
        client.room.leave(client.player, false);
      }
    };
    ws.addEventListener('close', closed);
    ws.addEventListener('error', closed);
  }

  onMessage(client, msg) {
    const room = client.room;
    const player = client.player;

    switch (msg.type) {
      case 'hello':
        client.name = String(msg.name || 'Agent').slice(0, 12);
        client.clientId = String(msg.clientId || '').slice(0, 64);
        if (msg.build && BUILD_ID && msg.build !== BUILD_ID) {
          this.send(client, { type: 'error', code: 'build', message: 'The game was updated. Please reload the page.' });
          return;
        }
        this.send(client, {
          type: 'welcome',
          build: BUILD_ID,
          enabled: true,
          relay: true,
          arenas: ARENAS,
          scenarios: SCENARIOS,
          weaponSets: WEAPON_SETS,
        });
        this.send(client, { type: 'rooms', rooms: this.roomList() });
        break;

      case 'create':
        this.create(client, msg.settings, msg.join !== false);
        break;

      case 'join':
        this.join(client, msg);
        break;

      case 'leave':
        if (room && player) {
          room.leave(player, true);
        }
        client.room = null;
        client.player = null;
        this.send(client, { type: 'left' });
        break;

      case 'hash':
        if (room && player) {
          room.onClientHash(player, msg.tick >>> 0, msg.hash >>> 0);
        }
        break;

      case 'over':
        if (room && player) {
          room.onOver(player, msg.results);
        }
        break;

      case 'snapshot-failed':
        if (room && player) {
          room.onSnapshotFailed(player, msg.reqId >>> 0);
        }
        break;

      case 'ping':
        this.send(client, { type: 'pong', t: msg.t });
        break;

      default:
        break;
    }
  }

  create(client, rawSettings, join) {
    const live = [...this.rooms.values()].filter((r) => r.state !== 'closed' && r.state !== 'ended');
    if (live.length >= this.maxRooms) {
      throw new Error(`The server is busy (${this.maxRooms} matches running). Join one or try again later.`);
    }
    const settings = validateSettings(rawSettings);
    const id = randomHex(4);
    const room = new RelayRoom({
      id,
      settings,
      log: (t) => this.log(t),
      onChange: () => this.scheduleBroadcast(),
      onEnded: () => { room.endedAt = Date.now(); },
    });
    this.rooms.set(id, room);
    this.log(`match ${id} created: ${settings.name}`);
    this.scheduleBroadcast();

    if (join) {
      this.join(client, { roomId: id, password: settings.password });
    } else {
      this.send(client, { type: 'created', roomId: id });
    }
  }

  join(client, msg) {
    const room = this.rooms.get(msg.roomId);
    if (!room || room.state === 'closed') {
      throw new Error('That match no longer exists.');
    }
    if (room.settings.password && msg.password !== room.settings.password && !(msg.token && room.players.has(msg.token))) {
      throw new Error('Wrong password.');
    }
    if (client.room && client.player && client.room !== room) {
      client.room.leave(client.player, true);
    }

    const { player, fresh } = room.join(client.ws, { token: msg.token, name: client.name });
    client.room = room;
    client.player = player;

    this.send(client, {
      type: 'joined',
      roomId: room.id,
      slot: player.slot,
      token: player.token,
      match: room.matchConfig,
      settings: { ...room.settings, password: undefined },
      arena: ARENAS[room.settings.stage],
      scenarioName: SCENARIOS[room.settings.scenario],
      fresh,
    });

    if (fresh) {
      room.sendNamesAfter(player, -1);
    } else {
      room.provideSnapshot(player, room.players.size > 1 ? 'join' : 'rejoin');
    }
  }

  // ---------------------------------------------------------------------------

  ensureHousekeeping() {
    if (!this.housekeepingTimer) {
      this.housekeepingTimer = setInterval(() => this.housekeeping(), 5000);
    }
  }

  housekeeping() {
    const now = Date.now();
    for (const room of this.rooms.values()) {
      if (room.humanCount > 0) {
        room.emptySince = now;
      }
      const idle = room.state === 'playing' && now - room.emptySince > ROOM_IDLE_CLOSE_MS;
      const ended = room.state === 'ended' && now - (room.endedAt || now) > ROOM_ENDED_CLOSE_MS;
      const unused = room.state === 'waiting' && now - room.createdAt > ROOM_WAITING_CLOSE_MS;
      if (idle || ended || unused) {
        this.log(`match ${room.id}: closing (${idle ? 'nobody left' : ended ? 'over' : 'nobody joined'})`);
        this.closeRoom(room);
      }
    }
    if (this.rooms.size) {
      this.scheduleBroadcast(); // time left
    }
    if (!this.rooms.size && !this.clients.size) {
      clearInterval(this.housekeepingTimer);
      this.housekeepingTimer = null;
    }
  }

  closeRoom(room) {
    room.stop();
    this.rooms.delete(room.id);
    for (const c of this.clients) {
      if (c.room === room) {
        c.room = null;
        c.player = null;
      }
    }
    this.scheduleBroadcast();
  }

  roomList() {
    return [...this.rooms.values()]
      .filter((r) => r.state !== 'closed')
      .map((r) => ({ ...r.publicInfo(), arena: ARENAS[r.settings.stage], scenarioName: SCENARIOS[r.settings.scenario] }));
  }

  scheduleBroadcast() {
    if (this.broadcastTimer) {
      return;
    }
    this.broadcastTimer = setTimeout(() => {
      this.broadcastTimer = null;
      const msg = JSON.stringify({ type: 'rooms', rooms: this.roomList() });
      for (const c of this.clients) {
        if (!c.room) {
          try { c.ws.send(msg); } catch { /* closing */ }
        }
      }
    }, 250);
  }

  send(client, obj) {
    try {
      client.ws.send(JSON.stringify(obj));
    } catch {
      // closed
    }
  }
}
