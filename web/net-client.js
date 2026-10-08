// Online multiplayer client: the lobby connection for the game's Online menu (port/src/lobby.c),
// and running an online match in this browser. Talks to a lobby server (web/net/lobby.js) over a
// WebSocket at /net: by default the one this page came from, or another picked in the game.
// Needs nethost.js (window.PDNet) and pd-web.js (window.PDWeb) loaded first.
(() => {
  'use strict';

  const MSG_TICK = 1;
  const MSG_SNAPSHOT = 2;
  const MSG_INPUT = 3;
  const MSG_SNAPSHOT_REPLY = 4; // to a relay server: [4][u32 request id][u32 tick][deflate-raw snapshot]
  const HASH_REPORT_TICKS = 120; // state hashes sent to the server (the game makes one every 60 ticks)
  const SESSION_KEY = 'pd-online-session';
  const NAME_KEY = 'pd-online-name';
  const CLIENT_KEY = 'pd-online-client';
  const SESSION_MAX_AGE_MS = 60 * 1000; // matches the server's reconnect grace period
  const SESSION_ORPHAN_MS = 15 * 1000;   // a session whose tab hasn't checked in this long was closed
  const TAB_KEY = 'pd-online-tab';
  const INTENT_KEY = 'pd-online-intent'; // the match to join after the page reloads
  const RETURN_KEY = 'pd-online-return'; // open the game's Online menu after the page reloads

  const $ = (id) => document.getElementById(id);
  const ui = {
    panel: $('online'),
    status: $('online-status'),
    rejoin: $('online-rejoin'),
    back: $('online-back'),
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
  let pendingRejoin = null;   // session to rejoin once connected
  let pendingIntent = null;   // match picked in the game's menu, joined once connected
  let matchServer = null;     // lobby server of the match (see lobbyUrl)
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

  // the agent name the game's menu passed along
  function playerName() {
    return (store.get(NAME_KEY) || '').trim().slice(0, 12) || 'Agent';
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
    ws = new WebSocket(lobbyUrl(matchServer));
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
          setStatus('Connected.');
        }
        if (pendingIntent) {
          const intent = pendingIntent;
          pendingIntent = null;
          joinIntent(intent);
        } else if (pendingRejoin) {
          rejoinWhenReady();
        }
        break;
      case 'rooms':
        break;
      case 'joined':
        onJoined(msg);
        break;
      case 'name':
        if (match) {
          match.names.push(msg);
        }
        break;
      case 'snapshot-request':
        answerSnapshotRequest(msg.reqId).catch((e) => {
          console.warn('snapshot for another player failed', e);
          send({ type: 'snapshot-failed', reqId: msg.reqId });
        });
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
        if (msg.code === 'build' && isDefaultServer(matchServer)) {
          setStatus(msg.message, true);
          setTimeout(() => location.reload(), 1500);
        } else if (msg.code === 'build') {
          match = null;
          failJoin('That server runs a different version of the game.');
        } else if (match && match.pending) {
          // the server refused to let us in (once in, the game is loading or running; errors
          // after that are only reported)
          match = null;
          clearSession();
          document.body.classList.remove('is-online');
          failJoin(msg.message);
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
  // the in-game Online menu (port/src/lobby.c) and handing over to a match

  // "host[:port]" or a ws:// / wss:// URL -> the lobby's WebSocket URL
  function lobbyUrl(serverName) {
    const s = String(serverName || '').trim() || defaultServer();
    if (/^wss?:\/\//i.test(s)) {
      return /\/net\/?$/.test(s) ? s : `${s.replace(/\/$/, '')}/net`;
    }
    const host = s.replace(/^https?:\/\//i, '').replace(/\/.*$/, '');
    // a page served over HTTPS can only open secure WebSockets
    const secure = location.protocol === 'https:' || /:443$/.test(host);
    return `${secure ? 'wss' : 'ws'}://${host}/net`;
  }

  function defaultServer() {
    return (config && config.lobby) || location.host;
  }

  function isDefaultServer(serverName) {
    return !serverName || lobbyUrl(serverName) === lobbyUrl(defaultServer());
  }

  // the game's own connection, for listing, creating and joining matches from its menu
  const gameLobby = { ws: null, want: null, queue: [], retry: 0 };

  function lobbyOpen(serverName) {
    lobbyClose();
    gameLobby.want = serverName || defaultServer();
    gameLobby.queue = [];
    const open = () => {
      if (!gameLobby.want) {
        return;
      }
      let sock;
      try {
        sock = new WebSocket(lobbyUrl(gameLobby.want));
      } catch (e) {
        console.warn(`lobby: ${e.message}`);
        gameLobby.retry = setTimeout(open, 3000);
        return;
      }
      gameLobby.ws = sock;
      sock.onmessage = (ev) => {
        if (typeof ev.data === 'string' && gameLobby.ws === sock) {
          gameLobby.queue.push(ev.data);
          if (gameLobby.queue.length > 64) {
            gameLobby.queue.shift();
          }
        }
      };
      sock.onclose = () => {
        if (gameLobby.ws === sock) {
          gameLobby.ws = null;
          gameLobby.retry = setTimeout(open, 3000);
        }
      };
    };
    open();
    return true;
  }

  function lobbyClose() {
    gameLobby.want = null;
    clearTimeout(gameLobby.retry);
    if (gameLobby.ws) {
      const sock = gameLobby.ws;
      gameLobby.ws = null;
      sock.close();
    }
    gameLobby.queue = [];
  }

  // 0 = closed, 1 = connecting, 2 = open (port/src/lobby.c TRANSPORT_*)
  function lobbyState() {
    if (!gameLobby.ws) {
      return gameLobby.want ? 1 : 0;
    }
    return gameLobby.ws.readyState === WebSocket.OPEN ? 2 : gameLobby.ws.readyState === WebSocket.CONNECTING ? 1 : 0;
  }

  function lobbySend(text) {
    if (gameLobby.ws && gameLobby.ws.readyState === WebSocket.OPEN) {
      gameLobby.ws.send(text);
    }
  }

  function lobbyRecv() {
    return gameLobby.queue.length ? gameLobby.queue.shift() : null;
  }

  function setFlag(key) {
    try { sessionStorage.setItem(key, '1'); } catch { /* ignore */ }
  }

  function takeFlag(key) {
    try {
      const v = sessionStorage.getItem(key) === '1';
      sessionStorage.removeItem(key);
      return v;
    } catch {
      return false;
    }
  }

  // The game can't switch into a match in place, so the page reloads and joins (see init)
  async function enterMatch(intent) {
    lobbyClose();
    store.set(NAME_KEY, String(intent.name || 'Agent').slice(0, 12));
    try {
      sessionStorage.setItem(INTENT_KEY, JSON.stringify({
        server: intent.server || defaultServer(),
        roomId: intent.roomId,
        password: intent.password || undefined,
        at: Date.now(),
      }));
    } catch { /* ignore */ }
    await window.PDWeb.flush(); // keep the agent's progress and settings
    location.reload();
  }

  // after a match: restart the game and open its Online menu
  function backToGame() {
    setFlag(RETURN_KEY);
    window.PDWeb.autostartNextLoad();
    location.hash = '';
    location.reload();
  }

  function takeIntent() {
    try {
      const intent = JSON.parse(sessionStorage.getItem(INTENT_KEY) || 'null');
      sessionStorage.removeItem(INTENT_KEY);
      return intent && Date.now() - intent.at < 60 * 1000 ? intent : null;
    } catch {
      return null;
    }
  }

  // joins the match the game's menu picked, once the ROM is loaded
  function joinIntent(intent) {
    const started = Date.now();
    const attempt = () => {
      if (window.PDWeb.romBytes()) {
        send({ type: 'hello', name: playerName(), clientId: clientId(), build: config.build });
        const prev = usableSession();
        send({
          type: 'join',
          roomId: intent.roomId,
          password: intent.password,
          token: prev && prev.roomId === intent.roomId ? prev.token : undefined,
        });
        match = { pending: true };
        setStatus('Joining the match…');
      } else if (Date.now() - started < 15000) {
        setTimeout(attempt, 300); // the ROM is still being loaded from browser storage
      } else {
        failJoin('Choose your ROM first.');
      }
    };
    attempt();
  }

  function failJoin(text) {
    setStatus(text, true);
    ui.back.hidden = false;
    showLobby();
  }

  ui.rejoin.addEventListener('click', () => {
    const s = rejoinCandidate || usableSession();
    ui.rejoin.hidden = true;
    if (s && requireRom()) {
      matchServer = s.server || defaultServer();
      pendingRejoin = s;
      if (ws && ws.readyState === WebSocket.OPEN) {
        sendRejoin();
      } else {
        connect();
      }
      setStatus('Rejoining…');
    }
  });

  function sendRejoin() {
    const s = pendingRejoin;
    pendingRejoin = null;
    send({ type: 'hello', name: playerName(), clientId: clientId(), build: config.build });
    send({ type: 'join', roomId: s.roomId, token: s.token });
    match = { pending: true };
  }

  ui.back.addEventListener('click', backToGame);

  function requireRom() {
    if (!window.PDWeb || !window.PDWeb.romBytes()) {
      setStatus('Choose your ROM above first.', true);
      return false;
    }
    return true;
  }

  function showLobby() {
    window.PDWeb.showOverlay();
  }

  // ---------------------------------------------------------------------------
  // running a match

  function hud(text) {
    ui.hudText.textContent = text;
  }

  async function onJoined(msg) {
    session = { roomId: msg.roomId, token: msg.token, slot: msg.slot, server: matchServer, at: Date.now() };
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
      // a relay server starts a new match on its first player's game, from tick 0
      fresh: !!msg.fresh,
      awaitingSnapshot: !msg.fresh,
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
          onNetHash: (tick, hash) => {
            if (tick % HASH_REPORT_TICKS === 0) {
              send({ type: 'hash', tick, hash });
            }
          },
          onNetLocalInput: (ptr) => sendInput(ptr),
        },
      });
      match.module = module;
      match.host = PDNet.attach(module);
      if (match.snapshot) {
        await applySnapshot();
      } else if (match.fresh) {
        match.live = true;
        const buffered = match.buffer;
        match.buffer = [];
        for (const packet of buffered) {
          pushTick(packet);
        }
        updateHud();
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

  // The server repeats a player's last input until a new one arrives, so only changes are sent,
  // plus a heartbeat. This keeps the message count low (the Cloudflare relay pays per message).
  const INPUT_HEARTBEAT_MS = 1000;
  let lastInput = null;
  let lastInputSentAt = 0;

  function sendInput(ptr) {
    if (!match || !match.live || !ws || ws.readyState !== WebSocket.OPEN) {
      return;
    }
    const input = match.module.HEAPU8.subarray(ptr, ptr + PDNet.INPUT_SIZE);
    const now = performance.now();
    if (lastInput && now - lastInputSentAt < INPUT_HEARTBEAT_MS && lastInput.every((b, i) => b === input[i])) {
      return;
    }
    lastInput = input.slice();
    lastInputSentAt = now;
    const msg = new Uint8Array(1 + PDNet.INPUT_SIZE);
    msg[0] = MSG_INPUT;
    msg.set(input, 1);
    ws.send(msg);
  }

  // ---------------------------------------------------------------------------
  // serving a relay server (cloudlare_worker_server): it has no copy of the game, so the players'
  // games provide snapshots for players joining, and report the result

  let snapRanges = null;
  async function loadSnapRanges() {
    if (!snapRanges) {
      const build = config && config.build;
      const res = await fetch(build ? `pd.snap.json?v=${build}` : 'pd.snap.json');
      snapRanges = (await res.json()).ranges;
    }
    return snapRanges;
  }

  async function deflate(bytes) {
    const stream = new Blob([bytes]).stream().pipeThrough(new CompressionStream('deflate-raw'));
    return new Uint8Array(await new Response(stream).arrayBuffer());
  }

  async function answerSnapshotRequest(reqId) {
    if (!match || !match.module || !match.live || match.awaitingSnapshot) {
      send({ type: 'snapshot-failed', reqId });
      return;
    }
    const ranges = await loadSnapRanges();
    // taken between ticks: the game is waiting for the next one while this handler runs
    const snap = PDNet.makeSnapshot(match.module, ranges);
    const packed = await deflate(snap.bytes);
    const msg = new Uint8Array(9 + packed.length);
    const view = new DataView(msg.buffer);
    msg[0] = MSG_SNAPSHOT_REPLY;
    view.setUint32(1, reqId >>> 0, true);
    view.setUint32(5, snap.tick >>> 0, true);
    msg.set(packed, 9);
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(msg);
    }
  }

  // tells the server the match is over, with the scores, once
  function checkMatchOver() {
    if (!match || !match.module || !match.live || match.ended || match.overSent) {
      return;
    }
    if (match.module._netGetMatchOver()) {
      match.overSent = true;
      const results = readScores().map((r) => ({
        name: r.name, bot: r.bot, kills: r.kills, suicides: r.suicides, deaths: r.deaths,
      }));
      send({ type: 'over', tick: match.lastTick, results });
    }
  }
  setInterval(checkMatchOver, 500);

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
    // the match can't be unloaded from the page; reload into the game's Online menu
    backToGame();
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

  function rejoinWhenReady() {
    const started = Date.now();
    const attempt = () => {
      if (!pendingRejoin) {
        return;
      }
      if (window.PDWeb.romBytes()) {
        ui.rejoin.hidden = true;
        sendRejoin();
        setStatus('Rejoining…');
      } else if (Date.now() - started < 15000) {
        setTimeout(attempt, 300); // the ROM is still being loaded from browser storage
      }
    };
    attempt();
  }

  // ---------------------------------------------------------------------------

  async function init() {
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

    // a match picked in the game's Online menu
    const intent = takeIntent();
    if (intent) {
      matchServer = intent.server;
      pendingIntent = intent;
      setStatus('Joining the match…');
      connect();
      return;
    }

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
      matchServer = prev.server || defaultServer();
      pendingRejoin = prev;
      setStatus('Rejoining your match…');
      connect();
    } else if (prev) {
      ui.rejoinText.textContent = 'You were in a match a moment ago.';
      ui.rejoin.hidden = false;
    }
  }

  window.PDOnline = {
    init,
    build: () => config && config.build,
    clientId,
    defaultServer,
    // used by the game (port/src/lobby.c)
    lobbyOpen,
    lobbyClose,
    lobbyState,
    lobbySend,
    lobbyRecv,
    enterMatch,
    takeReturnToMenu: () => takeFlag(RETURN_KEY),
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
