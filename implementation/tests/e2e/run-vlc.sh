#!/usr/bin/env bash
# VLC end-to-end suite (spec 5.19.5): the cvlc log-driven scenarios and the libvlc-driven one, against the corpus.
#   run-vlc.sh <plugin .so> <corpus dir> [vlc-binary]
# Exit 0 pass, 1 fail, 77 skipped (no VLC runtime or no corpus). Each scenario replays in real time (30-45 s).
set -uo pipefail
PLUGIN=${1:?plugin .so}; CORPUS=${2:?corpus dir}; VLCBIN=${3:-cvlc}
PLUGIN_DIR=$(cd "$(dirname "$PLUGIN")" && pwd)
HERE=$(cd "$(dirname "$0")" && pwd)
TESTS="$HERE/../../integrations/vlc/tests"
command -v "$VLCBIN" >/dev/null 2>&1 || { echo "skip: $VLCBIN not found"; exit 77; }
UNI="$CORPUS/dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap"
BWI="$CORPUS/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap"
SMALL="$CORPUS/ch11-25-slt.pcap"
[ -f "$UNI" ] || { echo "skip: corpus not available ($UNI)"; exit 77; }
fail=0
run() {   # name capture opts seconds expected
  echo "### $1"
  "$TESTS/cvlc/run.sh" "$PLUGIN_DIR" "$2" "$3" "$4" "$TESTS/expected/$5" "$VLCBIN" || { [ $? -eq 77 ] && echo "   (skipped)" || fail=1; }
}
run "small sample: open, list programs, EOF" "$SMALL" "show-hidden=1" 12 small-open-close.json
run "UniSoft usc02 (HEVC + AAC)" "$UNI" "service=2" 30 unisoft-usc02.json
run "UniSoft usc03 (IMSC1 captions)" "$UNI" "service=3" 30 unisoft-usc03.json
if [ -f "$BWI" ]; then
  run "BWI WMAR (ROUTE, AC-4 listed, EPG, alerts)" "$BWI" "service=5004" 45 bwi-wmar.json
  run "BWI WNUV (MMT)" "$BWI" "service=5007" 45 bwi-wnuv.json
  run "BWI WBAL (DRM)" "$BWI" "service=5003" 40 bwi-wbal.json
fi
echo "### libvlc: play, tracks, pause/resume, program switch"
ALT_PROGRAM=3 python3 -I "$TESTS/pyvlc/libvlc_test.py" "$PLUGIN_DIR" "$UNI" 2 || { [ $? -eq 77 ] && echo "   (skipped)" || fail=1; }
[ $fail -eq 0 ] && echo "VLC e2e: all scenarios passed" || echo "VLC e2e: FAILURES"
exit $fail
