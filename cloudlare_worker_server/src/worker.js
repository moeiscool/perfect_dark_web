// Perfect Dark on Cloudflare Workers: the game's files (static assets, see scripts/sync-build.mjs),
// /server-config.json, and the online lobby at /net (one Durable Object, lobby-do.js).
import { BUILD_ID } from './build-info.js';

export { LobbyDO } from './lobby-do.js';

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (url.pathname === '/net') {
      // every player shares one lobby object (match list and matches)
      const id = env.LOBBY.idFromName('main');
      return env.LOBBY.get(id).fetch(request);
    }

    if (url.pathname === '/server-config.json') {
      return new Response(JSON.stringify({
        httpsPort: null,
        build: BUILD_ID,
        online: true,
        // another host name for the lobby, if the game is served elsewhere; null = this host
        lobby: env.LOBBY_URL || null,
      }), {
        headers: { 'Content-Type': 'application/json; charset=utf-8', 'Cache-Control': 'no-cache' },
      });
    }

    return env.ASSETS.fetch(request);
  },
};
