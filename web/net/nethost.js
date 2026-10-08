// Shared netplay glue between JavaScript and the game (port/src/net.c).
// Used by the browser client (web/net-client.js) and the server's match worker (web/net/match-worker.js).
//
// Works as a CommonJS module in Node and as a global (window.PDNet) in the browser.
'use strict';

(function (root, factory) {
  if (typeof module === 'object' && module.exports) {
    module.exports = factory();
  } else {
    root.PDNet = factory();
  }
})(typeof self !== 'undefined' ? self : this, function () {
  const NUM_SLOTS = 4;

  // struct netinput (port/include/net.h)
  const INPUT_SIZE = 28;
  const FLAG_ESC = 0x0001;
  const FLAG_FREEAIM = 0x0002;

  const NEUTRAL_INPUT = Object.freeze({ buttons: 0, sx: 0, sy: 0, rsx: 0, rsy: 0, aimx: 0, aimy: 0, mdx: 0, mdy: 0, flags: 0 });

  const EVENT_OCCUPY = (slot) => 1 << slot;
  const EVENT_VACATE = (slot) => 1 << (slot + 8);

  function writeInput(view, offset, input) {
    view.setUint32(offset, input.buttons >>> 0, true);
    view.setInt8(offset + 4, input.sx | 0);
    view.setInt8(offset + 5, input.sy | 0);
    view.setInt8(offset + 6, input.rsx | 0);
    view.setInt8(offset + 7, input.rsy | 0);
    view.setFloat32(offset + 8, input.aimx, true);
    view.setFloat32(offset + 12, input.aimy, true);
    view.setFloat32(offset + 16, input.mdx, true);
    view.setFloat32(offset + 20, input.mdy, true);
    view.setUint32(offset + 24, input.flags >>> 0, true);
  }

  function readInput(view, offset) {
    return {
      buttons: view.getUint32(offset, true),
      sx: view.getInt8(offset + 4),
      sy: view.getInt8(offset + 5),
      rsx: view.getInt8(offset + 6),
      rsy: view.getInt8(offset + 7),
      aimx: view.getFloat32(offset + 8, true),
      aimy: view.getFloat32(offset + 12, true),
      mdx: view.getFloat32(offset + 16, true),
      mdy: view.getFloat32(offset + 20, true),
      flags: view.getUint32(offset + 24, true),
    };
  }

  // Tick packet on the wire: u32 tick, u32 events, 4 x netinput (little endian)
  const TICK_PACKET_SIZE = 8 + NUM_SLOTS * INPUT_SIZE;

  function encodeTick(tick, events, inputs, out, outOffset) {
    const buf = out || new ArrayBuffer(TICK_PACKET_SIZE);
    const view = new DataView(buf, outOffset || 0, TICK_PACKET_SIZE);
    view.setUint32(0, tick >>> 0, true);
    view.setUint32(4, events >>> 0, true);
    for (let i = 0; i < NUM_SLOTS; i++) {
      writeInput(view, 8 + i * INPUT_SIZE, inputs[i] || NEUTRAL_INPUT);
    }
    return buf;
  }

  function decodeTick(view, offset) {
    const inputs = [];
    for (let i = 0; i < NUM_SLOTS; i++) {
      inputs.push(readInput(view, offset + 8 + i * INPUT_SIZE));
    }
    return { tick: view.getUint32(offset, true), events: view.getUint32(offset + 4, true), inputs };
  }

  /**
   * Connects a game instance (an Emscripten module created with --net-match) to the host.
   * Returns { push(tick, events, inputs), pushRaw(bytes), queued() }.
   * Hooks Module.netWaitForTick, which the game awaits when it has no tick to run.
   */
  function attach(Module) {
    let waiters = [];

    if (Module._netGetSizeofInput() !== INPUT_SIZE) {
      throw new Error(`netinput size mismatch: game ${Module._netGetSizeofInput()} vs js ${INPUT_SIZE}`);
    }

    Module.netWaitForTick = () => new Promise((resolve) => waiters.push(resolve));

    function wake() {
      const w = waiters;
      waiters = [];
      for (const resolve of w) {
        resolve();
      }
    }

    function push(tick, events, inputs) {
      const ptr = Module._netGetPushBuffer();
      const view = new DataView(Module.HEAPU8.buffer);
      for (let i = 0; i < NUM_SLOTS; i++) {
        writeInput(view, ptr + i * INPUT_SIZE, inputs[i] || NEUTRAL_INPUT);
      }
      if (!Module._netPushTick(tick, events)) {
        throw new Error('game tick queue is full');
      }
      wake();
    }

    // bytes: one encoded tick packet (Uint8Array or ArrayBuffer view)
    function pushRaw(bytes) {
      const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
      const t = decodeTick(view, 0);
      push(t.tick, t.events, t.inputs);
      return t.tick;
    }

    return {
      push,
      pushRaw,
      queued: () => Module._netGetQueuedTicks(),
      ticksRun: () => Module._netGetTicksRun(),
      // the local player's input captured by the game after each tick (see netEndTick)
      readInput: (ptr) => readInput(new DataView(Module.HEAPU8.buffer), ptr),
    };
  }

  // ---------------------------------------------------------------------------
  // Snapshots: the game's global state (ranges from pd.snap.json, made at build time from the
  // linker map) plus the simulation arena (port/src/simarena.c), minus the ROM image inside it,
  // which is identical on every machine. Only valid between instances of the same pd.wasm.

  const SNAP_MAGIC = 0x31535044; // 'PDS1'

  function snapshotRanges(Module, gameRanges) {
    const infoPtr = Module._netGetSnapshotInfo();
    const info = new Uint32Array(Module.HEAPU8.buffer, infoPtr, 5);
    const [arenaBase, highWater, romStart, romSize, tick] = Array.from(info);
    const arenaEnd = arenaBase + highWater;
    const ranges = gameRanges.map((r) => [r[0], r[1]]);

    if (romStart >= arenaBase && romStart + romSize <= arenaEnd) {
      ranges.push([arenaBase, romStart - arenaBase]);
      ranges.push([romStart + romSize, arenaEnd - (romStart + romSize)]);
    } else {
      ranges.push([arenaBase, highWater]);
    }

    // 0xffffffff: no tick has run yet; -1 in JS so that every tick counts as "after" the snapshot
    return { ranges: ranges.filter((r) => r[1] > 0), tick: tick === 0xffffffff ? -1 : tick, highWater };
  }

  function makeSnapshot(Module, gameRanges) {
    const { ranges, tick, highWater } = snapshotRanges(Module, gameRanges);
    const headerSize = 16 + ranges.length * 8;
    const dataSize = ranges.reduce((n, r) => n + r[1], 0);
    const out = new Uint8Array(headerSize + dataSize);
    const view = new DataView(out.buffer);
    const heap = Module.HEAPU8;

    view.setUint32(0, SNAP_MAGIC, true);
    view.setUint32(4, tick >>> 0, true);
    view.setUint32(8, highWater, true);
    view.setUint32(12, ranges.length, true);

    let pos = headerSize;
    ranges.forEach(([start, len], i) => {
      view.setUint32(16 + i * 8, start, true);
      view.setUint32(20 + i * 8, len, true);
      out.set(heap.subarray(start, start + len), pos);
      pos += len;
    });

    return { bytes: out, tick };
  }

  // returns the tick the snapshot was taken after (-1: before the first); the next tick to push is tick + 1
  function restoreSnapshot(Module, bytes) {
    const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
    if (view.getUint32(0, true) !== SNAP_MAGIC) {
      throw new Error('not a game snapshot');
    }
    const tick = view.getUint32(4, true);
    const highWater = view.getUint32(8, true);
    const count = view.getUint32(12, true);
    const heap = Module.HEAPU8;
    let pos = 16 + count * 8;

    for (let i = 0; i < count; i++) {
      const start = view.getUint32(16 + i * 8, true);
      const len = view.getUint32(20 + i * 8, true);
      heap.set(bytes.subarray(pos, pos + len), start);
      pos += len;
    }

    Module._netAfterRestore(tick, highWater);
    return tick === 0xffffffff ? -1 : tick;
  }

  /**
   * Match config file contents for --net-match (port/src/net.c netConfigureMatch).
   */
  function matchConfigText(cfg) {
    const lines = [];
    const put = (k, v) => {
      if (v !== undefined && v !== null) {
        lines.push(`${k}=${v}`);
      }
    };
    put('seed', cfg.seed);
    put('stage', cfg.stage);
    put('scenario', cfg.scenario);
    put('timelimit', cfg.timelimit);
    put('scorelimit', cfg.scorelimit);
    put('teamscorelimit', cfg.teamscorelimit);
    put('options', cfg.options);
    put('weaponset', cfg.weaponset);
    // bit per player slot with someone in it when the match starts; the rest are parked (port/src/netsim.c)
    put('occupied', cfg.occupied);
    (cfg.slots || []).forEach((s, i) => {
      if (!s) {
        return;
      }
      put(`slot${i}.name`, s.name ? String(s.name).replace(/[\r\n=]/g, '').slice(0, 12) : undefined);
      put(`slot${i}.body`, s.body);
      put(`slot${i}.head`, s.head);
      put(`slot${i}.team`, s.team);
      put(`slot${i}.occupied`, s.occupied ? 1 : 0);
    });
    put('bots', (cfg.bots || []).length);
    (cfg.bots || []).forEach((b, i) => {
      put(`bot${i}.type`, b.type);
      put(`bot${i}.difficulty`, b.difficulty);
      put(`bot${i}.team`, b.team);
    });
    return lines.join('\n') + '\n';
  }

  /**
   * Deterministic pseudo-random input for testing: the same (seed, slot, tick) always gives
   * the same input. Walks, turns, aims and fires in bursts.
   */
  function scriptedInput(seed, slot, tick) {
    let x = (seed ^ (slot * 0x9e3779b1) ^ Math.floor(tick / 45) * 0x85ebca6b) >>> 0;
    const next = () => {
      x ^= x << 13; x >>>= 0;
      x ^= x >>> 17;
      x ^= x << 5; x >>>= 0;
      return x;
    };
    next(); next();
    const r = next();
    const fire = (next() % 3) === 0 && (tick % 45) > 20;
    return {
      buttons: (fire ? 0x2000 /* Z_TRIG */ : 0) | ((tick % 600) === 300 ? 0x8000 /* A: respawn */ : 0),
      sx: ((r & 0xff) % 101) - 50,
      sy: (((r >>> 8) & 0xff) % 81) - 10,
      rsx: 0,
      rsy: 0,
      aimx: (((r >>> 16) & 0xff) / 255) * 1.6 - 0.8,
      aimy: (((r >>> 24) & 0xff) / 255) * 0.8 - 0.4,
      mdx: 0,
      mdy: 0,
      flags: FLAG_FREEAIM,
    };
  }

  return {
    NUM_SLOTS,
    INPUT_SIZE,
    TICK_PACKET_SIZE,
    FLAG_ESC,
    FLAG_FREEAIM,
    NEUTRAL_INPUT,
    EVENT_OCCUPY,
    EVENT_VACATE,
    writeInput,
    readInput,
    encodeTick,
    decodeTick,
    attach,
    makeSnapshot,
    restoreSnapshot,
    matchConfigText,
    scriptedInput,
  };
});
