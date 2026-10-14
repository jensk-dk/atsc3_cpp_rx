#!/usr/bin/env bash
# setup.sh — fetches the inputs that are not in version control (see .gitignore and README.md "Inputs"):
#
#   specs/            the ATSC 3.0 standards (PDF) and schema zips from https://www.atsc.org and https://atsc-schemas.org
#   specs/schemas/    the A/331 schema release unpacked from A331-2022-09-Schema.zip into the tree the unit tests read
#   samples/          the five small ALP captures from https://wiki.wireshark.org/samplecaptures#atsc3-protocols
#   samples/dektec/   the DekTec STLTP and raw-IP captures (ATSC3-STLTP/, ATSC3-PCAP/) from ftp.dektec.nl, user dektec,
#                     folder pub/persistent — the password is prompted for (or taken from DEKTEC_FTP_PASSWORD)
#
# Usage: ./setup.sh [--specs] [--schemas] [--samples] [--dektec] [--force]
#   With no selector every group is fetched; --specs includes --schemas. Files already present are skipped unless
#   --force is given. Captures and the schema tree are verified against the SHA-256 the tests were written for;
#   a mismatch is an error. Non-interactive DekTec fetch: DEKTEC_FTP_PASSWORD=... ./setup.sh --dektec
#
# Needs curl, sha256sum and unzip (or python3); lftp (preferred, mirrors the FTP folders including the extracted
# *_sls/ tree) or wget for the DekTec step.
set -euo pipefail
cd "$(dirname "$0")"

UA="Mozilla/5.0 (atsc3 setup.sh)"
FORCE=0
DO_SPECS=0
DO_SCHEMAS=0
DO_SAMPLES=0
DO_DEKTEC=0
for arg in "$@"; do
  case "$arg" in
    --specs) DO_SPECS=1 ;;
    --schemas) DO_SCHEMAS=1 ;;
    --samples) DO_SAMPLES=1 ;;
    --dektec) DO_DEKTEC=1 ;;
    --force) FORCE=1 ;;
    -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "setup.sh: unknown argument $arg (try --help)" >&2; exit 2 ;;
  esac
done
if [ $((DO_SPECS + DO_SCHEMAS + DO_SAMPLES + DO_DEKTEC)) -eq 0 ]; then
  DO_SPECS=1; DO_SAMPLES=1; DO_DEKTEC=1
fi
[ "$DO_SPECS" -eq 1 ] && DO_SCHEMAS=1

ATSC=https://www.atsc.org/wp-content/uploads
ATSC_SCHEMAS=https://www.atsc-schemas.org/atsc3.0
SCHEMA_ZIP=A331-2022-09-Schema.zip
SCHEMA_ZIP_URL=$ATSC_SCHEMAS/a331/20220215/$SCHEMA_ZIP
SCHEMA_TREE_FILES=54
# sha256 of "sha256sum of every file" (paths sorted with LC_ALL=C) — the tree the tests and spec 5.16 were written for
SCHEMA_TREE_SHA256=75c8551e115ca740c2e94704d0fa24eeb7d51a6b44d2276da8e73def5b623873

fetched=0
skipped=0
failed=0

have() { [ "$FORCE" -eq 0 ] && [ -s "$1" ]; }

# fetch URL DEST [MAGIC]: download to DEST unless present; MAGIC is a byte prefix the file must start with.
fetch() {
  local url=$1 dest=$2 magic=${3:-}
  if have "$dest"; then skipped=$((skipped + 1)); return 0; fi
  mkdir -p "$(dirname "$dest")"
  printf '  %-62s ' "$(basename "$dest")"
  if ! curl -fsSL --retry 3 --retry-delay 2 -A "$UA" -o "$dest.part" "$url"; then
    echo "FAILED ($url)"; rm -f "$dest.part"; failed=$((failed + 1)); return 0
  fi
  if [ -n "$magic" ] && [ "$(head -c "${#magic}" "$dest.part")" != "$magic" ]; then
    echo "FAILED (not a ${magic} file: $url)"; rm -f "$dest.part"; failed=$((failed + 1)); return 0
  fi
  mv "$dest.part" "$dest"
  echo "$(stat -c %s "$dest") bytes"
  fetched=$((fetched + 1))
}

# verify FILE SHA256: the capture must be byte-identical to the one the golden numbers were taken from.
verify() {
  local file=$1 want=$2 got
  [ -s "$file" ] || return 0
  got=$(sha256sum "$file" | cut -d' ' -f1)
  if [ "$got" != "$want" ]; then
    echo "  $file: SHA-256 mismatch (expected $want, got $got)" >&2
    failed=$((failed + 1))
  fi
}

# unpack_schemas: sort the flat A/331 schema zip into specs/schemas/{lls,sls/...} (spec 5.16); the DWD schema with
# its examples and the ReadMe are placed twice, as the release notes file them under both application and emergency.
unpack_schemas() {
  local tmp n got
  if [ ! -s "specs/$SCHEMA_ZIP" ]; then
    echo "  specs/$SCHEMA_ZIP missing, cannot unpack specs/schemas/" >&2; failed=$((failed + 1)); return 0
  fi
  n=0
  [ -d specs/schemas ] && n=$(find specs/schemas -type f | wc -l)
  if [ "$FORCE" -eq 0 ] && [ "$n" -eq "$SCHEMA_TREE_FILES" ]; then
    echo "  specs/schemas/: $n files present, skipped"
    skipped=$((skipped + n))
  else
    tmp=$(mktemp -d)
    if command -v unzip >/dev/null; then
      unzip -q -o -d "$tmp" "specs/$SCHEMA_ZIP"
    else
      python3 -m zipfile -e "specs/$SCHEMA_ZIP" "$tmp"
    fi
    n=0
    while read -r member dir; do
      mkdir -p "specs/schemas/$dir"
      if cp "$tmp/$member" "specs/schemas/$dir/$member"; then n=$((n + 1)); else
        echo "  $member: not in $SCHEMA_ZIP" >&2; failed=$((failed + 1))
      fi
    done <<EOF
ReadMe.docx lls
RRT-1.0-20210128.xsd lls
RRT-Example-20210128.xml lls
SA-1.0-20210312.xsd lls
SA-ContentAdvisoryRatings-Example-20211208.xml lls
SLT-1.0-20211209.xsd lls
SLT-Example-20211209.xml lls
SLT-Example2-20211209.xml lls
SLT-Example3-20211209.xml lls
SYSTIME-1.0-20211208.xsd lls
SYSTIME-Example-20211208.xml lls
ReadMe.docx sls
APD-1.0-20170209.xsd sls/application
APD-Example-20170209.xml sls/application
ATSC-META-1.0-20190122.xsd sls/application
ContentRating-20211208.abnf sls/application
ContentRating-Example-20211208.abnf sls/application
DWD-1.0-20180830.xsd sls/application
DWD-Example1-20180830.xml sls/application
DWD-Example2-20180830.xml sls/application
DWD-Example3-20180830.xml sls/application
MakeContentAdvisoryString-20210402.xsl sls/application
META-Example-20210127.xml sls/application
ONSCREEN-1.0-20210401.xsd sls/application
ONSCREEN-Example-20210401.xml sls/application
UserAgentString-20211208.abnf sls/application
UserAgentString-Example-20211208.abnf sls/application
AEAT-1.0-20210322.xsd sls/emergency
AEAT-Example-20210322.xml sls/emergency
AEAT-example-school-lockdown-update-20210322.xml sls/emergency
AEAT-example-subgroup-20210322.xml sls/emergency
AEAT-example-tornado-alert-bilingual-20210322.xml sls/emergency
AEAT-example-traffic-newsdesk-alert-20210322.xml sls/emergency
DWD-1.0-20180830.xsd sls/emergency
DWD-Example1-20180830.xml sls/emergency
DWD-Example2-20180830.xml sls/emergency
DWD-Example3-20180830.xml sls/emergency
HELD-1.0-20210312.xsd sls/emergency
HELD-Example1-20210312.xml sls/emergency
HELD-Example2-20210312.xml sls/emergency
MMTUSD-1.0-20210401.xsd sls/mmtp
MMTUSD-Example-20210401.xml sls/mmtp
ATSC-FDT-1.0-20210428.xsd sls/route
EFDT-Example-20211208.xml sls/route
RepairFlow-Example1-20220708.xml sls/route
RepairFlow-Example2-20220708.xml sls/route
RepairFlow-Example3-20220708.xml sls/route
RepairFlow-Example4-20220708.xml sls/route
RepairFlow-Example5-20220708.xml sls/route
ROUTE_SLS-OTT-delivery-Triveni-SegmentTimeline-fixed-MPD.xml sls/route
ROUTEUSD-1.0-20170920.xsd sls/route
ROUTEUSD-Example-20170920.xml sls/route
S-TSID-1.0-20220708.xsd sls/route
S-TSID-Example-20220708.xml sls/route
EOF
    rm -rf "$tmp"
    echo "  specs/schemas/: $n files unpacked from $SCHEMA_ZIP"
    fetched=$((fetched + n))
  fi
  [ -d specs/schemas ] || return 0
  got=$(cd specs/schemas && find . -type f | LC_ALL=C sort | xargs sha256sum | sha256sum | cut -d' ' -f1)
  if [ "$got" != "$SCHEMA_TREE_SHA256" ]; then
    echo "  specs/schemas/: tree differs from the release the tests were written for (aggregate SHA-256 $got)" >&2
    failed=$((failed + 1))
  fi
}

# ---------------------------------------------------------------------------------------------------------------------
if [ "$DO_SPECS" -eq 1 ]; then
  echo "specs/: ATSC 3.0 standards (the revisions ATSC3_CLIENT_STACK_SPEC.md was written from) and schema zips"
  while read -r url name; do
    fetch "$url" "specs/$name" "%PDF"
  done <<EOF
$ATSC/2024/04/A200-2024-04-Regional-Service-Availability-Signaling.pdf A200-2024-04-Regional-Service-Availability-Signaling.pdf
$ATSC/2024/04/A300-2024-04-ATSC-3-System-Standard.pdf A300-2024-04-ATSC-3-System-Standard.pdf
$ATSC/2024/04/A321-2024-04-System-Discovery-and-Signaling.pdf A321-2024-04-System-Discovery-and-Signaling.pdf
$ATSC/2024/04/A322-2024-04-Physical-Layer-Protocol.pdf A322-2024-04-Physical-Layer-Protocol.pdf
$ATSC/2024/04/A323-2024-04-Dedicated-Return-Channel.pdf A323-2024-04-Dedicated-Return-Channel.pdf
$ATSC/2024/04/A324-2024-04-Scheduler-STL.pdf A324-2024-04-Scheduler-STL.pdf
$ATSC/2024/04/A330-2024-04-Link-Layer-Protocol.pdf A330-2024-04-Link-Layer-Protocol.pdf
$ATSC/2025/01/A331-2024-04a-Signaling-Delivery-Sync-FEC.pdf A331-2024-04a-Signaling-Delivery-Sync-FEC.pdf
$ATSC/2024/04/A332-2024-04-Service-Announcement.pdf A332-2024-04-Service-Announcement.pdf
$ATSC/2024/04/A333-2024-04-Service-Usage-Reporting.pdf A333-2024-04-Service-Usage-Reporting.pdf
$ATSC/2024/04/A334-2024-04-Audio-Watermark-Emission.pdf A334-2024-04-Audio-Watermark-Emission.pdf
$ATSC/2024/04/A335-2024-04-Video-Watermark-Emission.pdf A335-2024-04-Video-Watermark-Emission.pdf
$ATSC/2024/04/A336-2024-04-Content-Recovery-in-Redistribution-Scenarios.pdf A336-2024-04-Content-Recovery-in-Redistribution-Scenarios.pdf
$ATSC/2024/04/A337-2024-04-Application-Event-Delivery.pdf A337-2024-04-Application-Event-Delivery.pdf
$ATSC/2024/04/A338-2024-04-Companion-Device.pdf A338-2024-04-Companion-Device.pdf
$ATSC/2024/04/A341-2024-04-Video-HEVC.pdf A341-2024-04-Video-HEVC.pdf
$ATSC/2024/04/A342-1-2024-04-Audio-Common-Elements.pdf A342-1-2024-04-Audio-Common-Elements.pdf
$ATSC/2024/04/A342-2-2024-04-AC4-System.pdf A342-2-2024-04-AC4-System.pdf
$ATSC/2024/04/A342-3-2024-04-MPEG-System.pdf A342-3-2024-04-MPEG-System.pdf
$ATSC/2024/04/A343-2024-04-Captions-and-Subtitles.pdf A343-2024-04-Captions-and-Subtitles.pdf
$ATSC/2024/04/A344-2024-04-Interactive-Content.pdf A344-2024-04-Interactive-Content.pdf
$ATSC/2024/05/A360-2024-04-Security.pdf A360-2024-04-Security.pdf
EOF
  while read -r url name; do
    fetch "$url" "specs/$name" "PK"
  done <<EOF
$ATSC_SCHEMAS/a324/20220331/A324-2022-03-Schema.zip A324-2022-03-Schema.zip
$ATSC_SCHEMAS/a332/20240403/A332-2022-03-Schemas.zip A332-2022-03-Schemas.zip
$ATSC_SCHEMAS/a338/20220117/A338-2022-03-SchemasS38-298r3.zip A338-2022-03-SchemasS38-298r3.zip
$ATSC_SCHEMAS/a323/S32-445r0-DRC-Schema-2018-08-23.zip S32-445r0-DRC-Schema-2018-08-23.zip
$ATSC_SCHEMAS/a331/archive/S33-645r2-FDT-Enhancement-Schema.zip S33-645r2-FDT-Enhancement-Schema.zip
$ATSC_SCHEMAS/a360/20240403/S36-267r0-Schemas-For-A360-2022-03.zip S36-267r0-Schemas-For-A360-2022-03.zip
$ATSC_SCHEMAS/a200/S39-008r0-RSAT-SchemaFor-A200.zip S39-008r0-RSAT-SchemaFor-A200.zip
$ATSC_SCHEMAS/a337/20240403/SchemasFor-A337-2022-03.zip SchemasFor-A337-2022-03.zip
EOF
fi

# ---------------------------------------------------------------------------------------------------------------------
if [ "$DO_SCHEMAS" -eq 1 ]; then
  echo "specs/schemas/: the A/331 schema release ($SCHEMA_ZIP) unpacked into the tree the unit tests read"
  fetch "$SCHEMA_ZIP_URL" "specs/$SCHEMA_ZIP" "PK"
  unpack_schemas
fi

# ---------------------------------------------------------------------------------------------------------------------
if [ "$DO_SAMPLES" -eq 1 ]; then
  echo "samples/: Wireshark sample captures (SiliconDust receiver, several stations)"
  W=https://wiki.wireshark.org/uploads
  while read -r path sha; do
    name=$(basename "$path")
    fetch "$W/$path" "samples/$name"
    verify "samples/$name" "$sha"
  done <<EOF
ffedd6b9fa50190e1fd12de2b90f43a4/alp-sample1.pcap 5a63b548d91bad55b77b18ee052b5bf86ee9653728296323f75a22432cac2dd0
486520825a0a8fb0372a77f70f0afbfc/alp-sample2.pcap 71c99327074855978eb725c51e4d574f0e574af63a5328c7e7380b052ee2de60
35812d74f0d49a361a36491d69697052/ch11-25-slt.pcap b0d1a3a3793630c8954284bc4cef76f561f3b197300e4804a1255c8674187732
a0884e575b7edb2034948fd1bb13e5fa/lls-tables-alp.pcap b912e950167ff047052105101c42bd77002387ee7a7b0e6dc695c5cca1dba57a
19568882943f275165a5c314f35c024f/mp4-ftyp-styp-sidx.pcap 33a8eb1ff8df57306b7fb70a19060f9999a395fb6d3d8c6b9391d0abe082456f
EOF
fi

# ---------------------------------------------------------------------------------------------------------------------
if [ "$DO_DEKTEC" -eq 1 ]; then
  echo "samples/dektec/: DekTec captures from ftp.dektec.nl (user dektec, folder pub/persistent; about 2.9 GB)"
  HOST=ftp.dektec.nl
  USER=dektec
  REMOTE=/pub/persistent
  DEKTEC_FILES="ATSC3-STLTP/BWI_20230307_1742.pcap
ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap
ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.xa3
ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap
ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.xa3"
  missing=0
  for f in $DEKTEC_FILES; do have "samples/dektec/$f" || missing=1; done
  if [ "$missing" -eq 0 ]; then
    echo "  all five files present, skipped (use --force to refetch)"
    skipped=$((skipped + 5))
  else
    pw=${DEKTEC_FTP_PASSWORD:-}
    if [ -z "$pw" ]; then
      if [ -t 0 ]; then
        read -rs -p "  password for $USER@$HOST: " pw; echo
      else
        echo "  no terminal and DEKTEC_FTP_PASSWORD is unset: DekTec step skipped" >&2
        failed=$((failed + 1))
      fi
    fi
    if [ -n "$pw" ]; then
      mkdir -p samples/dektec
      if command -v lftp >/dev/null; then
        # mirror -c resumes partial transfers; the folders also carry an extracted *_sls/ tree beside the captures
        lftp <<EOF || failed=$((failed + 1))
set ssl:verify-certificate no
set net:timeout 30
set net:max-retries 3
open -u "$USER","$pw" "$HOST"
mirror -c --parallel=2 --verbose=1 "$REMOTE/ATSC3-STLTP" samples/dektec/ATSC3-STLTP
mirror -c --parallel=2 --verbose=1 "$REMOTE/ATSC3-PCAP" samples/dektec/ATSC3-PCAP
quit
EOF
      elif command -v wget >/dev/null; then
        wget -q --show-progress -r -nH --cut-dirs=2 -c -P samples/dektec --ftp-user="$USER" --ftp-password="$pw" \
          "ftp://$HOST$REMOTE/ATSC3-STLTP/" "ftp://$HOST$REMOTE/ATSC3-PCAP/" || failed=$((failed + 1))
      else
        netrc=$(mktemp); chmod 600 "$netrc"
        printf 'machine %s login %s password %s\n' "$HOST" "$USER" "$pw" > "$netrc"
        for f in $DEKTEC_FILES; do
          have "samples/dektec/$f" && { skipped=$((skipped + 1)); continue; }
          mkdir -p "samples/dektec/$(dirname "$f")"
          printf '  %-62s ' "$(basename "$f")"
          if curl -fsS --netrc-file "$netrc" -C - -o "samples/dektec/$f" "ftp://$HOST$REMOTE/$f"; then
            echo "$(stat -c %s "samples/dektec/$f") bytes"; fetched=$((fetched + 1))
          else
            echo "FAILED"; failed=$((failed + 1))
          fi
        done
        rm -f "$netrc"
      fi
      for f in $DEKTEC_FILES; do [ -s "samples/dektec/$f" ] || { echo "  missing after fetch: samples/dektec/$f" >&2; failed=$((failed + 1)); }; done
      unset pw
    fi
  fi
  while read -r f sha; do verify "samples/dektec/$f" "$sha"; done <<EOF
ATSC3-STLTP/BWI_20230307_1742.pcap e04ef5b3a24524b2554461e4125704318749bb01d54ffb79ee4acb903ee46d8c
ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap c5610c82b88d51c54317867be826828eb61c777891873c246c0b7aaf47bd2abf
ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap c72fea6b5f20888206d7c25c042bb0424d9407118238a28953f83296c61017ff
ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.xa3 2cbe1cef4d72fa671e84aca6762aabd2d36d9bf6cbab5a0780aa9c20eccebfa2
ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.xa3 1f7145b9af658e9f0720dcc888c38193d28e0e18ac6f0c2ed9e3cddcd70f7f42
EOF
fi

echo "setup.sh: $fetched fetched, $skipped already present, $failed failed"
[ "$failed" -eq 0 ]
