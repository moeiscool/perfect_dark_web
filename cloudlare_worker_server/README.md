# Perfect Dark on Cloudflare Workers

Everything the web version needs, running on Cloudflare instead of your own server:

* the game's files (page, `pd.js`, `pd.wasm`, icons) as static assets;
* `/server-config.json`;
* the online lobby and matches at `/net` (WebSocket), in one Durable Object.

Players use it exactly like the Node server (`web/server.js`): open the page, start the game, and pick **Online Matches** in the Perfect Menu.

## How it differs from the Node server

The Node server runs its own copy of every match (headless, with the ROM) and owns the true game state. A Worker can't: it has 128 MB of memory, and no ROM should be stored in the cloud. So this server is a **relay**:

* **Clock and inputs.** The match's Durable Object owns the 60 Hz clock: every tick it combines each player's latest input into a tick and sends it to everyone, the same as the Node server.
* **Starting a match.** The first player's game starts the match from tick 0, with no snapshot.
* **Joining and rejoining.** A player joining later gets a snapshot taken by a player already in the match. Every 20 seconds one player also sends a *checkpoint* snapshot, used if nobody else is connected when someone reconnects.
* **Staying in sync.** Players' state hashes are compared with each other. Whoever disagrees with the majority (or, between two players, with the one who has been in the match longer) is resynchronized from a player who agrees.
* **The result.** When the game says the match is over, the players report the scores.
* **Empty matches.** A match ends 60 seconds after its last player leaves, since only the players' games hold its state.

The protocol is the same as `web/net/lobby.js`, plus the messages above (`joined.fresh`, `snapshot-request`, `over`), which the web client (`web/net-client.js`) already speaks. The match settings come from `web/net/settings.js` and the tick format from `web/net/nethost.js`, shared with the Node server.

Like any lockstep game, every player runs the whole match, so a modified client could cheat in either version. The relay trusts the majority of players for the result and for resyncs.

## Setup

You need Node.js 18+, a Cloudflare account, and the game's web build (`web/build.sh` in the repository root, which writes `build-web/`).

```
cd cloudlare_worker_server
npm install
npx wrangler login            # once, opens the browser
npm run deploy                # copies ../build-web into public/, then deploys
```

`npm run deploy` prints the Worker's `*.workers.dev` address. Open it to play.

To use another build directory: `node scripts/sync-build.mjs /path/to/build-web && npx wrangler deploy`.

### Your own domain

The domain's DNS must be on Cloudflare. In `wrangler.jsonc`, uncomment `routes` and list the host names, then run `npm run deploy` again. Wrangler creates the DNS records and certificates. For example:

```jsonc
"routes": [
  { "pattern": "perfectdark.m03.ca", "custom_domain": true },
  { "pattern": "perfectdarklobby.m03.ca", "custom_domain": true }
]
```

Remove the existing DNS records for those names first (for example the ones pointing at the old server). With both names on the Worker:

* `perfectdark.m03.ca` serves the game and its own lobby;
* `perfectdarklobby.m03.ca` keeps working for players whose Online menu is set to that server.

To keep the game on another server and move only the lobby, add just the lobby name. On the game server, keep `--lobby-url perfectdarklobby.m03.ca`.

**The game's build must match.** The page, the game and the lobby check that they run the same `pd.wasm`. Deploy the Worker from the same build you serve elsewhere, or serve everything from the Worker.

### Testing locally

```
npm run sync
npx wrangler dev --port 9300
```

Then open `http://localhost:9300/`. A second player can use `http://127.0.0.1:9300/`: a different address keeps separate saves.

## Settings

In `wrangler.jsonc`, under `vars`:

| Variable | Default | What it does |
| --- | --- | --- |
| `MAX_ROOMS` | 4 | matches at once |
| `LOBBY_URL` | none | the lobby the game's Online menu uses by default, if it isn't this Worker |

## Cost on the free plan

Static files cost nothing. The Durable Object is billed for incoming WebSocket messages, at 20 messages per request, and for the time it is active. These are the allowances when this was written; check Cloudflare's pricing page:

* **Requests:** about 100,000 Durable Object requests a day.
  * A player sends an input only when it changes, plus one a second. Moving and aiming changes it most frames, so an active player can send up to about 60 a second, which is about 3 requests a second.
  * **The free allowance lasts roughly 9 player-hours of active play a day**, for example 2 hours of a full 4-player match.
  * Menus, the match list and standing still cost almost nothing.
* **Duration:** about 13,000 GB-s a day, at 128 MB, which is about 28 hours of the lobby being active.
  * It's active while anyone is connected or a match is running, and idle otherwise.

For more play, the Workers Paid plan ($5/month) includes millions of requests.
