#!/usr/bin/env bash
# Builds the web version and deploys it to a server that runs it with pm2
# (see ecosystem.config.js next to this script).
#
#   deploy/deploy.sh user@host [remote-dir]
#
# Run it on a machine that can build (Emscripten SDK, see web/build.sh) and reach the server over
# ssh. The first deploy also needs, on the server:
#   - Node 18+ in <remote-dir>/node (eg. the official linux-x64 tarball extracted there)
#   - pm2
#   - the ROM in <remote-dir>/private/ (only the lobby reads it; it is never served)
# then: pm2 start <remote-dir>/ecosystem.config.js && pm2 save

set -euo pipefail

TARGET="${1:?usage: deploy/deploy.sh user@host [remote-dir]}"
REMOTE_DIR="${2:-/home/perfectdarkserver}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SRC_DIR/build-web}"

if [ "${SKIP_BUILD:-0}" != 1 ]; then
  "$SRC_DIR/web/build.sh"
fi

# only what the servers serve or run
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/app/build-web" "$STAGE/app/web"
for f in index.html pd-web.js pd-host.js nethost.js net-client.js manifest.webmanifest sw.js icons pd.js pd.wasm pd.snap.json; do
  cp -r "$BUILD_DIR/$f" "$STAGE/app/build-web/"
done
cp -r "$SRC_DIR/web/server.js" "$SRC_DIR/web/net" "$SRC_DIR/web/package.json" "$STAGE/app/web/"
tar czf "$STAGE/release.tgz" -C "$STAGE" app

BUILD_ID="$(sha1sum "$BUILD_DIR/pd.wasm" | cut -c1-12)"
echo "deploying build $BUILD_ID to $TARGET:$REMOTE_DIR"

scp "$STAGE/release.tgz" "$TARGET:/tmp/pd-release.tgz"
scp "$SRC_DIR/deploy/ecosystem.config.js" "$TARGET:$REMOTE_DIR/ecosystem.config.js"
ssh "$TARGET" bash -s -- "$REMOTE_DIR" <<'EOF'
set -euo pipefail
cd "$1"
mkdir -p releases
cp /tmp/pd-release.tgz "releases/$(date +%Y%m%d-%H%M%S).tgz"
tar xzf /tmp/pd-release.tgz
rm /tmp/pd-release.tgz
(cd app/web && PATH="$1/node/bin:$PATH" npm install --omit=dev --no-audit --no-fund)
# running matches end; players can create new ones right away
pm2 reload ecosystem.config.js
pm2 save
EOF
echo "done: build $BUILD_ID"
