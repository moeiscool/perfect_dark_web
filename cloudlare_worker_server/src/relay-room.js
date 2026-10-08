// One online match on the relay server. Unlike web/net/match.js there is no copy of the game here:
// the room owns the 60 Hz clock and turns the players' latest inputs into ticks, and the players'
// own games provide everything that needs the game state:
//   - the first player starts the match from tick 0 (no snapshot);
//   - a player joining later gets a snapshot taken by a player already in the match, or the
//     room's latest checkpoint (a snapshot a player sends every CHECKPOINT_MS) if nobody is;
//   - state hashes are compared between players, and whoever disagrees with the majority is
//     resynchronized from a player who agrees;
//   - the players report the result when the game says the match is over.
import PDNetModule from '../../web/net/nethost.js';
import { makeMatchConfig } from '../../web/net/settings.js';

const PDNet = PDNetModule && PDNetModule.encodeTick ? PDNetModule : globalThis.PDNet;

const TICK_MS = 1000 / 60;
const CHECKPOINT_MS = 20 * 1000;
const HISTORY_TICKS = 60 * 35;          // ticks kept: covers the oldest checkpoint still used
const INPUT_TIMEOUT_MS = 3000;          // clients send input changes plus a 1 s heartbeat
const RECONNECT_GRACE_MS = 60 * 1000;
const SNAPSHOT_TIMEOUT_MS = 8000;
const NAMES_KEPT_TICKS = 60 * 40;

export const MSG_TICK = 1;
export const MSG_SNAPSHOT = 2;
export const MSG_INPUT = 3;
export const MSG_SNAPSHOT_REPLY = 4;

function randomHex(bytes) {
  return Array.from(crypto.getRandomValues(new Uint8Array(bytes)), (b) => b.toString(16).padStart(2, '0')).join('');
}

export class RelayRoom {
  /**
   * @param {object} opts
   * @param {string} opts.id
   * @param {object} opts.settings  validated (web/net/settings.js)
   * @param {function} opts.log
   * @param {function} opts.onChange  public state changed
   * @param {function} opts.onEnded   match over
   */
  constructor(opts) {
    Object.assign(this, opts);
    this.tick = 0;
    this.slots = [null, null, null, null];
    this.reserved = [null, null, null, null];
    this.players = new Map();          // token -> player
    this.pendingEvents = new Map();    // tick -> events bitmask
    this.history = [];                 // [tick, Uint8Array packet]
    this.names = [];                   // { tick, slot, name } recently queued
    this.hashReports = new Map();      // tick -> Map(player -> hash)
    this.requests = new Map();         // reqId -> { kind: 'join' | 'checkpoint', target, donor, at }
    this.nextReqId = 1;
    this.checkpoint = null;            // { tick, packed }
    this.lastCheckpointAt = 0;
    this.state = 'waiting';            // waiting for its first player, then playing, ended, closed
    this.results = null;
    this.matchConfig = makeMatchConfig(this.settings);
    this.createdAt = Date.now();
    this.emptySince = Date.now();
    this.stats = { checks: 0, desyncs: 0 };
  }

  // ---------------------------------------------------------------------------
  // clock

  startClock() {
    this.state = 'playing';
    this.nextTickAt = Date.now();
    this.timer = setInterval(() => this.loop(), 4);
    this.log(`match ${this.id} started: stage 0x${this.settings.stage.toString(16)}, seed ${this.matchConfig.seed}`);
  }

  stop() {
    clearInterval(this.timer);
    this.timer = null;
    if (this.state !== 'ended') {
      this.state = 'closed';
    }
  }

  loop() {
    if (this.state !== 'playing') {
      return;
    }
    const now = Date.now();
    let steps = 0;
    while (this.nextTickAt <= now && steps < 6) {
      this.step(now);
      this.nextTickAt += TICK_MS;
      steps++;
    }
    if (now - this.nextTickAt > 250) {
      this.nextTickAt = now; // fell behind (eg. the object was busy); don't burst
    }
    this.housekeeping(now);
  }

  step(now) {
    const t = this.tick++;
    const inputs = this.slots.map((p) => {
      if (!p || !p.connected || now - p.lastInputAt > INPUT_TIMEOUT_MS) {
        return PDNet.NEUTRAL_INPUT;
      }
      const input = { ...p.input, flags: (p.input.flags & ~PDNet.FLAG_ESC) | p.pendingFlags };
      p.pendingFlags = 0;
      return input;
    });
    const events = this.pendingEvents.get(t) || 0;
    this.pendingEvents.delete(t);

    const packet = new Uint8Array(1 + PDNet.TICK_PACKET_SIZE);
    packet[0] = MSG_TICK;
    PDNet.encodeTick(t, events, inputs, packet.buffer, 1);

    this.history.push([t, packet]);
    if (this.history.length > HISTORY_TICKS) {
      this.history.shift();
    }

    for (const p of this.players.values()) {
      if (p.live && p.connected) {
        this.sendRaw(p, packet);
      }
    }
  }

  housekeeping(now) {
    // safety net if nobody reports the end (eg. everyone's game froze): the time limit plus a minute
    if (this.settings.timelimit < 60 && this.tick > (this.settings.timelimit * 60 + 60) * 60) {
      this.finish([]);
      return;
    }

    for (const [reqId, r] of this.requests) {
      if (now - r.at > SNAPSHOT_TIMEOUT_MS) {
        this.requests.delete(reqId);
        if (r.kind === 'join') {
          this.provideSnapshot(r.target, r.reason, r.donor);
        }
      }
    }

    if (now - this.lastCheckpointAt > CHECKPOINT_MS) {
      const donor = this.pickDonor();
      if (donor) {
        this.lastCheckpointAt = now;
        this.requestSnapshot(donor, { kind: 'checkpoint' });
      }
    }
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
    const now = Date.now();
    return {
      id: this.id,
      name: s.name,
      stage: s.stage,
      scenario: s.scenario,
      bots: s.bots,
      players: this.slots.map((p) => (p && p.connected ? p.name : null)),
      free: this.slots.filter((p, i) => !p && (!this.reserved[i] || this.reserved[i].until < now)).length,
      reservedSlots: this.slots.filter((p, i) => !p && this.reserved[i] && this.reserved[i].until >= now).length,
      locked: !!s.password,
      state: this.state === 'waiting' ? 'playing' : this.state,
      timeLeft: this.timeLeftSecs,
    };
  }

  freeSlotFor(token) {
    const now = Date.now();
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
   * Adds or reconnects a player. Returns { player, fresh }; a fresh player starts the match from
   * tick 0, anyone else gets a snapshot (see provideSnapshot). Throws if they can't join.
   */
  join(ws, { token, name }) {
    if (this.state === 'ended' || this.state === 'closed') {
      throw new Error(this.state === 'ended' ? 'This match is over.' : 'This match no longer exists.');
    }
    const fresh = this.state === 'waiting';
    if (!fresh && !this.pickDonor() && !this.checkpoint) {
      throw new Error('Everyone left this match, so it can\'t be joined any more.');
    }

    let p = token && this.players.get(token);
    const returning = !!p;
    if (p && p.connected && p.ws && p.ws !== ws) {
      try { p.ws.close(4000, 'replaced'); } catch { /* ignore */ }
    }

    const slot = this.freeSlotFor(token);
    if (slot < 0) {
      throw new Error('This match is full.');
    }

    if (!p) {
      token = randomHex(16);
      p = { token, slot, input: { ...PDNet.NEUTRAL_INPUT }, pendingFlags: 0, lastInputAt: 0, joinedAt: Date.now() };
      this.players.set(token, p);
    }

    p.ws = ws;
    p.name = String(name || 'Agent').replace(/[^\x20-\x7e]/g, '').slice(0, 12) || 'Agent';
    p.slot = slot;
    p.connected = true;
    p.live = false;
    p.connectedAt = Date.now();
    this.slots[slot] = p;
    this.reserved[slot] = null;

    if (fresh) {
      this.startClock();
    }

    // the player appears (and gets the name) a couple of ticks from now, on every machine; the
    // joining player gets the name with their snapshot (or, starting the match, right after joining)
    const at = this.tick + 2;
    this.pendingEvents.set(at, (this.pendingEvents.get(at) || 0) | PDNet.EVENT_OCCUPY(slot));
    this.queueName(at, slot, p.name);
    p.live = fresh;

    this.log(`match ${this.id}: ${p.name} ${returning ? 'rejoined' : 'joined'} slot ${slot}${fresh ? ' (starts the match)' : ''}`);
    this.notice(`${p.name} has ${returning ? 're' : ''}joined`, p);
    this.onChange();
    return { player: p, fresh };
  }

  queueName(tick, slot, name) {
    this.names.push({ tick, slot, name });
    while (this.names.length && this.names[0].tick < this.tick - NAMES_KEPT_TICKS) {
      this.names.shift();
    }
    for (const other of this.players.values()) {
      if (other.live && other.connected) {
        this.sendJson(other, { type: 'name', tick, slot, name });
      }
    }
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
        this.reserved[p.slot] = { token: p.token, until: Date.now() + RECONNECT_GRACE_MS };
      } else {
        this.players.delete(p.token);
      }
      if (this.state === 'playing') {
        const at = this.tick + 1;
        this.pendingEvents.set(at, (this.pendingEvents.get(at) || 0) | PDNet.EVENT_VACATE(p.slot));
      }
    }

    // a snapshot this player was providing goes to someone else
    for (const [reqId, r] of this.requests) {
      if (r.donor === p) {
        this.requests.delete(reqId);
        if (r.kind === 'join') {
          this.provideSnapshot(r.target, r.reason, p);
        }
      }
    }

    if (this.humanCount === 0) {
      this.emptySince = Date.now();
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
    // ESC is one-shot: it must reach a tick even if another input follows before it, and must not
    // repeat while the input is repeated
    p.pendingFlags |= input.flags & PDNet.FLAG_ESC;
    p.input = input;
    p.lastInputAt = Date.now();
  }

  // ---------------------------------------------------------------------------
  // snapshots

  // a player whose game is running in step with the match, preferring the one here longest
  pickDonor(except) {
    let best = null;
    for (const p of this.players.values()) {
      if (p !== except && p.live && p.connected && !p.suspect && (!best || p.connectedAt < best.connectedAt)) {
        best = p;
      }
    }
    return best;
  }

  requestSnapshot(donor, info) {
    const reqId = this.nextReqId++;
    this.requests.set(reqId, { ...info, donor, at: Date.now() });
    this.sendJson(donor, { type: 'snapshot-request', reqId });
    return reqId;
  }

  /**
   * Gets the player into the match: a fresh snapshot from another player, or the latest checkpoint.
   */
  provideSnapshot(target, reason, exclude) {
    if (!target.connected || this.state !== 'playing') {
      return;
    }
    const donor = this.pickDonor(exclude || target);
    if (donor && donor !== target) {
      this.requestSnapshot(donor, { kind: 'join', target, reason });
      return;
    }
    if (this.checkpoint && this.history.length && this.history[0][0] <= this.checkpoint.tick + 1) {
      this.deliverSnapshot(target, this.checkpoint.tick, this.checkpoint.packed, `${reason}, checkpoint`);
      return;
    }
    this.sendJson(target, { type: 'error', message: 'Nobody in the match could send its state. Try again in a moment.' });
  }

  onSnapshotReply(donor, bytes) {
    if (bytes.length < 9) {
      return;
    }
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    const reqId = view.getUint32(1, true);
    const tick = view.getUint32(5, true);
    const r = this.requests.get(reqId);
    if (!r || r.donor !== donor) {
      return;
    }
    this.requests.delete(reqId);
    const packed = bytes.slice(9);

    // every snapshot is also the newest checkpoint
    if (!this.checkpoint || tick === 0xffffffff || tick >= this.checkpoint.tick) {
      this.checkpoint = { tick, packed };
    }
    if (r.kind === 'join') {
      this.deliverSnapshot(r.target, tick, packed, `${r.reason}, from ${donor.name}`);
    }
  }

  onSnapshotFailed(donor, reqId) {
    const r = this.requests.get(reqId);
    if (r && r.donor === donor) {
      this.requests.delete(reqId);
      if (r.kind === 'join') {
        this.provideSnapshot(r.target, r.reason, donor);
      }
    }
  }

  deliverSnapshot(p, tick, packed, reason) {
    if (!p.connected) {
      return;
    }
    const after = tick === 0xffffffff ? -1 : tick;
    this.sendNamesAfter(p, after);
    const msg = new Uint8Array(5 + packed.length);
    msg[0] = MSG_SNAPSHOT;
    new DataView(msg.buffer).setUint32(1, tick, true);
    msg.set(packed, 5);
    this.sendRaw(p, msg);
    for (const [t, packet] of this.history) {
      if (t > after) {
        this.sendRaw(p, packet);
      }
    }
    p.live = true;
    p.suspect = false;
    this.log(`match ${this.id}: snapshot for ${p.name} (${reason}) at tick ${after}, ${(packed.length / 1048576).toFixed(2)} MiB`);
  }

  // names for ticks after the given one, sent before those ticks so they're queued in time
  sendNamesAfter(p, after) {
    for (const n of this.names) {
      if (n.tick > after) {
        this.sendJson(p, { type: 'name', tick: n.tick, slot: n.slot, name: n.name });
      }
    }
  }

  // ---------------------------------------------------------------------------
  // keeping players in sync

  onClientHash(p, tick, hash) {
    if (!p.live) {
      return;
    }
    let reports = this.hashReports.get(tick);
    if (!reports) {
      reports = new Map();
      this.hashReports.set(tick, reports);
      if (this.hashReports.size > 20) {
        this.hashReports.delete(this.hashReports.keys().next().value);
      }
    }
    reports.set(p, hash >>> 0);

    const live = [...this.players.values()].filter((o) => o.live && o.connected);
    if (reports.size < Math.min(live.length, 2)) {
      return; // wait for someone to compare with
    }

    // the majority hash; on a tie, the one of the player here longest
    const counts = new Map();
    for (const [player, h] of reports) {
      const c = counts.get(h) || { n: 0, since: Infinity };
      c.n++;
      c.since = Math.min(c.since, player.connectedAt);
      counts.set(h, c);
    }
    if (counts.size === 1) {
      this.stats.checks++;
      if (this.stats.checks % 50 === 0) {
        this.log(`match ${this.id}: ${this.stats.checks} state checks agreed, ${this.stats.desyncs} desyncs`);
      }
      return;
    }
    const [truth] = [...counts.entries()].sort((a, b) => b[1].n - a[1].n || a[1].since - b[1].since)[0];
    for (const [player, h] of reports) {
      if (h !== truth && player.live) {
        this.stats.desyncs++;
        this.log(`match ${this.id}: DESYNC for ${player.name} at tick ${tick}; resyncing`);
        player.live = false;
        player.suspect = true;
        this.sendJson(player, { type: 'resync' });
        this.provideSnapshot(player, 'resync');
      }
    }
  }

  // ---------------------------------------------------------------------------
  // end

  onOver(p, results) {
    if (this.state !== 'playing' || !p.live) {
      return;
    }
    const clean = (Array.isArray(results) ? results : []).slice(0, 16).map((r) => ({
      name: String((r && r.name) || '').replace(/[^\x20-\x7e]/g, '').slice(0, 16),
      bot: !!(r && r.bot),
      kills: r.kills | 0,
      suicides: r.suicides | 0,
      deaths: r.deaths | 0,
    }));
    this.finish(clean);
  }

  finish(results) {
    if (this.state !== 'playing') {
      return;
    }
    this.state = 'ended';
    clearInterval(this.timer);
    this.timer = null;
    this.results = results;
    this.log(`match ${this.id} ended at tick ${this.tick}`);
    for (const p of this.players.values()) {
      if (p.connected) {
        this.sendJson(p, { type: 'ended', results });
      }
    }
    this.onEnded(results);
    this.onChange();
  }

  // ---------------------------------------------------------------------------

  notice(text, except) {
    for (const other of this.players.values()) {
      if (other !== except && other.connected) {
        this.sendJson(other, { type: 'notice', text });
      }
    }
  }

  sendJson(p, obj) {
    this.sendRaw(p, JSON.stringify(obj));
  }

  sendRaw(p, data) {
    try {
      if (p.ws && p.ws.readyState === 1) {
        p.ws.send(data);
      }
    } catch {
      // closed under us; the close event cleans up
    }
  }
}
