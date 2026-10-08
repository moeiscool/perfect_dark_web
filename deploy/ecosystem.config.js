// pm2 apps for Perfect Dark on this server. HTTPS is done by Cloudflare in front of the "swerve"
// proxy on port 80: perfectdark.m03.ca -> :4002 (game), perfectdarklobby.m03.ca -> :4003 (lobby).
// Uses the Node 22 in ./node (the system Node is too old).
//
//   pm2 start /home/perfectdarkserver/ecosystem.config.js && pm2 save
const base = '/home/perfectdarkserver';

module.exports = {
  apps: [
    {
      // the page and game files; points the game's Online menu at the lobby's host name
      name: 'perfectdark-game',
      cwd: `${base}/app`,
      script: 'web/server.js',
      interpreter: `${base}/node/bin/node`,
      args: '--port 4002 --no-https --no-lobby --lobby-url perfectdarklobby.m03.ca',
      max_memory_restart: '200M',
    },
    {
      // lobby and match server (WebSocket /net); runs each match with the ROM, which is never served
      name: 'perfectdark-lobby',
      cwd: `${base}/app`,
      script: 'web/server.js',
      interpreter: `${base}/node/bin/node`,
      args: `--port 4003 --no-https --rom ${base}/private/pd.ntsc-final.z64 --max-rooms 2`,
    },
  ],
};
