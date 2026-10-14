#!/usr/bin/env bash
# STB end-to-end suite (spec 5.20.5): atsc3stb headless (fake sinks) on the corpus with scripted keys; the summary
# lines it prints are checked for decoded frames on every tuned channel, the banner and the channel list.
#   run-stb.sh <atsc3stb binary> <corpus dir>
# Exit 0 pass, 1 fail, 77 skipped (no binary, no corpus, or the GStreamer plugins are missing). Real-time replays.
set -uo pipefail
BIN=${1:?atsc3stb binary}; CORPUS=${2:?corpus dir}
[ -x "$BIN" ] || { echo "skip: $BIN not built"; exit 77; }
UNI="$CORPUS/dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap"
BWI="$CORPUS/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap"
SMALL="$CORPUS/ch11-25-slt.pcap"
[ -f "$UNI" ] || { echo "skip: corpus not available ($UNI)"; exit 77; }
for e in compositor audiomixer cairooverlay decodebin avdec_h265 avdec_aac fakevideosink; do
  gst-inspect-1.0 "$e" >/dev/null 2>&1 || { echo "skip: GStreamer element $e not installed"; exit 77; }
done
HEADLESS="--video-sink fakevideosink --audio-sink fakesink"
fail=0
field() {   # field NAME from the summary line of a log
  sed -n 's/.*summary .*\b'"$2"'=\([0-9]*\).*/\1/p' "$1" | tail -1
}
frames_of() {   # frames_of LOG "<label>": the video frames of that tune's stats line
  sed -n 's/^atsc3stb: stats '"$2"': video frames \([0-9]*\),.*/\1/p' "$1" | tail -1
}
expect() {   # expect "<what>" <value> -ge <min>
  if [ -n "$2" ] && [ "$2" "$3" "$4" ]; then echo "   ok   $1 = $2"; else echo "   FAIL $1 = '$2' (wanted $3 $4)"; fail=1; fi
}
LOG=$(mktemp)

echo "### UniSoft usc02 (HEVC + AAC), channel change with Up, list open/tune with Return/Up/Return"
# tune 24.2 (usc02) → Up at 14 s (usc03, the last row) → Return/Up/Return at 26-28 s (the list: back to usc02), 40 s
"$BIN" $HEADLESS --duration 40 --service 2 --keys "Up@14,Return@26,Up@27,Return@28" "$UNI" > "$LOG" 2>&1
echo "   exit $?"
grep -q "^atsc3stb: tune " "$LOG" || { echo "   FAIL no tune"; fail=1; }
expect "channels" "$(field "$LOG" channels)" -ge 3
expect "tunes" "$(field "$LOG" tunes)" -ge 3
expect "keys applied" "$(field "$LOG" keys)" -eq 4
expect "banners" "$(field "$LOG" banners)" -ge 3
expect "audio buffers" "$(field "$LOG" audio_buffers)" -ge 100
first=$(sed -n 's/^atsc3stb: tune \([^(]*\) (.*/\1/p' "$LOG" | sed -n 1p | sed 's/ *$//')
second=$(sed -n 's/^atsc3stb: tune \([^(]*\) (.*/\1/p' "$LOG" | sed -n 2p | sed 's/ *$//')
expect "video frames on the first channel ($first)" "$(frames_of "$LOG" "$first")" -ge 60
expect "video frames on the second channel ($second)" "$(frames_of "$LOG" "$second")" -ge 60
grep -q "^atsc3stb: GStreamer error" "$LOG" && { echo "   FAIL GStreamer error"; grep "GStreamer error" "$LOG"; fail=1; }

if [ -f "$BWI" ]; then
  echo "### BWI WNUV (MMT, HEVC decoded, AC-4 listed without decoder)"
  "$BIN" $HEADLESS --duration 35 --service 5007 "$BWI" > "$LOG" 2>&1
  echo "   exit $?"
  expect "video frames" "$(field "$LOG" video_frames)" -ge 60
  grep -q "^atsc3stb: track audio ac-4.* not played: no GStreamer decoder" "$LOG" && echo "   ok   AC-4 reported without decoder" || { echo "   FAIL AC-4 verdict missing"; fail=1; }
  grep -q '^atsc3stb: epg 54.1 WNUV: now "' "$LOG" && echo "   ok   present programme from the ESG" || { echo "   FAIL no present programme for WNUV"; fail=1; }
  grep -q "^atsc3stb: GStreamer error" "$LOG" && { echo "   FAIL GStreamer error"; fail=1; }
fi

echo "### small ALP sample: channel list, quits at the end of the capture"
"$BIN" $HEADLESS --show-hidden --duration 20 "$SMALL" > "$LOG" 2>&1
echo "   exit $?"
expect "channels" "$(field "$LOG" channels)" -ge 2
grep -q "^atsc3stb: end of capture\|^atsc3stb: duration reached" "$LOG" && echo "   ok   exited cleanly" || { echo "   FAIL no clean exit"; fail=1; }

rm -f "$LOG"
[ $fail -eq 0 ] && echo "STB e2e: all scenarios passed" || echo "STB e2e: FAILURES"
exit $fail
