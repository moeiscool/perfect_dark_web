#!/usr/bin/env node
// Netplay self-test (Node, headless):
//  1. determinism: two instances fed the same scripted inputs produce the same hashes
//  2. snapshots: an instance restored from a snapshot taken mid-match continues identically
//
//   node web/net/selftest.js <rom> [ticks=3600] [snapAt=1800] [seed=1234]
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const PDNet = require('./nethost.js');
const { startHeadless } = require('./headless.js');

const buildDir = path.resolve(__dirname, '..', '..', 'build-web');

async function runTicks(inst, from, to, seed) {
  for (let t = from; t < to; t++) {
    inst.host.push(t, 0, [0, 1, 2, 3].map((s) => PDNet.scriptedInput(seed, s, t)));
    while (inst.host.queued() > 0) {
      await new Promise((r) => setImmediate(r));
    }
  }
}

async function boot(rom, match, hashes) {
  return startHeadless({
    buildDir,
    rom,
    match,
    log: (t) => { if (/FATAL|ERROR/.test(t)) console.error(t); },
    onHash: (tick, hash) => hashes.set(tick, hash),
  });
}

(async () => {
  const [romPath, ticksArg, snapArg, seedArg] = process.argv.slice(2);
  const TICKS = parseInt(ticksArg || '3600', 10);
  const SNAP_AT = parseInt(snapArg || '1800', 10);
  const SEED = parseInt(seedArg || '1234', 10);
  const rom = fs.readFileSync(romPath);
  const { ranges } = JSON.parse(fs.readFileSync(path.join(buildDir, 'pd.snap.json'), 'utf8'));
  const match = {
    seed: SEED, stage: 0x32, scenario: 0, timelimit: 9,
    slots: [{ name: 'One' }, { name: 'Two' }, { name: 'Three' }, { name: 'Four' }],
    bots: [{ difficulty: 2 }, { difficulty: 3 }],
  };

  const hashesA = new Map();
  const hashesB = new Map();
  const a = await boot(rom, match, hashesA);

  await runTicks(a, 0, SNAP_AT, SEED);

  let t0 = Date.now();
  const snap = PDNet.makeSnapshot(a.module, ranges);
  const packed = zlib.deflateRawSync(snap.bytes, { level: 6 });
  console.log(`snapshot after tick ${snap.tick}: ${(snap.bytes.length / 1048576).toFixed(1)} MiB raw, ` +
    `${(packed.length / 1048576).toFixed(2)} MiB compressed, ${Date.now() - t0} ms`);

  // a second instance, booted to the start of the same match, then given the snapshot
  const b = await boot(rom, match, hashesB);
  t0 = Date.now();
  const restoredTick = PDNet.restoreSnapshot(b.module, zlib.inflateRawSync(packed));
  console.log(`restored at tick ${restoredTick} in ${Date.now() - t0} ms`);

  await runTicks(a, SNAP_AT, TICKS, SEED);
  await runTicks(b, SNAP_AT, TICKS, SEED);

  let checked = 0;
  let mismatches = 0;
  for (const [tick, hash] of hashesB) {
    if (tick < SNAP_AT) {
      continue;
    }
    checked++;
    if (hashesA.get(tick) !== hash) {
      mismatches++;
      if (mismatches <= 5) {
        console.log(`MISMATCH at tick ${tick}: A ${hashesA.get(tick)?.toString(16)} B ${hash.toString(16)}`);
      }
    }
  }

  console.log(`${checked} hashes after the snapshot compared, ${mismatches} mismatches`);
  console.log(mismatches === 0 && checked > 0 ? 'SNAPSHOT TEST PASSED' : 'SNAPSHOT TEST FAILED');
  process.exit(mismatches === 0 && checked > 0 ? 0 : 1);
})().catch((e) => {
  console.error(e);
  process.exit(1);
});
