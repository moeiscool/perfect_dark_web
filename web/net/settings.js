// Match settings shared by the lobby servers: web/net/lobby.js (Node) and the Cloudflare Workers
// relay (cloudlare_worker_server/). No Node-only dependencies.
'use strict';

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

function randomInt(n) {
  const buf = new Uint32Array(1);
  globalThis.crypto.getRandomValues(buf);
  return buf[0] % n;
}

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
    stage = ids[randomInt(ids.length)];
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

// the --net-match config every machine in the match starts from (port/src/net.c netConfigureMatch)
function makeMatchConfig(s) {
  return {
    seed: 1 + randomInt(0x7ffffffe),
    stage: s.stage,
    scenario: s.scenario,
    timelimit: s.timelimit - 1,
    scorelimit: s.scorelimit ? s.scorelimit - 1 : 100,
    teamscorelimit: 400,
    options: s.options,
    weaponset: s.weaponset,
    occupied: 0,
    slots: [0, 1, 2, 3].map((i) => ({ name: `Open ${i + 1}`, team: s.teams ? i % 2 : i })),
    bots: Array.from({ length: s.bots }, (_, i) => ({ difficulty: s.botDifficulty, team: s.teams ? i % 2 : undefined })),
  };
}

module.exports = { ARENAS, SCENARIOS, WEAPON_SETS, validateSettings, makeMatchConfig };
