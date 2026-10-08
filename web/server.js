#!/usr/bin/env node
// Static web server for the Perfect Dark WebAssembly build. No dependencies; Node 18+.
//
//   node web/server.js [--root build-web] [--port 8080] [--https-port 8443] [--host 0.0.0.0]
//                      [--cert cert.pem --key key.pem] [--no-https]
//                      [--rom pd.z64] [--max-rooms 4] [--no-lobby] [--lobby-url host]
//
// The online lobby (WebSocket /net) runs in the same server unless --no-lobby is given. With
// --lobby-url, the game's Online menu uses that lobby server by default instead of this one (eg.
// the game on one host name and the lobby on another, each its own server process).
//
// Serves index.html, pd-web.js, pd.js and pd.wasm from the build directory. ROM files are
// never served: players load their own ROM in the browser.
//
// Browsers only expose gamepads to secure contexts (https:// or http://localhost), so when
// the game is opened from another machine it has to be over HTTPS. Unless --cert/--key are
// given, a self-signed certificate is generated with openssl into web/.cert/ on first run;
// the browser will ask you to accept it once.

'use strict';

const http = require('node:http');
const https = require('node:https');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const os = require('node:os');
const { execFileSync } = require('node:child_process');

function arg(name, fallback) {
  const i = process.argv.indexOf(`--${name}`);
  return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
}

const ROOT = path.resolve(arg('root', process.env.PD_WEB_ROOT || path.join(__dirname, '..', 'build-web')));
const PORT = parseInt(arg('port', process.env.PORT || '8080'), 10);
const HTTPS_PORT = parseInt(arg('https-port', process.env.HTTPS_PORT || '8443'), 10);
const HOST = arg('host', process.env.HOST || '0.0.0.0');
const USE_HTTPS = !process.argv.includes('--no-https');
const CERT_DIR = path.join(__dirname, '.cert');
const ROM_PATH = arg('rom', process.env.PD_ROM);
const MAX_ROOMS = parseInt(arg('max-rooms', process.env.PD_MAX_ROOMS || '4'), 10);
const LOBBY_URL = arg('lobby-url', process.env.PD_LOBBY_URL || '');
const NO_LOBBY = process.argv.includes('--no-lobby');

// Online multiplayer lobby (WebSocket /net). Needs the 'ws' package (npm install in web/) and,
// to run matches, the ROM (--rom), which stays on the server and is never served.
let lobby = null;
if (!NO_LOBBY) {
  try {
    const { Lobby } = require('./net/lobby.js');
    lobby = new Lobby({ buildDir: ROOT, romPath: ROM_PATH, maxRooms: MAX_ROOMS, log: (m) => console.log(`${new Date().toISOString()} ${m}`) });
  } catch (e) {
    console.warn(`warning: multiplayer lobby disabled: ${e.message}`);
  }
}

// id of the game build (the pages load pd.js?v=<id>; clients and lobby must run the same build)
let buildCache = { mtimeMs: 0, id: null };
function buildId() {
  try {
    const wasm = path.join(ROOT, 'pd.wasm');
    const stat = fs.statSync(wasm);
    if (stat.mtimeMs !== buildCache.mtimeMs) {
      const id = require('node:crypto').createHash('sha1').update(fs.readFileSync(wasm)).digest('hex').slice(0, 12);
      buildCache = { mtimeMs: stat.mtimeMs, id };
    }
  } catch {
    buildCache = { mtimeMs: 0, id: null };
  }
  return buildCache.id;
}

const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.data': 'application/octet-stream',
  '.json': 'application/json; charset=utf-8',
  '.webmanifest': 'application/manifest+json; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.png': 'image/png',
  '.ico': 'image/x-icon',
  '.svg': 'image/svg+xml',
  '.txt': 'text/plain; charset=utf-8',
};

const COMPRESSIBLE = new Set(['.html', '.js', '.mjs', '.wasm', '.json', '.webmanifest', '.css', '.svg', '.txt', '.data']);
const BLOCKED = /\.(z64|v64|n64|rom|gbc|ini|log)$/i;

// path -> { mtimeMs, raw, br, gzip }
const cache = new Map();

function loadFile(filePath, stat) {
  const cached = cache.get(filePath);
  if (cached && cached.mtimeMs === stat.mtimeMs && cached.size === stat.size) {
    return cached;
  }
  const raw = fs.readFileSync(filePath);
  const entry = { mtimeMs: stat.mtimeMs, size: stat.size, raw, br: null, gzip: null };
  cache.set(filePath, entry);
  return entry;
}

function compressed(entry, encoding) {
  if (encoding === 'br') {
    if (!entry.br) {
      entry.br = zlib.brotliCompressSync(entry.raw, {
        params: { [zlib.constants.BROTLI_PARAM_QUALITY]: 9, [zlib.constants.BROTLI_PARAM_SIZE_HINT]: entry.raw.length },
      });
    }
    return entry.br;
  }
  if (!entry.gzip) {
    entry.gzip = zlib.gzipSync(entry.raw, { level: 9 });
  }
  return entry.gzip;
}

function send(res, status, body, headers = {}) {
  res.writeHead(status, { 'Content-Type': 'text/plain; charset=utf-8', ...headers });
  res.end(body);
}

function handler(req, res) {
  const started = Date.now();
  res.on('finish', () => {
    console.log(`${new Date().toISOString()} ${req.socket.remoteAddress} ${req.method} ${req.url} ${res.statusCode} ${Date.now() - started}ms`);
  });

  if (req.method !== 'GET' && req.method !== 'HEAD') {
    return send(res, 405, 'Method Not Allowed', { Allow: 'GET, HEAD' });
  }

  let urlPath;
  try {
    urlPath = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
  } catch {
    return send(res, 400, 'Bad Request');
  }
  if (urlPath === '/server-config.json') {
    // lets the page link to the HTTPS port when it was opened over plain HTTP
    if (lobby) {
      lobby.reloadBuild(); // the build may have been replaced since start-up
    }
    return send(res, 200, JSON.stringify({
      httpsPort: tls ? HTTPS_PORT : null,
      build: buildId(),
      online: !!(lobby && lobby.enabled) || !!LOBBY_URL,
      // default lobby server for the game's Online menu (null = this server)
      lobby: LOBBY_URL || null,
    }), {
      'Content-Type': 'application/json; charset=utf-8',
      'Cache-Control': 'no-cache',
    });
  }
  if (urlPath.endsWith('/')) {
    urlPath += 'index.html';
  }

  const filePath = path.normalize(path.join(ROOT, urlPath));
  if (!filePath.startsWith(ROOT + path.sep)) {
    return send(res, 403, 'Forbidden');
  }
  if (BLOCKED.test(filePath)) {
    return send(res, 403, 'Forbidden');
  }

  let stat;
  try {
    stat = fs.statSync(filePath);
  } catch {
    return send(res, 404, 'Not Found');
  }
  if (!stat.isFile()) {
    return send(res, 404, 'Not Found');
  }

  const ext = path.extname(filePath).toLowerCase();
  if (path.basename(filePath) === 'index.html') {
    return sendPage(req, res, filePath, stat);
  }

  const entry = loadFile(filePath, stat);
  const headers = {
    'Content-Type': MIME[ext] || 'application/octet-stream',
    // the build output isn't content-hashed, so always revalidate
    'Cache-Control': 'no-cache',
    'Last-Modified': new Date(stat.mtimeMs).toUTCString(),
    // cross-origin isolation: harmless now, required if the build ever uses pthreads
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin',
    'X-Content-Type-Options': 'nosniff',
    Vary: 'Accept-Encoding',
  };

  const ims = req.headers['if-modified-since'];
  if (ims && Math.floor(stat.mtimeMs / 1000) <= Math.floor(Date.parse(ims) / 1000)) {
    res.writeHead(304, headers);
    return res.end();
  }

  let body = entry.raw;
  if (COMPRESSIBLE.has(ext) && entry.raw.length > 1024) {
    const accept = req.headers['accept-encoding'] || '';
    const encoding = /\bbr\b/.test(accept) ? 'br' : /\bgzip\b/.test(accept) ? 'gzip' : null;
    if (encoding) {
      body = compressed(entry, encoding);
      headers['Content-Encoding'] = encoding;
    }
  }
  headers['Content-Length'] = body.length;

  res.writeHead(200, headers);
  res.end(req.method === 'HEAD' ? undefined : body);
}

// Adds ?v=<content hash> to the page's script URLs. Proxies and CDNs (Cloudflare sets its own
// browser cache time on .js files) can keep serving an old script; with versioned URLs a page
// never runs an old script against a newer game.
function versionScripts(html, dir) {
  return html.replace(/<script src="([^"?:]+\.js)"><\/script>/g, (tag, src) => {
    const file = path.join(dir, src);
    try {
      const stat = fs.statSync(file);
      const hash = require('node:crypto').createHash('sha1').update(loadFile(file, stat).raw).digest('hex').slice(0, 10);
      return `<script src="${src}?v=${hash}"></script>`;
    } catch {
      return tag;
    }
  });
}

function sendPage(req, res, filePath, stat) {
  const html = Buffer.from(versionScripts(loadFile(filePath, stat).raw.toString('utf8'), path.dirname(filePath)));
  const etag = `"${require('node:crypto').createHash('sha1').update(html).digest('hex').slice(0, 16)}"`;
  const headers = {
    'Content-Type': MIME['.html'],
    'Cache-Control': 'no-cache',
    ETag: etag,
    'Cross-Origin-Opener-Policy': 'same-origin',
    'Cross-Origin-Embedder-Policy': 'require-corp',
    'Cross-Origin-Resource-Policy': 'same-origin',
    'X-Content-Type-Options': 'nosniff',
  };
  if (req.headers['if-none-match'] === etag) {
    res.writeHead(304, headers);
    return res.end();
  }
  headers['Content-Length'] = html.length;
  res.writeHead(200, headers);
  return res.end(req.method === 'HEAD' ? undefined : html);
}

// Returns { key, cert } for the HTTPS server, or null if none could be loaded or generated.
function loadTlsCredentials() {
  let keyPath = arg('key');
  let certPath = arg('cert');

  if (!keyPath || !certPath) {
    keyPath = path.join(CERT_DIR, 'key.pem');
    certPath = path.join(CERT_DIR, 'cert.pem');
    if (!fs.existsSync(keyPath) || !fs.existsSync(certPath)) {
      const ips = Object.values(os.networkInterfaces()).flat()
        .filter((a) => a && a.family === 'IPv4').map((a) => `IP:${a.address}`);
      const san = ['DNS:localhost', `DNS:${os.hostname()}`, ...ips].join(',');
      try {
        fs.mkdirSync(CERT_DIR, { recursive: true });
        execFileSync('openssl', [
          'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-sha256', '-days', '3650',
          '-subj', '/CN=Perfect Dark Web', '-addext', `subjectAltName=${san}`,
          '-keyout', keyPath, '-out', certPath,
        ], { stdio: 'ignore' });
        console.log(`generated self-signed certificate in ${CERT_DIR}`);
      } catch (e) {
        console.warn(`warning: could not generate a certificate with openssl (${e.message}); HTTPS disabled`);
        return null;
      }
    }
  }

  try {
    return { key: fs.readFileSync(keyPath), cert: fs.readFileSync(certPath) };
  } catch (e) {
    console.warn(`warning: could not read TLS key/cert (${e.message}); HTTPS disabled`);
    return null;
  }
}

function listAddresses() {
  return HOST === '0.0.0.0'
    ? Object.values(os.networkInterfaces()).flat().filter((a) => a && a.family === 'IPv4').map((a) => a.address)
    : [HOST];
}

if (!fs.existsSync(path.join(ROOT, 'pd.wasm'))) {
  console.warn(`warning: ${path.join(ROOT, 'pd.wasm')} not found; build the game first (web/build.sh)`);
}

console.log(`Perfect Dark web server serving ${ROOT}`);

function attachLobby(server) {
  server.on('upgrade', (req, socket, head) => {
    if (lobby && new URL(req.url, 'http://localhost').pathname === '/net') {
      lobby.handleUpgrade(req, socket, head);
    } else {
      socket.destroy();
    }
  });
  return server;
}

function exitOnListenError(port) {
  return (e) => {
    console.error(`error: cannot listen on ${HOST}:${port}: ${e.code === 'EADDRINUSE' ? 'port already in use' : e.message}`);
    process.exit(1);
  };
}

attachLobby(http.createServer(handler)).on('error', exitOnListenError(PORT)).listen(PORT, HOST, () => {
  for (const a of listAddresses()) {
    console.log(`  http://${a}:${PORT}/`);
  }
});

const tls = USE_HTTPS ? loadTlsCredentials() : null;
if (tls) {
  attachLobby(https.createServer(tls, handler)).on('error', exitOnListenError(HTTPS_PORT)).listen(HTTPS_PORT, HOST, () => {
    for (const a of listAddresses()) {
      console.log(`  https://${a}:${HTTPS_PORT}/  (use this from other machines so gamepads work)`);
    }
  });
}
