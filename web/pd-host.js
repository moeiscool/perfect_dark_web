// The browser's implementation of pd.wasm's host interface (port/include/pdhost.h): keyboard,
// mouse, gamepads (Gamepad API), audio (Web Audio) and the canvas size. web/pdhost-lib.js calls
// into the object made by PDHost.create(); pd-web.js passes it to the game as Module.pdhost.
(() => {
  'use strict';

  // event types (PDHOST_EV_*)
  const EV = {
    KEYDOWN: 1, KEYUP: 2, TEXT: 3, MOUSEMOVE: 4, MOUSEDOWN: 5, MOUSEUP: 6, WHEEL: 7,
    PADADDED: 8, PADREMOVED: 9, PADBUTTONDOWN: 10, PADBUTTONUP: 11, PADAXIS: 12, RESIZE: 13, QUIT: 14,
  };
  const MAX_PADS = 8;

  // KeyboardEvent.code -> SDL scancode
  const SCANCODES = {
    Enter: 40, Escape: 41, Backspace: 42, Tab: 43, Space: 44, Minus: 45, Equal: 46, BracketLeft: 47,
    BracketRight: 48, Backslash: 49, Semicolon: 51, Quote: 52, Backquote: 53, Comma: 54, Period: 55,
    Slash: 56, CapsLock: 57, PrintScreen: 70, ScrollLock: 71, Pause: 72, Insert: 73, Home: 74,
    PageUp: 75, Delete: 76, End: 77, PageDown: 78, ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81,
    ArrowUp: 82, NumLock: 83, NumpadDivide: 84, NumpadMultiply: 85, NumpadSubtract: 86, NumpadAdd: 87,
    NumpadEnter: 88, Numpad0: 98, NumpadDecimal: 99, IntlBackslash: 100, ContextMenu: 101, NumpadEqual: 103,
    ControlLeft: 224, ShiftLeft: 225, AltLeft: 226, MetaLeft: 227, ControlRight: 228, ShiftRight: 229,
    AltRight: 230, MetaRight: 231,
  };
  for (let i = 0; i < 26; i++) SCANCODES[`Key${String.fromCharCode(65 + i)}`] = 4 + i;
  for (let i = 1; i <= 9; i++) SCANCODES[`Digit${i}`] = 29 + i;
  SCANCODES.Digit0 = 39;
  for (let i = 1; i <= 12; i++) SCANCODES[`F${i}`] = 57 + i;
  for (let i = 1; i <= 9; i++) SCANCODES[`Numpad${i}`] = 88 + i;

  // SDL KMOD_*
  function modifiers(e) {
    let m = 0;
    if (e.getModifierState) {
      if (e.shiftKey) m |= 0x0001;
      if (e.ctrlKey) m |= 0x0040;
      if (e.altKey) m |= 0x0100;
      if (e.metaKey) m |= 0x0400;
      if (e.getModifierState('NumLock')) m |= 0x1000;
      if (e.getModifierState('CapsLock')) m |= 0x2000;
    }
    return m;
  }

  // W3C "standard" gamepad button -> SDL_GameControllerButton (6 and 7 are the triggers, axes 4 and 5)
  const PAD_BUTTONS = { 0: 0, 1: 1, 2: 2, 3: 3, 4: 9, 5: 10, 8: 4, 9: 6, 10: 7, 11: 8, 12: 11, 13: 12, 14: 13, 15: 14, 16: 5, 17: 20 };

  function padType(id) {
    if (/054c/i.test(id) || /playstation|dualshock|dualsense/i.test(id)) {
      return /0ce6|0df2|dualsense/i.test(id) ? 7 : 4;
    }
    if (/045e|xbox|xinput/i.test(id)) return 1;
    if (/057e|pro controller|nintendo/i.test(id)) return 5;
    return 0;
  }

  function create(canvas) {
    const events = [];
    const push = (...ev) => {
      if (events.length < 512) events.push(ev);
    };
    const pads = new Array(MAX_PADS).fill(null); // { buttons: [...21], axes: [...6], name, type, gp }
    let textOn = false;
    let lastPadScan = 0;

    // ---- canvas size: the drawable is the canvas' CSS size (the page lays it out)
    function resize() {
      const w = Math.max(1, Math.round(canvas.clientWidth));
      const h = Math.max(1, Math.round(canvas.clientHeight));
      if (canvas.width !== w || canvas.height !== h) {
        canvas.width = w;
        canvas.height = h;
        push(EV.RESIZE, w, h);
      }
    }
    resize();
    new ResizeObserver(resize).observe(canvas);
    window.addEventListener('resize', resize);

    // ---- keyboard
    const isFormField = (t) => t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.isContentEditable);
    window.addEventListener('keydown', (e) => {
      if (isFormField(e.target)) return;
      const sc = SCANCODES[e.code];
      if (sc) push(EV.KEYDOWN, sc, modifiers(e), e.repeat ? 1 : 0);
      if (textOn && e.key && e.key.length === 1 && !e.ctrlKey && !e.metaKey && e.key.charCodeAt(0) < 0x80) {
        push(EV.TEXT, e.key.charCodeAt(0));
      }
    });
    window.addEventListener('keyup', (e) => {
      if (isFormField(e.target)) return;
      const sc = SCANCODES[e.code];
      if (sc) push(EV.KEYUP, sc, modifiers(e));
    });
    // released keys can't be seen while the window is in the background
    window.addEventListener('blur', () => {
      for (const sc of new Set(Object.values(SCANCODES))) push(EV.KEYUP, sc, 0);
    });

    // ---- mouse: positions in drawable pixels, clamped to the canvas so the screen edges still
    // register when the canvas doesn't fill the window (free aim turns the view at the edges)
    function mousePos(e) {
      const r = canvas.getBoundingClientRect();
      const x = Math.min(Math.max(e.clientX - r.left, 0), r.width - 1) * (canvas.width / Math.max(1, r.width));
      const y = Math.min(Math.max(e.clientY - r.top, 0), r.height - 1) * (canvas.height / Math.max(1, r.height));
      return [Math.round(x), Math.round(y)];
    }
    const sdlButton = (b) => [1, 2, 3, 4, 5][b] || 0;
    window.addEventListener('mousemove', (e) => push(EV.MOUSEMOVE, ...mousePos(e)));
    canvas.addEventListener('mousedown', (e) => {
      canvas.focus();
      push(EV.MOUSEDOWN, sdlButton(e.button), ...mousePos(e));
    });
    window.addEventListener('mouseup', (e) => push(EV.MOUSEUP, sdlButton(e.button), ...mousePos(e)));
    canvas.addEventListener('wheel', (e) => {
      e.preventDefault();
      const y = e.deltaY < 0 ? 1 : e.deltaY > 0 ? -1 : 0;
      const x = e.deltaX > 0 ? 1 : e.deltaX < 0 ? -1 : 0;
      if (x || y) push(EV.WHEEL, x, y);
    }, { passive: false });
    canvas.addEventListener('contextmenu', (e) => e.preventDefault());

    // ---- gamepads (polled; the browser only reports buttons through getGamepads())
    function scanPads(force) {
      const now = performance.now();
      if (!force && now - lastPadScan < 4) return;
      lastPadScan = now;
      const list = navigator.getGamepads ? navigator.getGamepads() : [];
      const seen = new Array(MAX_PADS).fill(false);
      for (const gp of list) {
        if (!gp || !gp.connected || gp.index >= MAX_PADS) continue;
        const id = gp.index;
        seen[id] = true;
        let pad = pads[id];
        if (!pad) {
          pad = pads[id] = {
            buttons: new Array(21).fill(0),
            axes: new Array(6).fill(0),
            name: gp.id.replace(/\s*\(.*$/, '').slice(0, 60) || 'Gamepad',
            type: padType(gp.id),
            gp,
          };
          push(EV.PADADDED, id);
        }
        pad.gp = gp;
        for (const [std, sdl] of Object.entries(PAD_BUTTONS)) {
          const b = gp.buttons[std];
          const v = b && b.pressed ? 1 : 0;
          if (v !== pad.buttons[sdl]) {
            pad.buttons[sdl] = v;
            push(v ? EV.PADBUTTONDOWN : EV.PADBUTTONUP, id, sdl);
          }
        }
        const axis = (v) => Math.max(-32768, Math.min(32767, Math.round((v || 0) * 32767)));
        const values = [
          axis(gp.axes[0]), axis(gp.axes[1]), axis(gp.axes[2]), axis(gp.axes[3]),
          axis(gp.buttons[6] ? gp.buttons[6].value : 0), axis(gp.buttons[7] ? gp.buttons[7].value : 0),
        ];
        values.forEach((v, a) => {
          if (v !== pad.axes[a]) {
            pad.axes[a] = v;
            push(EV.PADAXIS, id, a, v);
          }
        });
      }
      for (let id = 0; id < MAX_PADS; id++) {
        if (pads[id] && !seen[id]) {
          pads[id] = null;
          push(EV.PADREMOVED, id);
        }
      }
    }

    // ---- audio: a queue of the game's samples, played (and resampled) by a ScriptProcessor
    const audio = { ctx: null, node: null, rate: 0, channels: 2, chunks: [], offset: 0, queuedFrames: 0, pos: 0, paused: true };

    function audioOpen(freq, channels) {
      if (audio.ctx) return audio.rate;
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) return 0;
      try {
        audio.ctx = new Ctx({ sampleRate: freq, latencyHint: 'interactive' });
      } catch {
        audio.ctx = new Ctx({ latencyHint: 'interactive' });
      }
      audio.rate = freq;
      audio.channels = channels;
      const step = freq / audio.ctx.sampleRate;
      const node = audio.ctx.createScriptProcessor(1024, 0, 2);
      node.onaudioprocess = (e) => {
        const l = e.outputBuffer.getChannelData(0);
        const r = e.outputBuffer.getChannelData(1);
        for (let i = 0; i < l.length; i++) {
          if (audio.paused || !audio.chunks.length) {
            l[i] = r[i] = 0;
            continue;
          }
          const chunk = audio.chunks[0];
          const frame = audio.offset;
          l[i] = chunk[frame * audio.channels] / 32768;
          r[i] = chunk[frame * audio.channels + (audio.channels > 1 ? 1 : 0)] / 32768;
          audio.pos += step;
          while (audio.pos >= 1 && audio.chunks.length) {
            audio.pos -= 1;
            audio.offset++;
            audio.queuedFrames--;
            if (audio.offset * audio.channels >= audio.chunks[0].length) {
              audio.chunks.shift();
              audio.offset = 0;
            }
          }
        }
      };
      node.connect(audio.ctx.destination);
      audio.node = node;
      // browsers start audio only after the player interacts with the page
      const resume = () => {
        if (audio.ctx.state !== 'running') audio.ctx.resume().catch(() => {});
      };
      for (const t of ['keydown', 'mousedown', 'touchstart', 'pointerdown']) {
        window.addEventListener(t, resume);
      }
      window.addEventListener('gamepadconnected', resume);
      resume();
      return freq;
    }

    const host = {
      pollEvent() {
        if (!events.length) scanPads(false);
        const ev = events.shift();
        if (!ev) return null;
        while (ev.length < 8) ev.push(0);
        return ev;
      },
      textInput(on) { textOn = on; },
      showCursor(on) { canvas.style.cursor = on ? '' : 'none'; },
      padButton(id, b) {
        scanPads(false);
        return pads[id] ? pads[id].buttons[b] || 0 : 0;
      },
      padAxis(id, a) {
        scanPads(false);
        return pads[id] ? pads[id].axes[a] || 0 : 0;
      },
      padName(id) { return pads[id] ? pads[id].name : ''; },
      padType(id) { return pads[id] ? pads[id].type : 0; },
      padRumble(id, lo, hi, ms) {
        const gp = pads[id] && pads[id].gp;
        const act = gp && gp.vibrationActuator;
        if (!act || !act.playEffect) return -1;
        act.playEffect('dual-rumble', { duration: ms, strongMagnitude: lo / 65535, weakMagnitude: hi / 65535 }).catch(() => {});
        return 0;
      },
      audioOpen,
      audioQueue(samples) {
        if (!audio.ctx) return;
        audio.chunks.push(samples);
        audio.queuedFrames += samples.length / audio.channels;
      },
      audioQueued() {
        return Math.max(0, Math.round(audio.queuedFrames)) * audio.channels * 2;
      },
      audioPause(on) { audio.paused = on; },
      // for troubleshooting from the console: PDHost.current.debug()
      debug: () => ({ audio: audio.ctx && audio.ctx.state, audioRate: audio.ctx && audio.ctx.sampleRate, queuedFrames: Math.round(audio.queuedFrames), pads: pads.map((p) => p && p.name) }),
    };
    window.PDHost.current = host;
    return host;
  }

  window.PDHost = { create };
})();
