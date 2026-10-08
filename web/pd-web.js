// Browser loader for the Perfect Dark WebAssembly build.
//
// The ROM never comes from the server: the player picks their own dump, it is validated,
// converted to big-endian .z64 if needed, cached in IndexedDB and written into the
// Emscripten virtual filesystem before the game's main() runs. Saves and pd.ini live in
// an IDBFS mount at /save which is flushed to IndexedDB periodically.

(() => {
  'use strict';

  const ROM_SIZE = 32 * 1024 * 1024;
  const ROM_FS_PATH = '/data/pd.ntsc-final.z64';
  const SAVE_DIR = '/save';
  const DB_NAME = 'perfect-dark-web';
  const DB_STORE = 'files';
  const DB_ROM_KEY = 'rom';
  const SAVE_SYNC_INTERVAL_MS = 5000;
  const SAVE_FLUSH_DELAY_MS = 500;
  const CONFIG_SAVE_INTERVAL_MS = 15000;

  // Emscripten's IDBFS keeps the files mounted at SAVE_DIR in this IndexedDB database
  // (see emscripten/src/lib/libidbfs.js); backups read and write it directly so they also
  // work before the game has been started
  const IDBFS_DB_NAME = SAVE_DIR;
  const IDBFS_DB_VERSION = 21;
  const IDBFS_STORE = 'FILE_DATA';
  const BACKUP_FORMAT = 'perfect-dark-web-saves';

  const $ = (id) => document.getElementById(id);
  const ui = {
    overlay: $('overlay'),
    drop: $('drop'),
    file: $('rom-file'),
    romInfo: $('rom-info'),
    start: $('start'),
    changeRom: $('change-rom'),
    status: $('status'),
    canvas: $('canvas'),
    toast: $('toast'),
    fullscreen: $('fullscreen'),
    error: $('error'),
    errorText: $('error-text'),
    exportSaves: $('export-saves'),
    importSaves: $('import-saves'),
    importFile: $('import-file'),
    savesStatus: $('saves-status'),
    backupIngame: $('backup-ingame'),
  };

  let romBytes = null;
  const AUTOSTART_KEY = 'pd-autostart';
  // set when the page reloads out of an online match, to go straight back into the game
  const autostart = (() => {
    try {
      const v = sessionStorage.getItem(AUTOSTART_KEY) === '1';
      sessionStorage.removeItem(AUTOSTART_KEY);
      return v;
    } catch (e) {
      return false;
    }
  })();
  let module = null;
  let syncing = null; // promise of the running save sync, if any
  let syncAgain = false;

  // ---------------------------------------------------------------------------
  // IndexedDB cache for the ROM

  function openDb() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(DB_NAME, 1);
      req.onupgradeneeded = () => req.result.createObjectStore(DB_STORE);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  async function dbGet(key) {
    const db = await openDb();
    return new Promise((resolve, reject) => {
      const req = db.transaction(DB_STORE, 'readonly').objectStore(DB_STORE).get(key);
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  async function dbPut(key, value) {
    const db = await openDb();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(DB_STORE, 'readwrite');
      tx.objectStore(DB_STORE).put(value, key);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async function dbDelete(key) {
    const db = await openDb();
    return new Promise((resolve, reject) => {
      const tx = db.transaction(DB_STORE, 'readwrite');
      tx.objectStore(DB_STORE).delete(key);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  // ---------------------------------------------------------------------------
  // ROM validation

  // Converts .v64 (16-bit byteswapped) and .n64 (32-bit little endian) dumps to .z64 in place.
  function normalizeRom(bytes) {
    const b0 = bytes[0], b1 = bytes[1], b2 = bytes[2], b3 = bytes[3];
    if (b0 === 0x80 && b1 === 0x37 && b2 === 0x12 && b3 === 0x40) {
      return 'z64';
    }
    if (b0 === 0x37 && b1 === 0x80 && b2 === 0x40 && b3 === 0x12) {
      for (let i = 0; i < bytes.length; i += 2) {
        const t = bytes[i]; bytes[i] = bytes[i + 1]; bytes[i + 1] = t;
      }
      return 'v64';
    }
    if (b0 === 0x40 && b1 === 0x12 && b2 === 0x37 && b3 === 0x80) {
      for (let i = 0; i < bytes.length; i += 4) {
        let t = bytes[i]; bytes[i] = bytes[i + 3]; bytes[i + 3] = t;
        t = bytes[i + 1]; bytes[i + 1] = bytes[i + 2]; bytes[i + 2] = t;
      }
      return 'n64';
    }
    return null;
  }

  function validateRom(bytes) {
    if (bytes.length !== ROM_SIZE) {
      throw new Error(`Expected a 32 MB ROM, got ${(bytes.length / 1048576).toFixed(1)} MB.`);
    }
    const format = normalizeRom(bytes);
    if (!format) {
      throw new Error('This does not look like an N64 ROM.');
    }
    const id = String.fromCharCode(bytes[0x3b], bytes[0x3c], bytes[0x3d], bytes[0x3e]);
    const rev = bytes[0x3f];
    if (id !== 'NPDE') {
      throw new Error(`This build needs Perfect Dark NTSC (NPDE), but this ROM is ${id}.`);
    }
    if (rev !== 1) {
      throw new Error(`This build needs the NTSC v1.1 ROM, but this is v1.${rev}.`);
    }
    return { format, title: 'Perfect Dark (USA) v1.1' };
  }

  // ---------------------------------------------------------------------------
  // UI

  function setStatus(text, isError) {
    ui.status.textContent = text || '';
    ui.status.classList.toggle('is-error', !!isError);
  }

  let toastTimer = 0;
  function toast(text) {
    ui.toast.textContent = text;
    ui.toast.classList.add('is-visible');
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => ui.toast.classList.remove('is-visible'), 3500);
  }

  function showRomReady(info) {
    ui.romInfo.textContent = info;
    ui.drop.classList.add('has-rom');
    ui.start.disabled = false;
    ui.changeRom.hidden = false;
  }

  async function acceptRomFile(file) {
    setStatus('Checking ROM…');
    try {
      const bytes = new Uint8Array(await file.arrayBuffer());
      const { format, title } = validateRom(bytes);
      romBytes = bytes;
      showRomReady(`${title}${format !== 'z64' ? ` (converted from .${format})` : ''}`);
      setStatus('');
      try {
        await dbPut(DB_ROM_KEY, bytes);
      } catch (e) {
        console.warn('could not cache ROM in IndexedDB', e);
      }
    } catch (e) {
      setStatus(e.message, true);
    }
  }

  async function loadCachedRom() {
    try {
      const cached = await dbGet(DB_ROM_KEY);
      if (cached && cached.length === ROM_SIZE) {
        const bytes = new Uint8Array(cached);
        const { title } = validateRom(bytes);
        romBytes = bytes;
        showRomReady(`${title} (remembered from last time)`);
        if (autostart) {
          // back from an online match (web/net-client.js)
          startGame().catch(() => {});
        }
      }
    } catch (e) {
      console.warn('could not load cached ROM', e);
    }
  }

  ui.file.addEventListener('change', () => {
    if (ui.file.files && ui.file.files[0]) {
      acceptRomFile(ui.file.files[0]);
    }
  });

  ui.drop.addEventListener('dragover', (e) => {
    e.preventDefault();
    ui.drop.classList.add('is-dragging');
  });
  ui.drop.addEventListener('dragleave', () => ui.drop.classList.remove('is-dragging'));
  ui.drop.addEventListener('drop', (e) => {
    e.preventDefault();
    ui.drop.classList.remove('is-dragging');
    if (e.dataTransfer.files && e.dataTransfer.files[0]) {
      acceptRomFile(e.dataTransfer.files[0]);
    }
  });

  ui.changeRom.addEventListener('click', async () => {
    romBytes = null;
    ui.start.disabled = true;
    ui.changeRom.hidden = true;
    ui.drop.classList.remove('has-rom');
    ui.romInfo.textContent = '';
    ui.file.value = '';
    try { await dbDelete(DB_ROM_KEY); } catch (e) { /* ignore */ }
    ui.file.click();
  });

  ui.fullscreen.addEventListener('click', () => {
    if (document.fullscreenElement) {
      document.exitFullscreen();
    } else {
      document.documentElement.requestFullscreen().catch(() => {});
    }
  });

  // ---------------------------------------------------------------------------
  // Installing as an app. An app window has no browser UI, so the game gets the whole window and
  // the mouse can reach the top edge, unlike F11 fullscreen with its exit button.

  const appModes = ['standalone', 'fullscreen', 'window-controls-overlay', 'minimal-ui'];
  const isApp = () => appModes.some((m) => window.matchMedia(`(display-mode: ${m})`).matches)
    || navigator.standalone === true;
  let installPrompt = null;

  function updateInstallUi() {
    const app = isApp();
    document.body.classList.toggle('is-app', app);
    $('install-note').hidden = app;
    $('install-app').hidden = app || !installPrompt;
    $('install-app-note').hidden = app || !installPrompt;
    if (installPrompt) {
      $('install-how').textContent = 'install this page as an app';
    }
  }

  async function install() {
    if (!installPrompt) {
      return;
    }
    const prompt = installPrompt;
    installPrompt = null;
    prompt.prompt();
    try { await prompt.userChoice; } catch (e) { /* ignore */ }
    updateInstallUi();
  }

  window.addEventListener('beforeinstallprompt', (e) => {
    e.preventDefault(); // show our own button instead of the mini-infobar
    installPrompt = e;
    updateInstallUi();
  });
  window.addEventListener('appinstalled', () => {
    installPrompt = null;
    toast('Installed. Open Perfect Dark from your apps for a full-window view.');
    updateInstallUi();
  });
  for (const m of appModes) {
    window.matchMedia(`(display-mode: ${m})`).addEventListener('change', updateInstallUi);
  }
  $('install-app').addEventListener('click', install);
  $('install-app-note').addEventListener('click', install);
  if ('serviceWorker' in navigator && window.isSecureContext) {
    navigator.serviceWorker.register('sw.js').catch(() => {});
  }
  updateInstallUi();

  window.addEventListener('gamepadconnected', (e) => {
    toast(`Gamepad connected: ${e.gamepad.id.replace(/\s*\(.*$/, '')}`);
  });
  window.addEventListener('gamepaddisconnected', () => toast('Gamepad disconnected'));

  // keep the browser from acting on keys the game uses (tab focus, page scroll, back navigation)
  const blockedKeys = new Set(['Tab', 'Space', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Backspace', 'Slash', 'Quote']);
  window.addEventListener('keydown', (e) => {
    if (module && blockedKeys.has(e.code)) {
      e.preventDefault();
    }
  });
  ui.canvas.addEventListener('contextmenu', (e) => e.preventDefault());
  ui.canvas.addEventListener('webglcontextlost', () => {
    // online matches recover by themselves (web/net-client.js)
    if (module && !document.body.classList.contains('is-online')) {
      syncSaves();
      ui.errorText.textContent = 'The browser reset the game\x27s graphics (this can happen when the tab is in the background for a while). Your saves are kept; reload the page to continue.';
      ui.error.hidden = false;
    }
  });

  // ---------------------------------------------------------------------------
  // Saves

  // writes /save (eeprom.bin, pd.ini) from memory to IndexedDB; resolves when done
  function syncSaves() {
    if (!module) {
      return Promise.resolve();
    }
    if (syncing) {
      // one is already running; queue another so the latest changes are included
      syncAgain = true;
      return syncing;
    }
    syncing = new Promise((resolve) => {
      module.FS.syncfs(false, (err) => {
        if (err) {
          console.warn('save sync failed', err);
        }
        resolve();
      });
    }).then(() => {
      syncing = null;
      if (syncAgain) {
        syncAgain = false;
        return syncSaves();
      }
    });
    return syncing;
  }

  // writes the game's settings (pd.ini) into /save; the native build only does this on exit,
  // which never happens in a browser tab (see pdWebSaveConfig in port/src/main.c)
  function saveConfig() {
    try {
      if (module && module._pdWebSaveConfig) {
        module._pdWebSaveConfig();
      }
    } catch (e) {
      console.warn('could not save settings', e);
    }
  }

  // called by the game (libultra.c) every time it writes the Game Pak
  let flushTimer = 0;
  function onSaveWritten() {
    clearTimeout(flushTimer);
    flushTimer = setTimeout(syncSaves, SAVE_FLUSH_DELAY_MS);
  }

  // ---------------------------------------------------------------------------
  // Save backups

  function openSaveDb() {
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(IDBFS_DB_NAME, IDBFS_DB_VERSION);
      req.onupgradeneeded = () => {
        // same layout IDBFS creates, so the game can use a database we created
        const db = req.result;
        if (!db.objectStoreNames.contains(IDBFS_STORE)) {
          db.createObjectStore(IDBFS_STORE).createIndex('timestamp', 'timestamp', { unique: false });
        }
      };
      req.onsuccess = () => resolve(req.result);
      req.onerror = () => reject(req.error);
    });
  }

  async function readSaveFiles() {
    const db = await openSaveDb();
    try {
      return await new Promise((resolve, reject) => {
        const files = [];
        const req = db.transaction(IDBFS_STORE, 'readonly').objectStore(IDBFS_STORE).openCursor();
        req.onsuccess = () => {
          const cursor = req.result;
          if (!cursor) {
            resolve(files);
            return;
          }
          const entry = cursor.value;
          if (entry && entry.contents) {
            files.push({ path: String(cursor.key), mode: entry.mode, timestamp: entry.timestamp, contents: new Uint8Array(entry.contents) });
          }
          cursor.continue();
        };
        req.onerror = () => reject(req.error);
      });
    } finally {
      db.close();
    }
  }

  async function replaceSaveFiles(files) {
    const db = await openSaveDb();
    try {
      await new Promise((resolve, reject) => {
        const tx = db.transaction(IDBFS_STORE, 'readwrite');
        const store = tx.objectStore(IDBFS_STORE);
        store.clear();
        for (const f of files) {
          store.put({ timestamp: f.timestamp, mode: f.mode, contents: f.contents }, f.path);
        }
        tx.oncomplete = () => resolve();
        tx.onerror = () => reject(tx.error);
      });
    } finally {
      db.close();
    }
  }

  function toBase64(bytes) {
    let bin = '';
    for (let i = 0; i < bytes.length; i += 0x8000) {
      bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    }
    return btoa(bin);
  }

  function fromBase64(str) {
    const bin = atob(str);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) {
      bytes[i] = bin.charCodeAt(i);
    }
    return bytes;
  }

  function setSavesStatus(text, isError) {
    ui.savesStatus.textContent = text || '';
    ui.savesStatus.classList.toggle('warn', !!isError);
  }

  async function exportSaves() {
    try {
      // make sure what's in IndexedDB includes the game's latest writes
      saveConfig();
      await syncSaves();
      const files = await readSaveFiles();
      if (!files.length) {
        setSavesStatus('There are no saves to back up yet.', true);
        if (module) {
          toast('There are no saves to back up yet');
        }
        return;
      }
      const backup = {
        format: BACKUP_FORMAT,
        version: 1,
        exported: new Date().toISOString(),
        origin: location.origin,
        files: files.map((f) => ({
          path: f.path,
          mode: f.mode,
          mtime: new Date(f.timestamp).toISOString(),
          data: toBase64(f.contents),
        })),
      };
      const blob = new Blob([JSON.stringify(backup, null, 2)], { type: 'application/json' });
      const a = document.createElement('a');
      a.href = URL.createObjectURL(blob);
      a.download = `perfect-dark-saves-${new Date().toISOString().slice(0, 10)}.json`;
      document.body.appendChild(a);
      a.click();
      a.remove();
      setTimeout(() => URL.revokeObjectURL(a.href), 10000);
      setSavesStatus(`Backed up ${files.length} file${files.length === 1 ? '' : 's'}.`);
      if (module) {
        toast('Saves backed up');
      }
    } catch (e) {
      setSavesStatus(`Backup failed: ${e.message || e}`, true);
    }
  }

  async function importSaves(file) {
    if (module) {
      // the running game would overwrite the restored files with what it has in memory
      setSavesStatus('Reload the page to restore a backup.', true);
      return;
    }
    try {
      const backup = JSON.parse(await file.text());
      if (!backup || backup.format !== BACKUP_FORMAT || !Array.isArray(backup.files)) {
        throw new Error('this is not a Perfect Dark save backup');
      }
      const files = backup.files.map((f) => {
        if (typeof f.path !== 'string' || !f.path.startsWith(SAVE_DIR + '/') || f.path.includes('..')) {
          throw new Error(`unexpected file in backup: ${f.path}`);
        }
        return {
          path: f.path,
          mode: f.mode || 0o100644,
          timestamp: f.mtime ? new Date(f.mtime) : new Date(),
          contents: fromBase64(f.data),
        };
      });
      const when = backup.exported ? new Date(backup.exported).toLocaleString() : 'an unknown date';
      if (!confirm(`Restore ${files.length} file(s) from the backup made ${when}?\n\nThis replaces the saves and settings currently in this browser.`)) {
        return;
      }
      await replaceSaveFiles(files);
      setSavesStatus(`Restored ${files.length} file${files.length === 1 ? '' : 's'}. Press Start to play.`);
    } catch (e) {
      setSavesStatus(`Restore failed: ${e.message || e}`, true);
    }
  }

  ui.exportSaves.addEventListener('click', exportSaves);
  ui.backupIngame.addEventListener('click', () => {
    exportSaves();
    ui.canvas.focus();
  });
  ui.importSaves.addEventListener('click', () => ui.importFile.click());
  ui.importFile.addEventListener('change', () => {
    if (ui.importFile.files && ui.importFile.files[0]) {
      importSaves(ui.importFile.files[0]);
    }
    ui.importFile.value = '';
  });

  // ---------------------------------------------------------------------------
  // Game startup

  function extraArgs() {
    // eg. ?args=--skip-intro%20--log for testing
    const args = new URLSearchParams(location.search).get('args');
    return args ? args.split(/\s+/).filter(Boolean) : [];
  }

  // the game script is loaded on demand, versioned by the server's build id so a stale cached copy
  // never runs against a newer server (online matches need the exact same build everywhere)
  let gameScript = null;
  function loadGameScript() {
    if (!gameScript) {
      gameScript = (async () => {
        let build = '';
        try {
          build = (await (await fetch('server-config.json', { cache: 'no-store' })).json()).build || '';
        } catch {
          // plain static hosting
        }
        await new Promise((resolve, reject) => {
          const s = document.createElement('script');
          s.src = build ? `pd.js?v=${build}` : 'pd.js';
          s.onload = resolve;
          s.onerror = () => reject(new Error('could not load pd.js'));
          document.head.appendChild(s);
        });
        return build;
      })();
    }
    return gameScript;
  }

  /**
   * Starts the game. opts (used by online matches, web/net-client.js):
   *   extraArgs: more command line arguments
   *   files: { path: text } written to the virtual filesystem before start
   *   hooks: properties added to the Emscripten module (eg. onNetHash)
   * Returns the module; throws on failure.
   */
  async function startGame(opts) {
    opts = opts || {};
    if (!romBytes || module) {
      throw new Error(module ? 'the game is already running' : 'no ROM selected');
    }

    ui.start.disabled = true;
    setStatus('Loading game…');

    // ask the browser not to evict our IndexedDB (saves) when it's low on space
    if (navigator.storage && navigator.storage.persist) {
      navigator.storage.persist().catch(() => {});
    }

    const build = await loadGameScript();

    const config = {
      canvas: ui.canvas,
      // keyboard, mouse, gamepads, audio and the canvas size (web/pd-host.js)
      pdhost: window.PDHost.create(ui.canvas),
      arguments: ['--basedir', '/data', '--savedir', SAVE_DIR, ...extraArgs(), ...(opts.extraArgs || [])],
      locateFile: (p) => (build ? `${p}?v=${build}` : p),
      ...(opts.hooks || {}),
      print: (text) => console.log(text),
      printErr: (text) => console.warn(text),
      onFatalError: (text) => {
        ui.errorText.textContent = text;
        ui.error.hidden = false;
      },
      onSaveWritten,
      preRun: [
        (mod) => {
          const FS = mod.FS;
          FS.mkdir('/data');
          FS.writeFile(ROM_FS_PATH, romBytes);
          romBytes = null; // the copy in MEMFS is the one the game uses now

          for (const [file, text] of Object.entries(opts.files || {})) {
            const dir = file.slice(0, file.lastIndexOf('/'));
            if (dir && !FS.analyzePath(dir).exists) {
              FS.mkdirTree(dir);
            }
            FS.writeFile(file, text);
          }

          FS.mkdir(SAVE_DIR);
          FS.mount(mod.IDBFS, {}, SAVE_DIR);
          mod.addRunDependency('idbfs-load');
          FS.syncfs(true, (err) => {
            if (err) {
              console.warn('could not load saves', err);
            }
            mod.removeRunDependency('idbfs-load');
          });
        },
      ],
      setStatus: (text) => {
        if (text) {
          setStatus(text);
        }
      },
    };

    try {
      module = await createPerfectDark(config);
    } catch (e) {
      setStatus(`Failed to start: ${e.message || e}`, true);
      ui.start.disabled = false;
      throw e;
    }

    ui.overlay.hidden = true;
    document.body.classList.add('is-running');
    ui.canvas.focus();

    setInterval(syncSaves, SAVE_SYNC_INTERVAL_MS);
    setInterval(saveConfig, CONFIG_SAVE_INTERVAL_MS);
    const saveEverything = () => {
      saveConfig();
      syncSaves();
    };
    document.addEventListener('visibilitychange', () => {
      if (document.visibilityState === 'hidden') {
        saveEverything();
      }
    });
    window.addEventListener('pagehide', saveEverything);

    return module;
  }

  ui.start.addEventListener('click', () => startGame().catch(() => {}));

  // for the online client (web/net-client.js)
  window.PDWeb = {
    romBytes: () => romBytes,
    startGame,
    // writes settings and saves to browser storage, eg. before the page reloads
    flush: async () => {
      saveConfig();
      await syncSaves();
      await syncSaves(); // includes changes made while an earlier sync was running
    },
    // start the game without the Start button after the next reload
    autostartNextLoad: () => {
      try { sessionStorage.setItem(AUTOSTART_KEY, '1'); } catch (e) { /* ignore */ }
    },
    showOverlay: () => {
      ui.overlay.hidden = false;
      document.body.classList.remove('is-running');
    },
  };

  // the Gamepad API is only available in secure contexts (https:// or localhost)
  if (!window.isSecureContext) {
    fetch('server-config.json')
      .then((r) => r.json())
      .then(({ httpsPort }) => {
        if (!httpsPort) {
          return;
        }
        const url = `https://${location.hostname}:${httpsPort}${location.pathname}${location.search}`;
        const link = $('secure-link');
        link.href = url;
        link.textContent = url;
        $('secure-note').hidden = false;
      })
      .catch(() => {});
  }

  loadCachedRom();
})();
