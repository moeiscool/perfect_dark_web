// One online match: the server's authoritative instance of the game (headless) plus the 60 Hz
// clock that turns the players' latest inputs into ticks and sends them to everyone.
// See port/src/net.c for the game side and web/net-client.js for the browser side.
'use strict';

const zlib = require('node:zlib');
const crypto = require('node:crypto');
const PDNet = require('./nethost.js');
const { makeMatchConfig } = require('./settings.js');
const { startHeadless } = require('./headless.js');

const TICK_MS = 1000 / 60;
const HISTORY_TICKS = 60 * 10;        // ticks kept to send after a snapshot
const INPUT_TIMEOUT_MS = 3000;        // a player who stops sending input is treated as idle
                                      // (clients send only changes, plus a 1 s heartbeat)
const RECONNECT_GRACE_MS = 60 * 1000; // a disconnected player's slot is kept for them this long
const HASH_INTERVAL = 60;

// binary message types (first byte)
const MSG_TICK = 1;
const MSG_SNAPSHOT = 2;
const MSG_INPUT = 3;

class Match {
  /**
   * @param {object} opts
   * @param {string} opts.id
   * @param {object} opts.settings  validated room settings (see lobby.js)
   * @param {string} opts.buildDir
   * @param {Buffer} opts.rom
   * @param {Array} opts.snapRanges  from pd.snap.json
   * @param {function} opts.log
   * @param {function} opts.onChange  called when the room's public state changes
   * @param {function} opts.onEnded   called with the results when the match is over
   */
  constructor(opts) {
    Object.assign(this, opts);
    this.tick = 0;
    this.slots = [null, null, null, null];   // player record or null
    this.reserved = [null, null, null, null]; // { token, until } for disconnected players
    this.players = new Map();                 // token -> player record
    this.pendingEvents = new Map();           // tick -> events bitmask
    this.history = [];                        // [tick, Uint8Array packet]
    this.serverHashes = new Map();            // tick -> hash
    this.state = 'starting';
    this.results = null;
    this.desyncs = 0;

    this.matchConfig = makeMatchConfig(this.settings);
  }

  async start() {
    const inst = await startHeadless({
      buildDir: this.buildDir,
      rom: this.rom,
      match: this.matchConfig,
      log: (t) => { if (/FATAL|ERROR|net:/.test(t)) this.log(`[game] ${t}`); },
      onHash: (tick, hash) => this.onServerHash(tick, hash),
    });
    this.module = inst.module;
    this.host = inst.host;
    this.state = 'playing';
    this.startedAt = Date.now();
    this.nextTickAt = performance.now();
    this.timer = setTimeout(() => this.loop(), 0);
    this.log(`match ${this.id} started: stage 0x${this.settings.stage.toString(16)}, seed ${this.matchConfig.seed}`);
  }

  stop() {
    clearTimeout(this.timer);
    this.state = 'closed';
    for (const p of this.players.values()) {
      p.live = false;
    }
  }

  // ---------------------------------------------------------------------------
  // clock

  loop() {
    if (this.state !== 'playing') {
      return;
    }

    const now = performance.now();
    let steps = 0;

    while (this.nextTickAt <= now && steps < 6) {
      this.step();
      this.nextTickAt += TICK_MS;
      steps++;
    }

    if (now - this.nextTickAt > 250) {
      // the server can't keep up (or was suspended); don't try to catch up all at once
      this.nextTickAt = now;
    }

    if (this.state === 'playing') {
      this.timer = setTimeout(() => this.loop(), Math.max(0, this.nextTickAt - performance.now()));
    }
  }

  step() {
    const t = this.tick++;
    const now = Date.now();
    const inputs = this.slots.map((p) => {
      if (!p || !p.connected || now - p.lastInputAt > INPUT_TIMEOUT_MS) {
        return PDNet.NEUTRAL_INPUT;
      }
      const input = { ...p.input, flags: p.input.flags | p.pendingFlags };
      p.pendingFlags = 0;
      return input;
    });
    const events = this.pendingEvents.get(t) || 0;
    this.pendingEvents.delete(t);

    const packet = new Uint8Array(1 + PDNet.TICK_PACKET_SIZE);
    packet[0] = MSG_TICK;
    PDNet.encodeTick(t, events, inputs, packet.buffer, 1);

    this.host.push(t, events, inputs);

    this.history.push([t, packet]);
    if (this.history.length > HISTORY_TICKS) {
      this.history.shift();
    }

    for (const p of this.players.values()) {
      if (p.live && p.ws) {
        p.ws.send(packet);
      }
    }

    // the game ran the previous tick by now (it runs between our timer callbacks)
    if (t > 0 && (t % 30) === 0 && this.module._netGetMatchOver()) {
      this.finish();
    }
  }

  finish() {
    if (this.state !== 'playing') {
      return;
    }
    this.state = 'ended';
    clearTimeout(this.timer);
    this.results = this.readResults();
    this.log(`match ${this.id} ended at tick ${this.tick}`);
    for (const p of this.players.values()) {
      this.sendJson(p, { type: 'ended', results: this.results });
    }
    this.onEnded(this.results);
    this.onChange();
  }

  readResults() {
    const count = this.module._netGetResults();
    const ptr = this.module._netGetResultsBuffer();
    const view = new DataView(this.module.HEAPU8.buffer);
    const out = [];
    for (let i = 0; i < count; i++) {
      const base = ptr + i * 36;
      let name = '';
      for (let j = 0; j < 16; j++) {
        const c = view.getUint8(base + j);
        if (!c) break;
        name += String.fromCharCode(c);
      }
      const slot = view.getInt32(base + 16, true);
      if (slot >= 0 && this.module._netGetSlotVacant(slot) && !this.everOccupied?.has(slot)) {
        continue; // never played
      }
      out.push({
        name,
        bot: slot < 0,
        kills: view.getInt32(base + 20, true),
        suicides: view.getInt32(base + 24, true),
        deaths: view.getInt32(base + 28, true),
        points: view.getInt32(base + 32, true),
      });
    }
    out.sort((a, b) => (b.kills - b.suicides) - (a.kills - a.suicides) || a.deaths - b.deaths);
    return out;
  }

  // ---------------------------------------------------------------------------
  // players

  get humanCount() {
    return this.slots.filter((p) => p && p.connected).length;
  }

  get timeLeftSecs() {
    if (this.settings.timelimit >= 60) {
      return null;
    }
    return Math.max(0, Math.round(this.settings.timelimit * 60 - this.tick / 60));
  }

  publicInfo() {
    const s = this.settings;
    return {
      id: this.id,
      name: s.name,
      stage: s.stage,
      scenario: s.scenario,
      bots: s.bots,
      players: this.slots.map((p) => (p && p.connected ? p.name : null)),
      // slots a new player can take (a disconnected player's slot is kept for them for a while)
      free: this.slots.filter((p, i) => !p && (!this.reserved[i] || this.reserved[i].until < Date.now())).length,
      reservedSlots: this.slots.filter((p, i) => !p && this.reserved[i] && this.reserved[i].until >= Date.now()).length,
      locked: !!s.password,
      state: this.state,
      timeLeft: this.timeLeftSecs,
    };
  }

  freeSlotFor(token) {
    const now = Date.now();
    // the player's own slot, if they had one
    const known = token && this.players.get(token);
    if (known && known.slot >= 0 && (!this.slots[known.slot] || this.slots[known.slot] === known)) {
      return known.slot;
    }
    for (let i = 0; i < 4; i++) {
      const r = this.reserved[i];
      if (!this.slots[i] && (!r || r.until < now || r.token === token)) {
        return i;
      }
    }
    return -1;
  }

  /**
   * Adds a player (or reconnects one). Returns the player record, or throws.
   */
  join(ws, { token, name }) {
    if (this.state !== 'playing') {
      throw new Error(this.state === 'ended' ? 'This match is over.' : 'This match isn\'t running.');
    }

    let p = token && this.players.get(token);
    const returning = !!p;
    if (p && p.connected && p.ws && p.ws !== ws) {
      // same player from another tab/reconnect: drop the old connection
      try { p.ws.close(4000, 'replaced'); } catch { /* ignore */ }
    }

    const slot = this.freeSlotFor(token);
    if (slot < 0) {
      throw new Error('This match is full.');
    }

    if (!p) {
      token = crypto.randomBytes(16).toString('hex');
      p = { token, slot, input: { ...PDNet.NEUTRAL_INPUT }, pendingFlags: 0, lastInputAt: 0 };
      this.players.set(token, p);
    }

    p.ws = ws;
    p.name = String(name || 'Agent').replace(/[^\x20-\x7e]/g, '').slice(0, 12) || 'Agent';
    p.slot = slot;
    p.connected = true;
    p.live = false;
    this.slots[slot] = p;
    this.reserved[slot] = null;
    this.everOccupied = this.everOccupied || new Set();
    this.everOccupied.add(slot);

    // the slot's player appears (and gets the name) a couple of ticks from now, on every machine
    const at = this.tick + 2;
    this.pendingEvents.set(at, (this.pendingEvents.get(at) || 0) | PDNet.EVENT_OCCUPY(slot));
    this.queueName(at, slot, p.name);

    this.log(`match ${this.id}: ${p.name} joined slot ${slot}`);
    this.notice(`${p.name} has ${returning ? 're' : ''}joined`, p);
    this.onChange();
    return p;
  }

  queueName(tick, slot, name) {
    // the server's instance gets it directly; it's part of the game state, so snapshots carry it
    const buf = this.module._netGetNameBuffer();
    const bytes = Buffer.from(name.slice(0, 12), 'latin1');
    this.module.HEAPU8.fill(0, buf, buf + 32);
    this.module.HEAPU8.set(bytes, buf);
    this.module._netQueueName(tick, slot);
    for (const other of this.players.values()) {
      if (other.live) {
        this.sendJson(other, { type: 'name', tick, slot, name });
      }
    }
  }

  /**
   * Sends the player the current game state and everything after it, then makes them live.
   */
  async sendSnapshot(p, reason) {
    const snap = PDNet.makeSnapshot(this.module, this.snapRanges);
    const startedAt = Date.now();
    const packed = await new Promise((resolve, reject) =>
      zlib.deflateRaw(snap.bytes, { level: 6 }, (err, out) => (err ? reject(err) : resolve(out))));

    if (!p.connected || p.ws.readyState !== 1) {
      return;
    }

    const msg = new Uint8Array(5 + packed.length);
    msg[0] = MSG_SNAPSHOT;
    new DataView(msg.buffer).setUint32(1, snap.tick, true);
    msg.set(packed, 5);
    p.ws.send(msg);

    // then every tick since the snapshot, then live ticks
    for (const [t, packet] of this.history) {
      if (t > snap.tick) {
        p.ws.send(packet);
      }
    }
    p.live = true;
    this.log(`match ${this.id}: snapshot for ${p.name} (${reason}) at tick ${snap.tick}, ` +
      `${(packed.length / 1048576).toFixed(2)} MiB, ${Date.now() - startedAt} ms`);
  }

  leave(p, explicit) {
    if (!p || !p.connected) {
      return;
    }
    p.connected = false;
    p.live = false;
    p.ws = null;

    if (this.slots[p.slot] === p) {
      this.slots[p.slot] = null;
      if (!explicit) {
        // keep the slot (and its score) for them for a while
        this.reserved[p.slot] = { token: p.token, until: Date.now() + RECONNECT_GRACE_MS };
      } else {
        this.players.delete(p.token);
      }
      if (this.state === 'playing') {
        const at = this.tick + 1;
        this.pendingEvents.set(at, (this.pendingEvents.get(at) || 0) | PDNet.EVENT_VACATE(p.slot));
      }
    }

    this.log(`match ${this.id}: ${p.name} ${explicit ? 'left' : 'disconnected'} (slot ${p.slot})`);
    this.notice(explicit ? `${p.name} has left` : `${p.name} lost connection`, p);
    this.onChange();
  }

  onInput(p, bytes) {
    if (bytes.length < 1 + PDNet.INPUT_SIZE) {
      return;
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset + 1, PDNet.INPUT_SIZE);
    const input = PDNet.readInput(view, 0);
    // one-shot flags (ESC) must not be lost if several inputs arrive between ticks
    p.pendingFlags |= input.flags & PDNet.FLAG_ESC;
    p.input = input;
    p.lastInputAt = Date.now();
  }

  // ---------------------------------------------------------------------------
  // desync detection

  onServerHash(tick, hash) {
    this.serverHashes.set(tick, hash >>> 0);
    if (this.serverHashes.size > 200) {
      this.serverHashes.delete(this.serverHashes.keys().next().value);
    }
  }

  onClientHash(p, tick, hash) {
    const expected = this.serverHashes.get(tick);
    if (!p.firstHashLogged) {
      p.firstHashLogged = true;
      this.log(`match ${this.id}: first state check from ${p.name} at tick ${tick} ` +
        `(server at tick ${this.tick}, has hash: ${expected !== undefined}, live: ${p.live})`);
    }
    if (expected === undefined || !p.live) {
      this.hashSkipped = (this.hashSkipped || 0) + 1;
      return;
    }
    if (expected === (hash >>> 0)) {
      this.hashChecks = (this.hashChecks || 0) + 1;
      if (this.hashChecks % 50 === 0) {
        this.log(`match ${this.id}: ${this.hashChecks} client state checks matched the server, ${this.desyncs} desyncs, ${this.hashSkipped || 0} skipped`);
      }
    } else {
      this.desyncs++;
      this.log(`match ${this.id}: DESYNC for ${p.name} at tick ${tick} ` +
        `(client ${(hash >>> 0).toString(16)}, server ${expected.toString(16)}); resyncing`);
      p.live = false;
      this.sendJson(p, { type: 'resync' });
      this.sendSnapshot(p, 'resync').catch((e) => this.log(`snapshot failed: ${e.message}`));
    }
  }

  // a message shown to everyone else in the match (eg. "Alpha has joined")
  notice(text, except) {
    for (const other of this.players.values()) {
      if (other !== except && other.connected) {
        this.sendJson(other, { type: 'notice', text });
      }
    }
  }

  sendJson(p, obj) {
    if (p.ws && p.ws.readyState === 1) {
      p.ws.send(JSON.stringify(obj));
    }
  }
}

module.exports = { Match, MSG_TICK, MSG_SNAPSHOT, MSG_INPUT, HASH_INTERVAL };
