#!/usr/bin/env node
// Headless test player: joins (or creates) an online match like the browser client does, but runs
// the game without graphics in Node and plays with scripted inputs. Reports the state checks it
// sends, so it can be used to test a server from any machine (eg. one too slow for a browser).
//
//   node web/net/testplayer.js <server url> <rom> [--name Bot] [--create] [--seconds 60] [--room <id>]
//
// Needs the same build as the server (build-web next to this repo, or --build <dir>).
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const PDNet = require('./nethost.js');
const { startHeadless } = require('./headless.js');

const MSG_TICK = 1;
const MSG_SNAPSHOT = 2;
const MSG_INPUT = 3;

function opt(name, def) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 ? process.argv[i + 1] : def;
}

const [serverUrl, romPath] = process.argv.slice(2);
if (!serverUrl || !romPath) {
  console.error('usage: testplayer.js <server url> <rom> [--name Bot] [--create] [--seconds 60] [--room <id>]');
  process.exit(1);
}

const NAME = opt('name', 'TestBot');
const SECONDS = parseInt(opt('seconds', '60'), 10);
const BUILD_DIR = path.resolve(opt('build', path.join(__dirname, '..', '..', 'build-web')));
const CREATE = process.argv.includes('--create');
const ROOM = opt('room');
const rom = fs.readFileSync(romPath);
const log = (m) => console.log(`${new Date().toISOString().slice(11, 19)} [${NAME}] ${m}`);

(async () => {
  const config = await (await fetch(new URL('server-config.json', serverUrl))).json();
  const wsUrl = new URL('net', serverUrl);
  wsUrl.protocol = wsUrl.protocol === 'https:' ? 'wss:' : 'ws:';
  const ws = new WebSocket(wsUrl);
  ws.binaryType = 'arraybuffer';

  let inst = null;
  let slot = -1;
  let lastTick = -1;
  let snapshot = null;
  let buffer = [];
  const names = [];
  let hashesSent = 0;
  let ticksRun = 0;
  let joinedAt = 0;
  const send = (o) => ws.send(JSON.stringify(o));

  function pushPacket(packet) {
    const tick = new DataView(packet.buffer, packet.byteOffset, 4).getUint32(0, true);
    if (tick <= lastTick) {
      return;
    }
    for (let i = names.length - 1; i >= 0; i--) {
      if (names[i].tick <= tick) {
        const n = names.splice(i, 1)[0];
        const buf = inst.module._netGetNameBuffer();
        inst.module.HEAPU8.fill(0, buf, buf + 32);
        inst.module.HEAPU8.set(Buffer.from(n.name.slice(0, 12), 'latin1'), buf);
        inst.module._netQueueName(n.tick, n.slot);
      }
    }
    lastTick = tick;
    inst.host.pushRaw(packet);

    // play: send our scripted input for (roughly) this tick
    const input = PDNet.scriptedInput(slot * 7919 + 13, slot, tick);
    const msg = new Uint8Array(1 + PDNet.INPUT_SIZE);
    msg[0] = MSG_INPUT;
    PDNet.writeInput(new DataView(msg.buffer), 1, input);
    ws.send(msg);
  }

  ws.onopen = () => {
    send({ type: 'hello', name: NAME, clientId: `testplayer-${NAME}`, build: config.build });
  };

  ws.onmessage = async (ev) => {
    if (typeof ev.data !== 'string') {
      const bytes = new Uint8Array(ev.data);
      if (bytes[0] === MSG_TICK) {
        const packet = bytes.slice(1);
        if (!inst || snapshot !== 'done') {
          buffer.push(packet);
        } else {
          pushPacket(packet);
        }
      } else if (bytes[0] === MSG_SNAPSHOT) {
        snapshot = bytes.slice(5);
        if (inst) {
          applySnapshot();
        }
      }
      return;
    }

    const msg = JSON.parse(ev.data);
    if (msg.type === 'welcome') {
      if (!msg.enabled) {
        log('server cannot host matches');
        process.exit(1);
      }
      if (CREATE) {
        send({ type: 'create', settings: { name: `${NAME}'s match`, stage: 0x32, bots: 1, timelimit: 10 } });
      }
    } else if (msg.type === 'rooms' && !CREATE && slot < 0 && !joinedAt) {
      const room = msg.rooms.find((r) => (!ROOM || r.id === ROOM) && r.state === 'playing' && r.players.filter(Boolean).length < 4);
      if (room) {
        joinedAt = Date.now();
        log(`joining "${room.name}" (${room.id})`);
        send({ type: 'join', roomId: room.id });
      }
    } else if (msg.type === 'joined') {
      slot = msg.slot;
      joinedAt = Date.now();
      log(`joined ${msg.roomId} in slot ${slot}; starting the game`);
      inst = await startHeadless({
        buildDir: BUILD_DIR,
        rom,
        match: msg.match,
        log: (t) => { if (/FATAL|ERROR/.test(t)) log(t); },
        onHash: (tick, hash) => {
          ticksRun = tick;
          if (snapshot === 'done') {
            send({ type: 'hash', tick, hash });
            hashesSent++;
          }
        },
      });
      if (snapshot) {
        applySnapshot();
      }
    } else if (msg.type === 'name') {
      names.push(msg);
    } else if (msg.type === 'resync') {
      log('server asked for a resync (desync detected)');
      snapshot = null;
      buffer = [];
    } else if (msg.type === 'ended') {
      log(`match over: ${msg.results.map((r) => `${r.name} ${r.kills}/${r.deaths}`).join(', ')}`);
      finish();
    } else if (msg.type === 'error') {
      log(`error: ${msg.message}`);
    }
  };

  function applySnapshot() {
    const raw = zlib.inflateRawSync(snapshot);
    inst.module._netClearQueue();
    lastTick = PDNet.restoreSnapshot(inst.module, raw);
    snapshot = 'done';
    log(`restored the match at tick ${lastTick} (${(raw.length / 1048576).toFixed(1)} MiB)`);
    const b = buffer;
    buffer = [];
    for (const p of b) {
      pushPacket(p);
    }
  }

  function finish() {
    log(`done: game at tick ${ticksRun}, ${hashesSent} state checks sent`);
    try { send({ type: 'leave' }); } catch { /* ignore */ }
    setTimeout(() => process.exit(0), 300);
  }

  setTimeout(finish, SECONDS * 1000);
  setInterval(() => {
    if (snapshot === 'done') {
      log(`tick ${lastTick}, game queue ${inst.host.queued()}, ${hashesSent} state checks sent`);
    }
  }, 15000).unref();
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
