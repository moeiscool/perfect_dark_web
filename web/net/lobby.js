// Multiplayer lobby: WebSocket endpoint /net on the web server. Keeps the list of rooms (matches),
// creates them, and routes players' messages to their match. See web/net/match.js.
//
// Client -> server JSON: hello {name, clientId, build}, create {settings}, join {roomId, token, password},
//                        leave, hash {tick, hash}, ping {t}
// Client -> server binary: [3][netinput]
// Server -> client JSON: welcome, rooms {rooms}, joined {roomId, slot, token, match, settings},
//                        name {tick, slot, name}, ended {results}, resync, left, error {message}, pong {t}
// Server -> client binary: [1][tick packet], [2][u32 tick][deflate-raw snapshot]
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { WebSocketServer } = require('ws');
const { Match, MSG_INPUT } = require('./match.js');

// arenas that can be picked (STAGE_MP_* ids, src/include/constants.h)
const ARENAS = {
  0x32: 'Skedar', 0x29: 'Pipes', 0x17: 'Ravine', 0x20: 'G5 Building', 0x42: 'Sewers', 0x3c: 'Warehouse',
  0x47: 'Grid', 0x41: 'Ruins', 0x3b: 'Area 52', 0x39: 'Base', 0x44: 'Fortress', 0x45: 'Villa',
  0x3d: 'Car Park', 0x25: 'Temple', 0x1f: 'Complex', 0x43: 'Felicity',
};
const SCENARIOS = ['Combat', 'Hold the Briefcase', 'Hacker Central', 'Pop a Cap', 'King of the Hill', 'Capture the Case'];
const WEAPON_SETS = ['Pistols', 'Automatics', 'Power', 'FarSight', 'Tranquilizer', 'Heavy', 'Golden Magnum',
  'Explosive', 'Grenade Launcher', 'Rocket Launcher', 'Proximity Mine', 'Close Combat'];

const MPOPTION_TEAMSENABLED = 0x00000002;
const ROOM_IDLE_CLOSE_MS = 2 * 60 * 1000;
const ROOM_ENDED_CLOSE_MS = 60 * 1000;

function clampInt(v, min, max, def) {
  const n = parseInt(v, 10);
  return Number.isFinite(n) ? Math.min(max, Math.max(min, n)) : def;
}

function validateSettings(s) {
  s = s || {};
  let stage = parseInt(s.stage, 10);
  if (!ARENAS[stage]) {
    // random
    const ids = Object.keys(ARENAS).map(Number);
    stage = ids[crypto.randomInt(ids.length)];
  }
  const scenario = clampInt(s.scenario, 0, SCENARIOS.length - 1, 0);
  // King of the Hill and Capture the Case are team games
  const teams = !!s.teams || scenario === 4 || scenario === 5;
  return {
    name: String(s.name || 'Combat Simulator').replace(/[^\x20-\x7e]/g, '').slice(0, 32) || 'Combat Simulator',
    stage,
    scenario,
    timelimit: clampInt(s.timelimit, 1, 20, 10),
    scorelimit: clampInt(s.scorelimit, 0, 100, 0),
    bots: clampInt(s.bots, 0, 8, 0),
    botDifficulty: clampInt(s.botDifficulty, 0, 5, 2),
    weaponset: clampInt(s.weaponset, 0, WEAPON_SETS.length - 1, 1),
    teams,
    options: teams ? MPOPTION_TEAMSENABLED : 0,
    password: s.password ? String(s.password).slice(0, 32) : '',
  };
}

class Lobby {
  /**
   * @param {object} opts
   * @param {string} opts.buildDir   directory with pd.js, pd.wasm, pd.snap.json
   * @param {string} [opts.romPath]  NTSC v1.1 ROM; without it rooms can't be created
   * @param {number} [opts.maxRooms]
   * @param {function} [opts.log]
   */
  constructor(opts) {
    this.buildDir = opts.buildDir;
    this.maxRooms = opts.maxRooms || 4;
    this.log = opts.log || console.log;
    this.rooms = new Map();
    this.clients = new Set();
    this.rom = null;
    this.buildId = null;
    this.snapRanges = null;

    if (opts.romPath) {
      try {
        this.rom = fs.readFileSync(opts.romPath);
      } catch (e) {
        this.log(`lobby: could not read ROM ${opts.romPath}: ${e.message}; online matches disabled`);
      }
    } else {
      this.log('lobby: no --rom given; online matches disabled');
    }

    this.reloadBuild();
    this.wss = new WebSocketServer({ noServer: true, maxPayload: 64 * 1024 });
    this.wss.on('connection', (ws) => this.onConnection(ws));
    this.broadcastTimer = null;
    // keep the lobby's time-left display fresh
    setInterval(() => { if (this.rooms.size) this.scheduleBroadcast(); }, 5000).unref();
  }

  // the build id ties clients and server to the same pd.wasm (snapshots only work between identical builds)
  reloadBuild() {
    try {
      const wasm = fs.readFileSync(path.join(this.buildDir, 'pd.wasm'));
      this.buildId = crypto.createHash('sha1').update(wasm).digest('hex').slice(0, 12);
      this.snapRanges = JSON.parse(fs.readFileSync(path.join(this.buildDir, 'pd.snap.json'), 'utf8')).ranges;
    } catch (e) {
      this.log(`lobby: build not found (${e.message})`);
    }
    return this.buildId;
  }

  get enabled() {
    return !!(this.rom && this.buildId && this.snapRanges);
  }

  handleUpgrade(req, socket, head) {
    this.wss.handleUpgrade(req, socket, head, (ws) => this.wss.emit('connection', ws, req));
  }

  // ---------------------------------------------------------------------------

  onConnection(ws) {
    const client = { ws, name: 'Agent', clientId: null, room: null, player: null };
    this.clients.add(client);
    ws.binaryType = 'nodebuffer';

    ws.on('message', (data, isBinary) => {
      try {
        if (isBinary) {
          if (data[0] === MSG_INPUT && client.room && client.player) {
            client.room.onInput(client.player, data);
          }
          return;
        }
        this.onMessage(client, JSON.parse(data.toString()));
      } catch (e) {
        this.send(client, { type: 'error', message: e.message || String(e) });
      }
    });

    ws.on('close', () => {
      this.clients.delete(client);
      if (client.room && client.player) {
        client.room.leave(client.player, false);
      }
    });
  }

  onMessage(client, msg) {
    switch (msg.type) {
      case 'hello':
        client.name = String(msg.name || 'Agent').slice(0, 12);
        client.clientId = String(msg.clientId || '').slice(0, 64);
        if (msg.build && this.buildId && msg.build !== this.buildId) {
          this.send(client, { type: 'error', code: 'build', message: 'The game was updated. Please reload the page.' });
          return;
        }
        this.send(client, {
          type: 'welcome',
          build: this.buildId,
          enabled: this.enabled,
          arenas: ARENAS,
          scenarios: SCENARIOS,
          weaponSets: WEAPON_SETS,
        });
        this.send(client, { type: 'rooms', rooms: this.roomList() });
        break;

      case 'create':
        this.create(client, msg.settings).catch((e) => this.send(client, { type: 'error', message: e.message || String(e) }));
        break;

      case 'join':
        this.join(client, msg);
        break;

      case 'leave':
        if (client.room && client.player) {
          client.room.leave(client.player, true);
        }
        client.room = null;
        client.player = null;
        this.send(client, { type: 'left' });
        break;

      case 'hash':
        if (client.room && client.player) {
          client.room.onClientHash(client.player, msg.tick >>> 0, msg.hash >>> 0);
        }
        break;

      case 'ping':
        this.send(client, { type: 'pong', t: msg.t });
        break;

      default:
        break;
    }
  }

  async create(client, rawSettings) {
    if (!this.enabled) {
      throw new Error('Online matches aren\'t available on this server.');
    }
    const live = [...this.rooms.values()].filter((r) => r.state !== 'closed');
    if (live.length >= this.maxRooms) {
      throw new Error(`The server is busy (${this.maxRooms} matches running). Join one or try again later.`);
    }

    const settings = validateSettings(rawSettings);
    const id = crypto.randomBytes(4).toString('hex');
    const room = new Match({
      id,
      settings,
      buildDir: this.buildDir,
      rom: this.rom,
      snapRanges: this.snapRanges,
      log: this.log,
      onChange: () => this.scheduleBroadcast(),
      onEnded: () => setTimeout(() => this.closeRoom(room), ROOM_ENDED_CLOSE_MS),
    });
    this.rooms.set(id, room);
    this.scheduleBroadcast();

    try {
      await room.start();
    } catch (e) {
      this.rooms.delete(id);
      this.scheduleBroadcast();
      throw e;
    }

    room.idleSince = Date.now();
    room.idleTimer = setInterval(() => this.checkIdle(room), 10 * 1000);
    this.join(client, { roomId: id, password: settings.password });
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

    const player = room.join(client.ws, { token: msg.token, name: client.name });
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
    });

    room.sendSnapshot(player, 'join').catch((e) => this.log(`snapshot failed: ${e.message}`));
  }

  checkIdle(room) {
    if (room.state === 'closed') {
      clearInterval(room.idleTimer);
      return;
    }
    if (room.humanCount > 0) {
      room.idleSince = Date.now();
    } else if (Date.now() - room.idleSince > ROOM_IDLE_CLOSE_MS) {
      this.log(`match ${room.id}: no players for ${ROOM_IDLE_CLOSE_MS / 60000} minutes, closing`);
      this.closeRoom(room);
    }
  }

  closeRoom(room) {
    room.stop();
    clearInterval(room.idleTimer);
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
        if (!c.room && c.ws.readyState === 1) {
          c.ws.send(msg);
        }
      }
    }, 250);
  }

  send(client, obj) {
    if (client.ws.readyState === 1) {
      client.ws.send(JSON.stringify(obj));
    }
  }
}

module.exports = { Lobby, validateSettings };
