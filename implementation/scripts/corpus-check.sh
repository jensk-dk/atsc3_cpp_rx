#!/usr/bin/env bash
# Verifies that the capture corpus is present before golden/e2e runs (spec 9).
set -euo pipefail
CORPUS=${1:-$(dirname "$0")/../../samples}
for f in alp-sample1.pcap alp-sample2.pcap ch11-25-slt.pcap lls-tables-alp.pcap mp4-ftyp-styp-sidx.pcap \
         dektec/ATSC3-STLTP/BWI_20230307_1742.pcap \
         dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap \
         "dektec/ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap"; do
  [ -f "$CORPUS/$f" ] || { echo "missing: $CORPUS/$f" >&2; exit 1; }
done
echo "corpus ok: $CORPUS"
