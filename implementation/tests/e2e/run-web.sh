#!/usr/bin/env bash
# Starts atsc3d with the web server and runs the Playwright suite of the web integration (spec 5.18.3).
#   run-web.sh <atsc3d binary> <corpus dir> [playwright args...]
# Env: ATSC3_E2E_PORT (default 18080, away from a developer's daemon on 8080), PLAYWRIGHT_PROJECT (default chromium;
# firefox works without the headless shell). Exit 77 (skipped) without node/npx.
set -euo pipefail
ATSC3D=${1:?atsc3d binary}; CORPUS=${2:?corpus dir}; shift 2
cd "$(dirname "$0")/../../integrations/web/frontend"
command -v npx >/dev/null 2>&1 || { echo "skip: npx not found"; exit 77; }
PORT=${ATSC3_E2E_PORT:-18080}
"$ATSC3D" --web 127.0.0.1:"$PORT" --captures "$CORPUS" & PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
sleep 1
ATSC3_WEB_URL=http://127.0.0.1:$PORT npx playwright test --project="${PLAYWRIGHT_PROJECT:-chromium}" "$@"
