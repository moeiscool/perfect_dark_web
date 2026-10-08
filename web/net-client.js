// Online multiplayer client: lobby UI, and running an online match in this browser.
// Talks to the server's lobby (web/net/lobby.js) over a WebSocket at /net.
// Needs nethost.js (window.PDNet) and pd-web.js (window.PDWeb) loaded first.
(() => {
  'use strict';

  const MSG_TICK = 1;
  const MSG_SNAPSHOT = 2;
  const MSG_INPUT = 3;
  const SESSION_KEY = 'pd-online-session';
  const NAME_KEY = 'pd-online-name';
  const CLIENT_KEY = 'pd-online-client';
  const SESSION_MAX_AGE_MS = 60 * 1000; // matches the server's reconnect grace period
  const SESSION_ORPHAN_MS = 15 * 1000;   // a session whose tab hasn't checked in this long was closed
  const TAB_KEY = 'pd-online-tab';

  const $ = (id) => document.getElementById(id);
  const ui = {
    panel: $('online'),
    status: $('online-status'),
    name: $('online-name'),
    rooms: $('online-rooms'),
    empty: $('online-empty'),
    createToggle: $('online-create-toggle'),
    createForm: $('online-create'),
    rejoin: $('online-rejoin'),
    rejoinText: $('online-rejoin-text'),
    hud: $('net-hud'),
    hudText: $('net-hud-text'),
    leave: $('net-leave'),
    results: $('net-results'),
    resultsBody: $('net-results-body'),
    resultsBack: $('net-results-back'),
    hint: $('net-hint'),
  };

  let ws = null;
  let server = null;      // { build, enabled, arenas, scenarios, weaponSets }
  let config = null;      // /server-config.json
  let session = null;     // { roomId, token, slot, at }
  let match = null;       // running match state, see startMatch
  let reconnectTimer = 0;
  let pingTimer = 0;
  let lastPing = 0;
  let rtt = 0;
  let rejoinCandidate = null; // session offered by the Rejoin button
  let autoRejoin = null;      // session to rejoin right away (after a graphics reset)
  let lastSessionSave = 0;
  const AUTOREJOIN_KEY = 'pd-online-autorejoin';

  const store = {
    get(key) { try { return localStorage.getItem(key); } catch { return null; } },
    set(key, v) { try { localStorage.setItem(key, v); } catch { /* ignore */ } },
    del(key) { try { localStorage.removeItem(key); } catch { /* ignore */ } },
  };

  function clientId() {
    let id = store.get(CLIENT_KEY);
    if (!id) {
      id = Array.from(crypto.getRandomValues(new Uint8Array(12)), (b) => b.toString(16).padStart(2, '0')).join('');
      store.set(CLIENT_KEY, id);
    }
    return id;
  }

  // identifies this tab (survives reloads, not shared with other tabs), so two tabs in one browser
  // don't take over each other's place in a match
  function tabId() {
    let id = null;
    try { id = sessionStorage.getItem(TAB_KEY); } catch { /* ignore */ }
    if (!id) {
      id = Math.random().toString(36).slice(2);
      try { sessionStorage.setItem(TAB_KEY, id); } catch { /* ignore */ }
    }
    return id;
  }

  // a saved session this tab may use: its own (eg. after a reload), or one whose tab was closed
  function usableSession() {
    const own = readSession(sessionKey());
    if (own) {
      return own;
    }
    let keys = [];
    try {
      keys = Object.keys(localStorage).filter((k) => k.startsWith(`${SESSION_KEY}:`));
    } catch {
      return null;
    }
    for (const key of keys) {
      const s = readSession(key);
      if (s && Date.now() - s.at > SESSION_ORPHAN_MS) {
        store.del(key); // this tab takes it over
        return s;
      }
    }
    return null;
  }

  function playerName() {
    return (ui.name.value || '').trim().slice(0, 12) || 'Agent';
  }

  function setStatus(text, isError) {
    ui.status.textContent = text;
    ui.status.classList.toggle('warn', !!isError);
  }

  // sessions are stored per tab (key pd-online-session:<tab>), so several tabs don't overwrite each other
  function sessionKey(tab) {
    return `${SESSION_KEY}:${tab || tabId()}`;
  }

  function saveSession() {
    if (session && match && !match.ended) {
      session.at = Date.now();
      session.tab = tabId();
      store.set(sessionKey(), JSON.stringify(session));
    }
  }

  function clearSession() {
    session = null;
    store.del(sessionKey());
  }

  function readSession(key) {
    try {
      const s = JSON.parse(store.get(key) || 'null');
      if (s && Date.now() - s.at < SESSION_MAX_AGE_MS) {
        return s;
      }
      store.del(key); // expired
    } catch {
      // ignore
    }
    return null;
  }

  // ---------------------------------------------------------------------------
  // connection

  function connect() {
    clearTimeout(reconnectTimer);
    const url = `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/net`;
    ws = new WebSocket(url);
    ws.binaryType = 'arraybuffer';

    ws.onopen = () => {
      send({ type: 'hello', name: playerName(), clientId: clientId(), build: config && config.build });
      clearInterval(pingTimer);
      pingTimer = setInterval(() => {
        lastPing = performance.now();
        send({ type: 'ping', t: lastPing });
      }, 2000);
      // back into a match after a dropped connection
      if (match && session) {
        send({ type: 'join', roomId: session.roomId, token: session.token });
      }
    };

    ws.onmessage = (ev) => {
      if (typeof ev.data === 'string') {
        onJson(JSON.parse(ev.data));
      } else {
        onBinary(new Uint8Array(ev.data));
      }
    };

    ws.onclose = (ev) => {
      clearInterval(pingTimer);
      if (ev.code === 4000) {
        // this player joined again from another tab or window
        if (match) {
          match.live = false;
          match.ended = true;
        }
        hud('You joined this match from another tab, so this one stopped.');
        setStatus('You joined from another tab.', true);
        return;
      }
      if (match && !match.ended) {
        match.live = false;
        hud('Connection lost, reconnecting…');
      } else {
        setStatus('Disconnected from the server, retrying…', true);
      }
      reconnectTimer = setTimeout(connect, 2000);
    };
  }

  function send(obj) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify(obj));
    }
  }

  function onJson(msg) {
    switch (msg.type) {
      case 'welcome':
        server = msg;
        if (!msg.enabled) {
          setStatus('This server can\'t host online matches right now.', true);
        } else {
          setStatus('Connected. Join a match or create one.');
        }
        fillCreateForm();
        tryAutoRejoin();
        break;
      case 'rooms':
        renderRooms(msg.rooms);
        break;
      case 'joined':
        onJoined(msg);
        break;
      case 'name':
        if (match) {
          match.names.push(msg);
        }
        break;
      case 'resync':
        if (match) {
          match.live = false;
          match.awaitingSnapshot = true;
          match.buffer = [];
          hud('Resynchronizing…');
        }
        break;
      case 'ended':
        onEnded(msg.results);
        break;
      case 'notice':
        showNotice(msg.text);
        break;
      case 'pong':
        rtt = performance.now() - msg.t;
        break;
      case 'left':
        break;
      case 'error':
        if (msg.code === 'build') {
          setStatus(msg.message, true);
          setTimeout(() => location.reload(), 1500);
        } else if (match && !match.module) {
          // failed to get into the match we were trying to start
          match = null;
          clearSession();
          document.body.classList.remove('is-online');
          setStatus(msg.message, true);
          showLobby();
        } else {
          setStatus(msg.message, true);
        }
        break;
      default:
        break;
    }
  }

  function onBinary(bytes) {
    if (!match) {
      return;
    }
    if (Date.now() - lastSessionSave > 5000) {
      lastSessionSave = Date.now();
      saveSession();
    }
    if (bytes[0] === MSG_TICK) {
      const packet = bytes.subarray(1);
      if (match.awaitingSnapshot || !match.host) {
        match.buffer.push(packet);
      } else {
        pushTick(packet);
      }
    } else if (bytes[0] === MSG_SNAPSHOT) {
      const tick = new DataView(bytes.buffer, bytes.byteOffset + 1, 4).getUint32(0, true);
      match.snapshot = { tick, packed: bytes.slice(5) };
      if (match.host) {
        applySnapshot().catch((e) => hud(`Could not load the match: ${e.message}`));
      }
    }
  }

  // ---------------------------------------------------------------------------
  // lobby UI

  function fillCreateForm() {
    const f = ui.createForm;
    if (!server || f.dataset.filled) {
      return;
    }
    const opt = (sel, value, label) => {
      const o = document.createElement('option');
      o.value = value;
      o.textContent = label;
      sel.appendChild(o);
    };
    opt(f.stage, '', 'Random');
    for (const [id, name] of Object.entries(server.arenas).sort((a, b) => a[1].localeCompare(b[1]))) {
      opt(f.stage, id, name);
    }
    server.scenarios.forEach((name, i) => opt(f.scenario, i, name));
    server.weaponSets.forEach((name, i) => opt(f.weaponset, i, name));
    f.weaponset.value = '1';
    f.stage.value = '50'; // Skedar
    f.dataset.filled = '1';
  }

  function fmtTime(secs) {
    if (secs === null || secs === undefined) {
      return 'no limit';
    }
    return `${Math.floor(secs / 60)}:${String(secs % 60).padStart(2, '0')} left`;
  }

  function renderRooms(rooms) {
    ui.rooms.innerHTML = '';
    ui.empty.hidden = rooms.length > 0;

    // joinable matches first, then the busiest
    const freeOf = (r) => (r.state === 'playing' ? (r.free ?? r.players.filter((p) => !p).length) : 0);
    rooms = [...rooms].sort((a, b) => (freeOf(b) > 0) - (freeOf(a) > 0) ||
      b.players.filter(Boolean).length - a.players.filter(Boolean).length);

    for (const r of rooms) {
      const players = r.players.filter(Boolean);
      const free = freeOf(r);
      const ended = r.state !== 'playing';
      const canJoin = !ended && free > 0 && server && server.enabled;

      const card = document.createElement('div');
      card.className = `room${canJoin ? '' : ' room-closed'}`;

      // title + status
      const head = document.createElement('div');
      head.className = 'room-head';
      const title = document.createElement('strong');
      title.textContent = r.name;
      if (r.locked) {
        const lock = document.createElement('span');
        lock.className = 'room-lock';
        lock.title = 'Needs a password';
        lock.textContent = '🔒';
        title.appendChild(lock);
      }
      const pill = document.createElement('span');
      pill.className = `room-pill ${ended ? 'pill-ended' : free > 0 ? 'pill-open' : 'pill-full'}`;
      pill.textContent = ended ? 'Finished' : free > 0 ? 'Open' : 'Full';
      head.append(title, pill);

      // what kind of match
      const sub = document.createElement('div');
      sub.className = 'room-sub';
      sub.textContent = [
        r.scenarioName,
        r.arena,
        r.bots ? `${r.bots} bot${r.bots === 1 ? '' : 's'}` : 'no bots',
        ended ? 'match over' : fmtTime(r.timeLeft),
      ].join(' · ');

      // slots: one marker per player slot, then "2 of 4 slots free"
      const slots = document.createElement('div');
      slots.className = 'room-slots';
      const pips = document.createElement('span');
      pips.className = 'pips';
      r.players.forEach((name, i) => {
        const pip = document.createElement('span');
        pip.className = `pip${name ? ' pip-taken' : ''}`;
        pip.title = name ? name : `Slot ${i + 1}: free`;
        pips.appendChild(pip);
      });
      const freeText = document.createElement('span');
      freeText.className = `free ${free > 0 && !ended ? 'free-yes' : 'free-no'}`;
      freeText.textContent = ended ? `${players.length} played`
        : free > 0 ? `${free} of ${r.players.length} slots free` : 'No free slots';
      slots.append(pips, freeText);
      if (r.reservedSlots) {
        const held = document.createElement('small');
        held.className = 'held';
        held.textContent = `(${r.reservedSlots} held for a player reconnecting)`;
        slots.appendChild(held);
      }

      const who = document.createElement('div');
      who.className = 'room-players';
      who.textContent = players.length ? `Playing: ${players.join(', ')}` : 'Nobody playing yet';

      const info = document.createElement('div');
      info.className = 'room-info';
      info.append(head, sub, slots, who);

      const btn = document.createElement('button');
      btn.type = 'button';
      btn.className = canJoin ? 'join primary' : 'join';
      btn.textContent = ended ? 'Finished' : free > 0 ? 'Join' : 'Full';
      btn.disabled = !canJoin;
      btn.addEventListener('click', () => joinRoom(r));

      card.append(info, btn);
      ui.rooms.appendChild(card);
    }
  }

  function requireRom() {
    if (!window.PDWeb || !window.PDWeb.romBytes()) {
      setStatus('Choose your ROM above first.', true);
      return false;
    }
    return true;
  }

  function joinRoom(room) {
    if (!requireRom()) {
      return;
    }
    let password;
    if (room.locked) {
      password = prompt('Password for this match:');
      if (password === null) {
        return;
      }
    }
    store.set(NAME_KEY, playerName());
    send({ type: 'hello', name: playerName(), clientId: clientId(), build: config.build });
    const prev = usableSession();
    send({ type: 'join', roomId: room.id, password, token: prev && prev.roomId === room.id ? prev.token : undefined });
    match = { pending: true };
    setStatus('Joining…');
  }

  ui.createToggle.addEventListener('click', () => {
    ui.createForm.hidden = !ui.createForm.hidden;
  });

  ui.createForm.addEventListener('submit', (e) => {
    e.preventDefault();
    if (!requireRom()) {
      return;
    }
    const f = ui.createForm;
    store.set(NAME_KEY, playerName());
    send({ type: 'hello', name: playerName(), clientId: clientId(), build: config.build });
    send({
      type: 'create',
      settings: {
        name: f.roomname.value,
        stage: f.stage.value,
        scenario: f.scenario.value,
        timelimit: f.timelimit.value,
        scorelimit: f.scorelimit.value,
        bots: f.bots.value,
        botDifficulty: f.botDifficulty.value,
        weaponset: f.weaponset.value,
        teams: f.teams.checked,
        password: f.password.value,
      },
    });
    match = { pending: true };
    setStatus('Starting the match…');
  });

  ui.rejoin.addEventListener('click', () => {
    const s = rejoinCandidate || usableSession();
    ui.rejoin.hidden = true;
    if (s && requireRom()) {
      send({ type: 'hello', name: playerName(), clientId: clientId(), build: config.build });
      send({ type: 'join', roomId: s.roomId, token: s.token });
      match = { pending: true };
      setStatus('Rejoining…');
    }
  });

  function showLobby() {
    window.PDWeb.showOverlay();
  }

  // ---------------------------------------------------------------------------
  // running a match

  function hud(text) {
    ui.hudText.textContent = text;
  }

  async function onJoined(msg) {
    session = { roomId: msg.roomId, token: msg.token, slot: msg.slot, at: Date.now() };
    session.tab = tabId();
    store.set(sessionKey(), JSON.stringify(session));

    if (match && match.module) {
      // reconnected to the match we're already running; a snapshot follows
      match.slot = msg.slot;
      match.awaitingSnapshot = true;
      match.live = false;
      match.buffer = [];
      hud('Rejoining…');
      return;
    }

    match = {
      roomId: msg.roomId,
      slot: msg.slot,
      settings: msg.settings,
      label: `${msg.settings.name} · ${msg.scenarioName} · ${msg.arena}`,
      buffer: [],
      names: [],
      backlog: [],
      awaitingSnapshot: true,
      snapshot: null,
      live: false,
      ended: false,
      module: null,
      host: null,
    };

    setStatus('Loading the match…');
    document.body.classList.add('is-online');
    ui.hud.hidden = false;
    hud('Loading…');

    try {
      const module = await window.PDWeb.startGame({
        extraArgs: ['--net-match', '/net/match.cfg', '--net-slot', String(msg.slot)],
        files: { '/net/match.cfg': PDNet.matchConfigText(msg.match) },
        hooks: {
          onNetHash: (tick, hash) => send({ type: 'hash', tick, hash }),
          onNetLocalInput: (ptr) => sendInput(ptr),
        },
      });
      match.module = module;
      match.host = PDNet.attach(module);
      if (match.snapshot) {
        await applySnapshot();
      }
      ui.hint.hidden = false;
      setTimeout(() => { ui.hint.hidden = true; }, 10000);
    } catch (e) {
      hud(`Could not start the game: ${e.message || e}`);
    }
  }

  async function inflate(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  async function applySnapshot() {
    const snap = match.snapshot;
    match.snapshot = null;
    const raw = await inflate(snap.packed);
    match.module._netClearQueue();
    match.backlog = [];
    const tick = PDNet.restoreSnapshot(match.module, raw);
    match.lastTick = tick;
    match.awaitingSnapshot = false;
    match.live = true;

    // ticks that arrived while loading
    const buffered = match.buffer;
    match.buffer = [];
    for (const packet of buffered) {
      pushTick(packet);
    }
    updateHud();
  }

  function pushTick(packet) {
    const view = new DataView(packet.buffer, packet.byteOffset, packet.byteLength);
    const tick = view.getUint32(0, true);
    if (match.lastTick !== undefined && tick <= match.lastTick) {
      return; // already part of the snapshot
    }
    match.lastTick = tick;
    match.backlog.push({ tick, packet });
    drainBacklog();
  }

  // hands ticks to the game, keeping its queue (1024 ticks) from overflowing during catch-up
  function drainBacklog() {
    while (match && match.host && match.backlog.length && match.host.queued() < 600) {
      const { tick, packet } = match.backlog.shift();
      // player name changes for this tick go in first
      for (let i = match.names.length - 1; i >= 0; i--) {
        if (match.names[i].tick <= tick) {
          queueName(match.names[i]);
          match.names.splice(i, 1);
        }
      }
      match.host.pushRaw(packet);
    }
  }
  setInterval(() => match && drainBacklog(), 50);

  function queueName(n) {
    const m = match.module;
    const buf = m._netGetNameBuffer();
    m.HEAPU8.fill(0, buf, buf + 32);
    for (let i = 0; i < Math.min(12, n.name.length); i++) {
      m.HEAPU8[buf + i] = n.name.charCodeAt(i) & 0x7f;
    }
    m._netQueueName(n.tick, n.slot);
  }

  function sendInput(ptr) {
    if (!match || !match.live || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    const msg = new Uint8Array(1 + PDNet.INPUT_SIZE);
    msg[0] = MSG_INPUT;
    msg.set(match.module.HEAPU8.subarray(ptr, ptr + PDNet.INPUT_SIZE), 1);
    ws.send(msg);
  }

  function updateHud() {
    if (!match || match.ended) {
      return;
    }
    if (match.live) {
      hud(`${match.label} · ping ${Math.round(rtt)} ms`);
    }
  }
  setInterval(updateHud, 1000);
  setInterval(saveSession, 5000);
  // the browser may freeze or discard a background tab; keep the session fresh for its return
  document.addEventListener('freeze', saveSession);
  window.addEventListener('pagehide', saveSession);

  function onEnded(results) {
    if (!match) {
      return;
    }
    match.ended = true;
    match.live = false;
    clearSession();
    hud('Match over');
    ui.resultsBody.innerHTML = '';
    results.forEach((r, i) => {
      const tr = document.createElement('tr');
      for (const v of [i + 1, `${r.name}${r.bot ? ' (bot)' : ''}`, r.kills, r.deaths, r.suicides]) {
        const td = document.createElement('td');
        td.textContent = v;
        tr.appendChild(td);
      }
      ui.resultsBody.appendChild(tr);
    });
    ui.results.hidden = false;
  }

  function leaveToLobby() {
    send({ type: 'leave' });
    clearSession();
    // the game can't be unloaded from the page; reload straight back into the lobby
    location.hash = 'online';
    location.reload();
  }

  ui.leave.addEventListener('click', () => {
    if (!match || match.ended || confirm('Leave this match?')) {
      leaveToLobby();
    }
  });
  ui.resultsBack.addEventListener('click', leaveToLobby);

  // ---------------------------------------------------------------------------
  // notices and scoreboard

  function showNotice(text) {
    if (!match || !match.module) {
      return;
    }
    const feed = $('net-feed');
    const el = document.createElement('div');
    el.className = 'net-notice';
    el.textContent = text;
    feed.appendChild(el);
    setTimeout(() => el.remove(), 5200);
    while (feed.children.length > 4) {
      feed.firstChild.remove();
    }
  }

  // live scores, read from this browser's copy of the match (port/src/net.c netGetResults)
  function readScores() {
    const m = match.module;
    const count = m._netGetResults();
    const ptr = m._netGetResultsBuffer();
    const view = new DataView(m.HEAPU8.buffer);
    const rows = [];
    for (let i = 0; i < count; i++) {
      const base = ptr + i * 36;
      let name = '';
      for (let j = 0; j < 16; j++) {
        const c = view.getUint8(base + j);
        if (!c) break;
        name += String.fromCharCode(c);
      }
      // older builds named bots "(null):2" (fixed in port/src/net.c)
      name = name.replace(/^\(null\)(?::(\d+))?$/, (_, n) => (n ? `Sim ${n}` : 'Sim'));
      const slot = view.getInt32(base + 16, true);
      const row = {
        name,
        slot,
        bot: slot < 0,
        kills: view.getInt32(base + 20, true),
        suicides: view.getInt32(base + 24, true),
        deaths: view.getInt32(base + 28, true),
      };
      row.gone = slot >= 0 && !!m._netGetSlotVacant(slot);
      // an empty slot nobody has played in ("Open 3"; older builds counted a death when parking it)
      if (row.gone && !row.kills && !row.suicides && (/^Open \d$/.test(row.name) || !row.deaths)) {
        continue;
      }
      row.score = row.kills - row.suicides;
      rows.push(row);
    }
    rows.sort((a, b) => b.score - a.score || a.deaths - b.deaths || a.name.localeCompare(b.name));
    return rows;
  }

  function renderScoreboard() {
    const body = $('sb-body');
    body.innerHTML = '';
    readScores().forEach((r, i) => {
      const tr = document.createElement('tr');
      if (r.slot === match.slot) tr.className = 'sb-me';
      if (r.gone) tr.classList.add('sb-gone');
      const kd = r.deaths ? (r.kills / r.deaths).toFixed(2) : r.kills.toFixed(2);
      const cells = [i + 1, r.name, r.kills, r.deaths, r.suicides, kd];
      cells.forEach((v, c) => {
        const td = document.createElement('td');
        td.textContent = v;
        if (c === 1 && (r.bot || r.gone)) {
          const tag = document.createElement('span');
          tag.className = 'sb-tag';
          tag.textContent = r.bot ? 'BOT' : 'LEFT';
          td.appendChild(tag);
        }
        tr.appendChild(td);
      });
      body.appendChild(tr);
    });
    $('sb-title').textContent = `${match.settings.name} · ${match.label.split(' · ').slice(1).join(' · ')}`;
    const limit = match.settings.timelimit;
    if (limit >= 60 || match.lastTick === undefined) {
      $('sb-time').textContent = '';
    } else {
      const left = Math.max(0, Math.round(limit * 60 - Math.max(0, match.lastTick) / 60));
      $('sb-time').textContent = `${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}`;
    }
  }

  // Hold to show the scoreboard: Select/View (Xbox), Minus (Switch Pro) and Share are button 8 of
  // the standard gamepad layout; on PlayStation controllers the touchpad (button 17, when the
  // browser exposes it) is used instead.
  function scoreboardButtonHeld() {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    for (const gp of pads) {
      if (!gp || !gp.connected) {
        continue;
      }
      const ps = /054c|playstation|dualshock|dualsense/i.test(gp.id);
      const index = ps && gp.buttons.length > 17 ? 17 : 8;
      if (gp.buttons[index] && gp.buttons[index].pressed) {
        return true;
      }
    }
    return false;
  }

  let scoreboardShown = false;
  let scoreboardRefresh = 0;
  function pollScoreboard() {
    const show = !!(match && match.module && match.live && !match.ended && scoreboardButtonHeld());
    if (show !== scoreboardShown) {
      scoreboardShown = show;
      $('net-scoreboard').hidden = !show;
    }
    if (show && performance.now() - scoreboardRefresh > 250) {
      scoreboardRefresh = performance.now();
      renderScoreboard();
    }
    requestAnimationFrame(pollScoreboard);
  }
  requestAnimationFrame(pollScoreboard);

  // The browser can drop the WebGL context, typically while the tab is in the background. The game
  // keeps simulating without drawing; once the tab is visible again the page reloads and rejoins,
  // which rebuilds everything the renderer had on the GPU.
  let glLost = false;
  function reloadAndRejoin() {
    if (!glLost || document.visibilityState !== 'visible') {
      return;
    }
    try { sessionStorage.setItem(AUTOREJOIN_KEY, '1'); } catch { /* ignore */ }
    saveSession();
    location.reload();
  }
  document.getElementById('canvas').addEventListener('webglcontextlost', (e) => {
    if (match && match.module && !match.ended) {
      e.preventDefault();
      glLost = true;
      hud('Graphics were reset by the browser; reconnecting…');
      reloadAndRejoin();
    }
  });
  document.addEventListener('visibilitychange', reloadAndRejoin);

  function tryAutoRejoin() {
    if (!autoRejoin || !server) {
      return;
    }
    const started = Date.now();
    const attempt = () => {
      if (window.PDWeb.romBytes()) {
        const s = autoRejoin;
        autoRejoin = null;
        ui.rejoin.hidden = true;
        send({ type: 'hello', name: playerName(), clientId: clientId(), build: config.build });
        send({ type: 'join', roomId: s.roomId, token: s.token });
        match = { pending: true };
        setStatus('Rejoining…');
      } else if (Date.now() - started < 15000) {
        setTimeout(attempt, 300); // the ROM is still being loaded from browser storage
      }
    };
    attempt();
  }

  // ---------------------------------------------------------------------------

  async function init() {
    ui.name.value = store.get(NAME_KEY) || '';
    ui.name.addEventListener('change', () => store.set(NAME_KEY, playerName()));
    try {
      config = await (await fetch('server-config.json', { cache: 'no-store' })).json();
    } catch {
      config = {};
    }
    if (!config.build) {
      ui.panel.hidden = true;
      return;
    }
    ui.panel.hidden = false;
    setStatus('Connecting…');

    const prev = usableSession();
    rejoinCandidate = prev;
    let wantAuto = false;
    try {
      wantAuto = sessionStorage.getItem(AUTOREJOIN_KEY) === '1';
      sessionStorage.removeItem(AUTOREJOIN_KEY);
    } catch { /* ignore */ }
    // this tab's own session (it was reloaded or discarded and restored), or after a graphics
    // reset: go straight back in. A session from another, closed tab is offered with the button.
    if (prev && (wantAuto || prev.tab === tabId())) {
      autoRejoin = prev;
    }
    if (prev) {
      ui.rejoinText.textContent = 'You were in a match a moment ago.';
      ui.rejoin.hidden = false;
    }
    if (location.hash === '#online') {
      ui.panel.scrollIntoView();
    }
    connect();
  }

  window.PDOnline = {
    init,
    build: () => config && config.build,
    // for troubleshooting from the console
    vacant: () => match && match.module && [0, 1, 2, 3].map((i) => match.module._netGetSlotVacant(i)),
    scores: () => match && match.module && readScores(),
    debug: () => match && {
      live: match.live,
      lastTick: match.lastTick,
      backlog: match.backlog && match.backlog.length,
      queued: match.host && match.host.queued(),
      ticksRun: match.host && match.host.ticksRun(),
      rtt: Math.round(rtt),
    },
  };
})();
