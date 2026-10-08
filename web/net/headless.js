// Boots the game (build-web/pd.js + pd.wasm) in Node without video, audio or input, as the
// server's authoritative instance of a match. See port/src/net.c.
'use strict';

const fs = require('node:fs');
const path = require('node:path');
const PDNet = require('./nethost.js');

/**
 * @param {object} opts
 * @param {string} opts.buildDir   directory containing pd.js and pd.wasm
 * @param {Buffer} opts.rom        the NTSC v1.1 ROM
 * @param {object} opts.match      match config (see PDNet.matchConfigText)
 * @param {function} [opts.onHash] (tick, hash) every 60 ticks
 * @param {function} [opts.log]    log lines from the game
 * @returns {Promise<{module, host}>}
 */
async function startHeadless(opts) {
  const createPerfectDark = require(path.join(opts.buildDir, 'pd.js'));
  const log = opts.log || (() => {});

  const config = {
    arguments: ['--basedir', '/data', '--savedir', '/save', '--net-match', '/net/match.cfg', '--headless'],
    // note: no --no-sound; it changes game logic, and players' browsers run with sound
    locateFile: (p) => path.join(opts.buildDir, p),
    print: (t) => log(t),
    printErr: (t) => log(t),
    onNetHash: (tick, hash) => opts.onHash && opts.onHash(tick, hash),
    onFatalError: (t) => log('FATAL: ' + t),
    preRun: [
      (m) => {
        m.FS.mkdir('/data');
        m.FS.writeFile('/data/pd.ntsc-final.z64', opts.rom);
        m.FS.mkdir('/save');
        m.FS.mkdir('/net');
        m.FS.writeFile('/net/match.cfg', PDNet.matchConfigText(opts.match));
      },
    ],
  };

  const module = await createPerfectDark(config);
  const host = PDNet.attach(module);
  return { module, host };
}

module.exports = { startHeadless };

// Command line: run a match with scripted inputs and print the hashes (determinism testing)
//   node web/net/headless.js <rom> <ticks> [seed] [--record <dir>]
// --record writes the match config (match.cfg), every tick packet (ticks.bin) and the hashes
// (hashes.txt) to <dir>, so another host can replay the same match and compare (see native_host/).
if (require.main === module) {
  (async () => {
    const args = process.argv.slice(2);
    const recIdx = args.indexOf('--record');
    const recordDir = recIdx >= 0 ? args.splice(recIdx, 2)[1] : null;
    const [romPath, ticksArg, seedArg] = args;
    const ticks = parseInt(ticksArg || '600', 10);
    const seed = parseInt(seedArg || '1234', 10);
    const buildDir = path.resolve(__dirname, '..', '..', 'build-web');
    const match = {
      seed,
      stage: 0x32,
      scenario: 0,
      timelimit: 9,
      slots: [{ name: 'One' }, { name: 'Two' }, { name: 'Three' }, { name: 'Four' }],
      bots: [{ difficulty: 2 }, { difficulty: 3 }],
    };

    const started = Date.now();
    const hashes = [];
    const { host } = await startHeadless({
      buildDir,
      rom: fs.readFileSync(romPath),
      match,
      log: (t) => { if (/FATAL|ERROR|net:/.test(t)) console.error(t); },
      onHash: (tick, hash) => hashes.push(`${tick} ${hash.toString(16).padStart(8, '0')}`),
    });

    const packets = [];
    for (let t = 0; t < ticks; t++) {
      const inputs = [0, 1, 2, 3].map((slot) => PDNet.scriptedInput(seed, slot, t));
      host.push(t, 0, inputs);
      if (recordDir) {
        packets.push(new Uint8Array(PDNet.encodeTick(t, 0, inputs)));
      }
      // let the game run what was pushed
      while (host.queued() > 0) {
        await new Promise((r) => setImmediate(r));
      }
    }

    const secs = (Date.now() - started) / 1000;
    if (recordDir) {
      fs.mkdirSync(recordDir, { recursive: true });
      fs.writeFileSync(path.join(recordDir, 'match.cfg'), PDNet.matchConfigText(match));
      fs.writeFileSync(path.join(recordDir, 'ticks.bin'), Buffer.concat(packets));
      fs.writeFileSync(path.join(recordDir, 'hashes.txt'), hashes.join('\n') + '\n');
    }
    console.log(hashes.join('\n'));
    console.error(`${ticks} ticks in ${secs.toFixed(1)}s (${(ticks / secs).toFixed(0)} ticks/s)`);
    process.exit(0);
  })().catch((e) => {
    console.error(e);
    process.exit(1);
  });
}
