#!/usr/bin/env bash
# cvlc log-driven scenario (spec 5.19.5 harness 1): plays an atsc3:// MRL headless for a while and checks the log.
#   run.sh <plugin-dir> <capture> <mrl-options> <seconds> <expected.json> [vlc-binary]
# The plugin dir goes into VLC_PLUGIN_PATH (any VLC 3.0.x); the VLC binary defaults to cvlc on PATH. Exit codes:
# 0 pass, 1 fail, 77 skipped (no VLC / no capture).
set -uo pipefail
PLUGIN_DIR=${1:?plugin dir}; CAPTURE=${2:?capture}; OPTS=${3:-}; SECONDS_=${4:-30}; EXPECTED=${5:?expected.json}; VLCBIN=${6:-cvlc}
HERE=$(cd "$(dirname "$0")" && pwd)
command -v "$VLCBIN" >/dev/null 2>&1 || { echo "skip: $VLCBIN not found"; exit 77; }
[ -f "$CAPTURE" ] || { echo "skip: capture $CAPTURE not found"; exit 77; }
LOG=${LOG:-$(mktemp -t atsc3-vlc-XXXXXX.log)}
export VLC_PLUGIN_PATH="$PLUGIN_DIR${VLC_PLUGIN_PATH:+:$VLC_PLUGIN_PATH}"
MRL="atsc3://$CAPTURE${OPTS:+?$OPTS}"
echo "run: $VLCBIN --run-time=$SECONDS_ $MRL (log $LOG)"
timeout $((SECONDS_ + 60)) "$VLCBIN" -I dummy -vv --no-video-title-show --vout dummy --aout dummy --play-and-exit --run-time="$SECONDS_" "$MRL" > "$LOG" 2>&1
RC=$?
[ $RC -eq 0 ] || echo "note: $VLCBIN exited $RC"
python3 -I "$HERE/check_log.py" "$LOG" "$EXPECTED"
