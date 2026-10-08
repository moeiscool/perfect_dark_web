#!/usr/bin/env node
// Build step: reads the wasm-ld map (build-web/pd.map) and writes pd.snap.json, the memory ranges
// of the game's global state. A netplay snapshot is these ranges plus the simulation arena
// (port/src/simarena.c); see web/net/nethost.js makeSnapshot/restoreSnapshot.
//
//   node web/net/snapranges.js pd.map pd.snap.json
'use strict';

const fs = require('node:fs');

// object files whose globals are game state
const INCLUDE = [
  /\/src\/game\//,
  /\/src\/lib\//,
  /\/port\/src\/preprocess\//,
  /\/port\/src\/(romdata|libultra|main|pdmain|pdsched|mixer|netsim)\.c\.o$/,
];

const [mapPath, outPath] = process.argv.slice(2);
if (!mapPath || !outPath) {
  console.error('usage: snapranges.js <pd.map> <out.json>');
  process.exit(1);
}

const ranges = [];
let total = 0;
const lineRe = /^\s*([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)\s+(\S.*):\((\.(?:data|bss)\b[^)]*)\)\s*$/;

for (const line of fs.readFileSync(mapPath, 'utf8').split('\n')) {
  const m = lineRe.exec(line);
  if (!m) {
    continue;
  }
  const [, addrHex, sizeHex, obj] = m;
  if (!INCLUDE.some((re) => re.test(obj))) {
    continue;
  }
  const start = parseInt(addrHex, 16);
  const len = parseInt(sizeHex, 16);
  if (len > 0) {
    ranges.push([start, len]);
    total += len;
  }
}

if (!ranges.length) {
  console.error('snapranges: no game data found in the map; is the build linked with --Map?');
  process.exit(1);
}

// merge ranges that touch; gaps are left out, since they may hold another file's globals
ranges.sort((a, b) => a[0] - b[0]);
const merged = [];
for (const [start, len] of ranges) {
  const last = merged[merged.length - 1];
  if (last && start <= last[0] + last[1]) {
    last[1] = Math.max(last[1], start + len - last[0]);
  } else {
    merged.push([start, len]);
  }
}

fs.writeFileSync(outPath, JSON.stringify({ ranges: merged }));
const mergedTotal = merged.reduce((n, r) => n + r[1], 0);
console.log(`snapranges: ${ranges.length} globals, ${merged.length} ranges, ${(mergedTotal / 1024).toFixed(0)} KiB (${(total / 1024).toFixed(0)} KiB exact)`);
