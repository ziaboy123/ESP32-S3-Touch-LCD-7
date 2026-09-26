#!/usr/bin/env bash
# Build the firmware and hand it to the backend; the panel notices the new
# MD5 on its next poll (within ~5s), downloads it and restarts itself.
# See README "Updates" for why updates are pulled rather than pushed.
#
# Where the build goes is local config, not code: copy deploy.env.example to
# deploy.env (gitignored) and fill it in.
set -euo pipefail
cd "$(dirname "$0")"
[ -f deploy.env ] || { echo "No deploy.env - copy deploy.env.example and fill it in."; exit 1; }
# shellcheck source=/dev/null
. ./deploy.env
: "${DEPLOY_SSH:?}" "${DEPLOY_PATH:?}"

pio run -e panel
BIN=.pio/build/panel/firmware.bin
scp -q "$BIN" "$DEPLOY_SSH:$DEPLOY_PATH.tmp"
ssh "$DEPLOY_SSH" "${DEPLOY_OWNER:+chown $DEPLOY_OWNER $DEPLOY_PATH.tmp && }mv $DEPLOY_PATH.tmp $DEPLOY_PATH"
echo "Uploaded $(md5 -q "$BIN" 2>/dev/null || md5sum "$BIN" | cut -d' ' -f1) - the panel will update itself shortly."
