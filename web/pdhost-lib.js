// Emscripten library: the pdhost_* imports of pd.wasm (port/include/pdhost.h).
//
// In the browser they forward to Module.pdhost (web/pd-host.js). Without one (the multiplayer
// server's headless instance in Node) they report no input, no window and no audio. A native host
// that runs pd.wasm implements the same imports itself.

addToLibrary({
  pdhost_now_us: () => performance.now() * 1000,

  pdhost_poll_event: (ptr) => {
    const host = Module.pdhost;
    const ev = host && host.pollEvent();
    if (!ev) {
      return 0;
    }
    for (let i = 0; i < 8; i++) {
      HEAP32[(ptr >> 2) + i] = ev[i] | 0;
    }
    return 1;
  },

  pdhost_text_input: (on) => {
    if (Module.pdhost) Module.pdhost.textInput(!!on);
  },

  pdhost_show_cursor: (on) => {
    if (Module.pdhost) Module.pdhost.showCursor(!!on);
  },

  pdhost_gl_create__deps: ['$GL'],
  pdhost_gl_create: (depth, stencil) => {
    const canvas = Module.canvas;
    if (!canvas) {
      return 0;
    }
    const handle = GL.createContext(canvas, {
      majorVersion: 2,
      minorVersion: 0,
      depth: depth > 0,
      stencil: stencil > 0,
      alpha: false,
      antialias: false,
      premultipliedAlpha: false,
      preserveDrawingBuffer: false,
      powerPreference: 'high-performance',
    });
    if (!handle) {
      return 0;
    }
    GL.makeContextCurrent(handle);
    return 1;
  },

  // the browser presents the frame when the game yields to it
  pdhost_gl_swap: () => {},

  pdhost_gl_context_lost__deps: ['$GL'],
  pdhost_gl_context_lost: () => (GL.currentContext && GL.currentContext.GLctx.isContextLost() ? 1 : 0),

  pdhost_drawable_size: (w, h) => {
    const canvas = Module.canvas;
    HEAP32[w >> 2] = canvas ? canvas.width : 0;
    HEAP32[h >> 2] = canvas ? canvas.height : 0;
  },

  pdhost_pad_button: (id, button) => (Module.pdhost ? Module.pdhost.padButton(id, button) : 0),
  pdhost_pad_axis: (id, axis) => (Module.pdhost ? Module.pdhost.padAxis(id, axis) : 0),
  pdhost_pad_name__deps: ['$stringToUTF8'],
  pdhost_pad_name: (id, buf, size) => {
    const name = Module.pdhost ? Module.pdhost.padName(id) : '';
    stringToUTF8(name, buf, size);
    return name.length;
  },
  pdhost_pad_type: (id) => (Module.pdhost ? Module.pdhost.padType(id) : 0),
  pdhost_pad_rumble: (id, lo, hi, ms) => (Module.pdhost ? Module.pdhost.padRumble(id, lo, hi, ms) : -1),

  pdhost_audio_open: (freq, channels) => (Module.pdhost ? Module.pdhost.audioOpen(freq, channels) : 0),
  pdhost_audio_queue: (ptr, bytes) => {
    if (Module.pdhost) {
      Module.pdhost.audioQueue(HEAP16.slice(ptr >> 1, (ptr + bytes) >> 1));
    }
  },
  pdhost_audio_queued: () => (Module.pdhost ? Module.pdhost.audioQueued() : 0),
  pdhost_audio_pause: (on) => {
    if (Module.pdhost) Module.pdhost.audioPause(!!on);
  },

  pdhost_fatal__deps: ['$UTF8ToString'],
  pdhost_fatal: (msg) => {
    if (Module.onFatalError) Module.onFatalError(UTF8ToString(msg));
  },
  // the file only lives in memory until the page flushes it to IndexedDB; ask for that now
  pdhost_save_written: () => {
    if (Module.onSaveWritten) Module.onSaveWritten();
  },
  pdhost_net_hash: (tick, hash) => {
    if (Module.onNetHash) Module.onNetHash(tick >>> 0, hash >>> 0);
  },
  pdhost_net_local_input: (ptr) => {
    if (Module.onNetLocalInput) Module.onNetLocalInput(ptr);
  },
});
