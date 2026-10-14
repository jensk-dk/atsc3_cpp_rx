# ATSC 3.0 Client-Side Stack — Software Specification (C++17)

Version 0.29 — 2026-10-14
Status: Draft for review

This document specifies a receiver-side ("client") ATSC 3.0 protocol stack written in C++17.
It covers everything between the output of an RF demodulator (Baseband Packets or ALP packets)
and the point where the application receives typed signaling, media segments / MPUs, NRT files and
events. It was written from the standards in `specs/` (from https://www.atsc.org/atsc-documents/type/3-0-standards/: A/300, A/321, A/322, A/324, A/330, A/331,
A/332, A/337, A/344, A/360, A/200), from the normative XML schemas and example instances in
`specs/schemas/` (A/331 schema release of 2021-12-08, see 5.16), and from a byte-level analysis of the
captures in `samples/`.
Findings from the captures are folded into the normative text as "Observed" notes so that the
implementation handles what real broadcasts actually do, not only what the standards say.

---

## Table of contents

1. Scope and goals
2. Ingest formats: what enters the stack
3. Sample inventory and findings
4. Architecture
5. Module specifications
   - 5.1 Tuners (the engine's tuner contract, RF tuners, pcap virtual tuner, UDP source)
   - 5.2 STLTP demultiplexer (A/324)
   - 5.3 Baseband Packet deframer (A/322 §5.2)
   - 5.4 ALP decapsulation (A/330 §5)
   - 5.5 Link-layer signaling, LMT/RDT and ROHC (A/330 §6–7)
   - 5.6 IP/UDP demultiplexer and flow router
   - 5.7 Low Level Signaling (A/331 §6)
   - 5.8 ROUTE / LCT object recovery (A/331 Annex A)
   - 5.9 ROUTE Service Layer Signaling (A/331 §7.1)
   - 5.10 MMTP (ISO/IEC 23008-1, A/331 §7.2, §8.1.2)
   - 5.11 Service manager
   - 5.12 Application API
   - 5.13 Hybrid / broadband hooks
   - 5.14 Security (A/360)
   - 5.15 Clock and timing
   - 5.16 XML signaling model and schema set
   - 5.17 Optional web server (`atsc3::web`)
   - 5.18 Optional Vue 3 frontend and end-to-end tests
   - 5.19 Optional VLC plugin (`atsc3://` demux module)
   - 5.20 Optional GStreamer set-top-box player (`atsc3stb`)
6. Data conversion and analysis tools
7. Non-functional requirements
8. Project layout, build and dependencies
9. Test strategy
10. Delivery phases
11. Open questions and assumptions
- Appendix A: Bit-level quick reference
- Appendix B: Constants observed in the samples

---

## 1. Scope and goals

### 1.1 In scope

- Ingest of the two physical-layer output formats tuners produce: **Baseband Packets (BBP)** per
  PLP (A/322 §5.2) and **ALP packets** (A/330). Both must be first-class inputs; the tuner
  decides which one it delivers.
- A **pcap virtual tuner** (5.1.2) that replaces the RF tuner for testing and for running on a
  Linux PC without RF hardware. It reads capture files (pcap/pcapng) in the three forms found in
  practice and auto-detects which one it has:
  1. `LINKTYPE_ATSC_ALP` (DLT 289): one ALP packet per record (all five ATSC samples use this).
  2. **STLTP** (A/324): RTP/UDP/IP tunnel packets carrying BBP streams, Preamble and T&M.
  3. **Raw IP** above ALP (Ethernet / raw-IP / Linux SLL link types): UDP multicast datagrams,
     not STLTP. In this case the virtual tuner **bypasses the PHY, ALP and link layers** and
     injects datagrams straight into the IP layer.
  It runs either in **real-time mode** (throttled by the timestamps in the data) or in
  **non-real-time mode** (read and injected as fast as possible).
- Link layer: ALP decapsulation incl. segmentation, concatenation, SID, header extensions, link
  layer signaling (LMT, RDT), MPEG-2 TS encapsulation pass-through, ROHC-U decompression hook.
- IP/UDP demultiplexing, flow subscription, multiple PLPs, multiple broadcasters per stream.
- LLS: SLT, SystemTime, RRT, AEAT, OnscreenMessageNotification, SignedMultiTable, CDT,
  UserDefined, unknown-table pass-through.
- ROUTE/LCT: all codepoints, EXT_FTI/EXT_TOL/EXT_TIME/EXT_ROUTE_PRESENTATION_TIME, EFDT
  (in-band TOI 0 and S-TSID-embedded), file/entity/package modes, gzip content encoding, MDE
  (fragment) delivery, object completion strategies, repair-flow (RaptorQ) interface.
- ROUTE SLS: metadataEnvelope, USBD, S-TSID, MPD, HELD, DWD, APD, RSAT; versioning; session setup.
- MMTP: packet header (version 01), MPU mode reassembly into ISOBMFF fragments, signaling
  messages (PA, MPT/MP table, MPI, HRBM, `mmt_atsc3_message` and its content types), timing.
- Service management: discovery, selection, SLS binding, session lifecycle, SLT/LLS updates,
  ESG (A/332) and EA (A/331 §6.5, Annex G) data paths, app-based service data (HELD, NRT files).
- Application API in C++17 (callback + pull), statistics, error reporting, logging.
- Tools: capture analyzer and converters between the ingest formats (Section 6).
- **Optional** web server (5.17) and Vue 3 frontend (5.18): service list, service selection, EPG,
  alerts, signaling inspector and A/V playback through Media Source Extensions (dash.js for ROUTE
  services, a fragment-push MSE player for MMT services). They are not part of the receiver core;
  they are an *integration* (4.1): the stack's end-to-end test vehicle and developer UI, built only when
  `ATSC3_WEB=ON`.
- **Optional** VLC plugin (5.19): a VLC demux module that embeds the stack and the pcap virtual tuner
  so that VLC can open a capture (or a UDP/RF source) as `atsc3://…`, list services as VLC programs
  and play the selected service with VLC's own decoders. Like the web server it is an integration (4.1) that doubles as an end-to-end
  test vehicle, built only when `ATSC3_VLC=ON`.

### 1.2 Out of scope

- RF front end, demodulation, L1 signaling decoding, LDPC/BCH decoding, BBP descrambling. The
  tuner delivers decoded, descrambled BBPs or ALP packets plus PLP ids.
- Media decoding and rendering inside the stack (the optional frontend decodes in the browser),
  DASH playback logic in the core, A/344 broadcaster-application runtime, A/338
  companion device, watermarks (A/334, A/335, A/336), DRC/return channel (A/323).
- Broadband fetching itself (HTTP client). The stack exposes URLs and hooks (Section 5.13).

### 1.3 Design goals

1. **Robustness over strictness.** Real captures contain vendor-private ALP packets, truncated
   frames, namespace variants and missing optional headers. Never crash, never stall; drop,
   count, log and continue.
2. **Format agnostic ingest.** The pipeline above the ALP parser is identical whatever the source.
3. **Deterministic, testable layers.** Every layer is a pure function of its input bytes plus
   explicit state, so each can be unit-tested with hex vectors and golden pcaps.
4. **Low allocation.** Packet paths use views over pooled buffers; copies only when an object is
   assembled or handed to the application.
5. **Spec traceability.** Every parser carries a comment referencing the table/section it implements.

---

## 2. Ingest formats: what enters the stack

| # | Input | Produced by | Unit delivered to stack | Normative reference |
|---|-------|-------------|------------------------|---------------------|
| I1 | Baseband Packets | Demodulators that expose the PLP data before ALP extraction (e.g. BBP-output demod chips, STLTP from a gateway) | `BasebandPacket{plp_id, bytes, flags}` — one complete BBP (header + payload), descrambled, FEC-decoded | A/322 §5.2 |
| I2 | ALP packets | Demodulators with built-in ALP extraction, DLT 289 pcaps, ALPTP | `AlpPacket{plp_id, bytes}` — one complete ALP packet (header + payload) | A/330 §5 |
| I3 | STLTP | Broadcast gateway output recorded on the STL, test generators | RTP/UDP/IP outer tunnel packets (PT 97) | A/324 §6, §9 |
| I4 | IP datagrams | pcaps taken on the IP side (Ethernet/raw IP/SLL), ALP-free test rigs | IPv4/UDP datagrams | A/331 §6–§8 |

Notes:

- Some tuners deliver (I1) and others (I2). The stack **shall** accept either without configuration
  beyond the tuner choice. A BBP tuner feeds `BbpDeframer` (5.3) whose output is (I2).
- (I3) is a transport form of (I1): the STLTP demux (5.2) extracts BBPs per PLP and feeds 5.3.
- (I4) bypasses the PHY, ALP and link layers entirely and enters the IP demux (5.6). PLP
  information is absent; the LMT path is then not available and all flows are treated as
  "PLP unknown". The pcap virtual tuner (5.1.2) selects this path automatically when a capture
  holds plain IP data that is not STLTP.
- On a Linux PC without RF hardware, and in all automated tests, the **pcap virtual tuner** is the
  source. It presents the same `ITuner` contract as a real tuner, so the layers above never
  know whether bytes came from a demodulator or from a file.
- In every path the pipeline must know the **PLP id** when available (BBP: given by tuner/port;
  ALP pcap: not available unless the writer supplies it — treat as `kUnknownPlp`; STLTP: from the
  inner UDP destination port 30000+plp).
- **BBP scrambling:** A/322 §5.2.3 scrambles the whole BBP before FEC. Demodulators descramble
  after FEC decoding; the stack assumes descrambled input. A `descramble=true` option applying
  the A/322 §5.2.3 PRBS is provided for sources that do not (Section 5.3, item 6).

---

## 3. Sample inventory and findings

All analysis is done with `atsc3scan` (6.1: format verdict, link-layer facts, LLS tables, UDP flows, ROUTE/LCT
flows with object completeness, MMTP flows with their signaling) and `tcpdump`; the numbers quoted here and in
Section 9 are asserted on the corpus by `tests/golden/scan_test.cpp`.
The table lists what each capture exercises so that the test plan (Section 9) can cite them.

### 3.1 Small captures (`samples/*.pcap`)

The five ALP captures are the ATSC 3.0 sample captures of the Wireshark wiki
(https://wiki.wireshark.org/samplecaptures#atsc3-protocols): recorded with a SiliconDust receiver from several
stations on 27 December 2022 and 3 January 2023, so they are independent of the DekTec STLTP capture of 3.2 even
though the same Baltimore broadcasters (bsid 1409) appear in both. `setup.sh` fetches them and checks their SHA-256.

| File | Link type | Packets / size | Content | Use in tests |
|------|-----------|----------------|---------|--------------|
| `alp-sample1.pcap` | ATSC ALP (289), ns timestamps | 17 / 9.9 kB | LLS AEAT (group 1, v38); LMT v2 with 3 PLPs and 56 multicasts; ROUTE media (TSI 100, CP 8); MMTP MPU packets (pid 100 and 200) on 239.255.54.1:5001; RTP/MPEG-TS (PT 33) on 239.255.54.206:5000; **9 zero-length ALP packets with HEF type 0xF1** | ALP robustness, LMT parse, MMTP header |
| `alp-sample2.pcap` | ATSC ALP | 85 / 125 kB | SignedMultiTable (SLT bsid 564 + SystemTime), CDT (0x06, v74); LMT (1 PLP, 4 multicasts); ROUTE WKCF media 239.255.18.1:5181 TSI 10/20/30, CP 128 + EXT_TOL; ESG-service SLS 239.255.2.255:8000 TSI 0/1/2 CP 1 (EFDT at TOI 0 → `multipart/signed` SLS at TOI 196829); 1 zero-length ALP (HEF 0xF0, 8 bytes) | ROUTE CP128/EXT_TOL, SLS via EFDT, SignedMultiTable |
| `ch11-25-slt.pcap` | ATSC ALP | 68 / 95 kB | Three broadcasters in one stream (LLS group 0 from 10.133.16.20, group 1 from 172.16.200.1, plus 10.4.49.5): SLT bsid 564 (4 services) and SLT bsid 1409 (6 services incl. MMTP service WNUV and ESG); CDT; UserDefined (0xFF, "UDST"); 4 LMTs; ROUTE CP 128/8/3/1 with EXT_TOL and EXT_FTI; MMTP; RTP/TS PT 33 and RTP PT 96 private flows; **1 truncated frame** (ALP length 1500, captured 1498) | Multi-SLT, multi-group LLS, flow routing, NRT packages (CP 3) |
| `lls-tables-alp.pcap` | ATSC ALP | 8 / 19 kB | Only LLS: SMT v0 (SLT bsid 668), CDT v45, SMT v1 (SLT 564), CDT v74, SystemTime with the **`http://www.atsc.org/...` namespace variant**, SLT bsid 1409 v60, AEAT, UserDefined | LLS parser golden tests |
| `mp4-ftyp-styp-sidx.pcap` | ATSC ALP | 5 / 6.3 kB | ROUTE media: TSI 30 CP 128 EXT_TOL (complete 1338-byte `styp/sidx/moof/mdat` segment, close-object flag set once); TSI 300 CP 8 with **EXT_FTI** (597-byte init segment TOI 1 complete; media TOI with transfer length 1918) | EXT_FTI layout, object completion, segment typing |

### 3.2 Large real-life captures (`samples/dektec/`)

Downloaded from DekTec's FTP server (`ftp.dektec.nl`, user `dektec`, folder `pub/persistent`, subfolders
`ATSC3-STLTP/` and `ATSC3-PCAP/` kept as they are); `setup.sh` mirrors them after asking for the password.

| File | Link type | Packets / size / duration | Content | Use in tests |
|------|-----------|---------------------------|---------|--------------|
| `ATSC3-STLTP/BWI_20230307_1742.pcap` | Ethernet (UDP) | 667 k / 943 MB / 300 s, 25 Mbit/s | **STLTP** from a Baltimore (bsid 1409, Sinclair) broadcast gateway: one outer tunnel flow 192.168.59.100:58532 → 239.232.11.51:30000, RTP PT 97, 1372-byte packets, `protocol_version` 0, no ST 2022-1 FEC streams, no signing. Inner streams 239.0.51.48: ports 30000/30001/30002 (BBP, PT 78, PLP 0/1/2), 30064 (Preamble, PT 77) and 30065 (T&M, PT 76), no 30066. BBPs are all 5916 bytes (LDPC 64800, rate 11/15, BCH) with `OFI=10` long-extension headers carrying all-padding extensions; pointer 8191 in 10 % of BBPs. Decodes to the same six-service SLT (WBFF, WBAL, WMAR, ESG, WMPT, WNUV-MMT) as the small samples plus LMT, AEAT, UDST, DRM-protected WBAL (CENC/Widevine in MPD), ESG with gzip SGDD/SGDU/PNG, NRT app packages, 15 private RTP/TS radio flows. One stray ICMP packet from 0.0.0.0 carries an ASCII marker (`bootAddress=224.0.23.60;bootPort=4937;bootPlp=0;stltpAddress=239.232.11.51;stltpPort=30000;`) | STLTP demux, BBP deframer, whole-chain golden test, MMTP v01 header, CENC signaling |
| `ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap` | Ethernet (UDP) | 436 k / 644 MB / 300 s, 17 Mbit/s | **Raw IP multicast** (no ALP, no LMT) from 10.5.120.103: LLS group 1 (SLT bsid 24 without XML namespace, SystemTime without XML declaration, AEAT tornado example), three ROUTE A/V services 239.255.24.1/2/3:8000 (TSI 1 video HEVC up to 4K, TSI 2 audio AAC, TSI 3 IMSC captions, TSI 4 app package), ESG service 239.255.24.99 (TSI 999 SGDD, 1000 SGDU, gzip). CP 128 everywhere with EXT_TOL; CP 130 for the app package; close-object flag used. A DekTec `.xa3` modulator profile sits beside it (PLP 0, QAM-256 9/15, LMT generated by the modulator) | IP bypass path, multi-service ROUTE, fileTemplate/$Number$ mapping, app packages |
| `ATSC3-PCAP/UniSoft-2xROUTE+1MMT-5-November-2019.pcap` | Ethernet (UDP) | 551 k / 765 MB / 300 s, 20 Mbit/s | As above but service 3 is **MMTP** (239.255.24.3:8000): packet_id 0 signaling (PA/MPT complete table 0x11, package `usc03m-1`, `mmt_atsc3_message` USBD), packet_id 20 HEVC, 21 AC-4, subset MP tables with `MPU_timestamp_descriptor`, HRBM and HRBM-removal messages | MMTP end to end, second encoder for header layout |
| `ATSC3-PCAP/*.xa3` | XML | — | DekTec StreamXpress modulation profiles: PLP parameters and the LMT entries generated for the raw-IP pcaps | Sidecar for the virtual tuner (5.1.2, L1Info and synthetic LMT) |
| `ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019_sls/` | files | 1058 files | Objects DekTec extracted from the capture beside it (media segments, init segments, `uscapp.zip.multipart`, SGDD/SGDU) | Cross-check of object recovery (byte-identical output expected) |

Key findings that shape the design:

1. **All three ingest forms now have real captures** (DLT 289, STLTP, raw IP). The STLTP capture
   decodes through tunnel → BBP → ALP → IP → LLS/ROUTE/MMTP (`atsc3scan --limit 320000`) with zero sequence gaps,
   zero BBP pointer errors and 282 642 ALP packets recovered from 320 000 tunnel packets; its
   the same Baltimore broadcasters (bsid 1409) appear in the small DLT 289 samples, which were captured
   independently two months earlier (3.1).
   Only a native BBP-output tuner capture is still missing; the `alp2bbp` converter covers that path.
2. **Vendor-private ALP packets exist.** `08 00 05 F1 00` / `08 00 05 F0 07 <8 bytes>` are ALP
   single packets with HM=1, payload length 0, and a user-private header extension (types 0xF0/0xF1).
   A/330 forbids zero-length payloads, yet capture tools emit them (likely PLP/time markers). The
   parser must accept, surface them as `AlpEvent::PrivateExtension`, and continue.
3. **Captures can be truncated** (record shorter than the ALP length). Treat as a dropped packet,
   never read past the buffer.
4. **Several broadcasters share one RF channel** (different `LLS_group_id`, different source IPs,
   different `bsid`). Every table and session must be keyed by group/bsid, not assumed singular.
5. **SLT appears both as a plain table (0x01) and inside a SignedMultiTable (0xFE)**, with CDT
   (0x06) carrying the certificates. The same SLT content must dedupe by (group, table_id, version).
6. **XML namespace variants** (`tag:atsc.org,2016:...` vs `http://www.atsc.org/...`) and CRLF
   line endings occur. Parsers match on local element names and treat the namespace as advisory.
7. **ROUTE in the wild:** CP 128 with EXT_TOL (24-bit, HET 194) on media flows; CP 8 with EXT_FTI
   (HET 64) and **no** EXT_TOL; CP 1 for SLS; CP 3 (unsigned package) for NRT; close-object flag (B)
   almost never set; PSI='10'; LCT header length 4 or 5 words (8 with EXT_FTI).
8. **EXT_FTI layout follows RFC 5775/RFC 5445** (Transfer Length first), e.g.
   `40 04 | 00 00 00 00 02 55 | 00 00 | 00 00 | 00 00 00 00` = TL 597, reserved, ESL 0, MSBL 0.
   A layout with the FEC Encoding ID first is incorrect and must not be used.
9. **SLS packaging varies.** The ESG service's TSI 0 carries an EFDT at TOI 0 describing a single
   `multipart/signed` SLS object at TOI 196829 (= 0x300DD: Annex C bits U+S, version 221, matching
   `afdt:efdtVersion="221"`). The stack must support Annex C TOI filtering, EFDT-described SLS, and
   `multipart/signed` (A/360) in addition to the plain `multipart/related` package.
10. **MMTP version 01 packets carry a 16-bit QoS classifier** after `packet_counter`
    (bytes `A4 00`), followed by the MPU payload header whose `length` equals the remaining UDP
    payload exactly. The parser must handle this (Section 5.10.2).
11. **Private RTP flows** (MPEG-TS over RTP PT 33, dynamic PT 96) are listed in the LMT and
    described by a UserDefined LLS table. The stack needs a raw UDP flow tap so applications can
    consume such flows.
12. **Timestamps in the ALP samples are inconsistent** (several files span hundreds of thousands of
    seconds; some appear concatenated from different captures). Real-time replay must clamp
    non-monotonic or absurd inter-packet gaps.
13. **STLTP as deployed (2023 gateway):** `protocol_version` is 0, not the '01' the 2024 standard
    says; the outer destination port is simply 30000 (not tied to a PLP); `packet_offset` is non-zero
    on almost every marked packet (inner packets straddle tunnel packets routinely); Preamble and T&M
    arrive once per physical-layer frame (about four per second); the Security Data Stream (30066),
    RTP header extensions and FEC streams are absent. Every BBP is exactly `Kpayload/8` = 5916 bytes
    and uses `OFI=10` with `EXT_TYPE=111` (all padding) — so padding, not the counter, is the common
    extension, and ALP packets (including their headers) are split across BBP boundaries constantly.
14. **MMTP version 01 header resolved (Q3).** Two independent encoders (UniSoft 2019: byte0 `0x40`;
    Sinclair 2023: `0x62` on MPU packets, `0x61` on signaling packets) all carry a 16-bit word
    `{reliability_flag(1), type_of_bitrate(2), delay_sensitivity(3), transmission_priority(3),
    flow_label(7)}` immediately after `packet_counter`, **regardless of the three low flag bits of
    byte 0** (`0x8000`, `0xA400`, `0x9BFF` observed). The MPU payload `length` field equals the
    remaining bytes only with that word present. Treat it as a fixed field of the version-01 header.
15. **Three EFDT packagings exist** for the SLS channel: a bare `FDT-Instance` at TOI 0 with
    `Content-Location="SLS"` and `Transfer-Length` (Sinclair), an `EFDT` wrapper element in the
    S-TSID namespace around the `FDT-Instance` (UniSoft), and the S-TSID-embedded `SrcFlow/EFDT`.
    The `afdt` namespace appears both with and without the trailing slash.
16. **SLS media types on air are the older `application/route-usd+xml` and
    `application/route-s-tsid+xml`**, sometimes with `; charset=utf-8` parameters; the envelope's
    `metadataURI` values (`usbd.rusd`, `stsid.sls`, `mpd.mpd`, `usbd.xml`, …) are matched to parts
    by `Content-Location`.
17. **Init-segment TOI conventions differ**: TOI 1 (Sinclair) or TOI 2100000000 (UniSoft) with an
    explicit `File` entry, media segments by `fileTemplate` (`video-$TOI$.mp4v`, `test-0-$TOI$.m4pv`)
    where TOI equals the MPD `$Number$` segment number; CP 8 with EXT_FTI (Sinclair) or CP 128 with
    EXT_TOL and the close-object flag (UniSoft).
18. **NRT and ESG in practice:** ESG is a ROUTE service whose channels carry gzip-encoded
    `application/vnd.oma.bcast.sgdd+xml` (SGDD), `application/vnd.oma.bcast.sgdu` (SGDU, up to 541 kB
    inflated) and PNG icons, all with `Content-Encoding="gzip"`; app packages are
    `multipart/related` objects on dedicated high TSIs (CP 3 or CP 130, up to 3.8 MB, with
    `appContextIdList`) whose parts use `Content-Transfer-Encoding: binary`.
    **Observed A/344 applications:** four services signal a HELD. Sinclair WBFF (ROUTE) names
    `App.pkg` (3.88 MB, entry `index.html`, broadband fallback on cdn.sinclairplatform.com,
    `appRendering="false"`, `coupledServices="5002"`) plus `weather-latest.pkg` (3.3 MB), `Alert.pkg`,
    `single-alert.pkg` and `weather-7day.pkg` (JSON only) on five-digit TSIs of the SLS session. `App.pkg`
    is a continuous carousel at ≈245 kbit/s (6599 packets, 9.2 MB in the 300 s capture, 2.4 copies), so a
    receiver assembles a complete copy ≈126 s after it opens the channel whichever moment it joins (the
    ROUTE session merges the ranges of consecutive repetitions); the small packages arrive within seconds. WNUV (MMTP) carries the
    same HELD as `mmt_atsc3_message` 0x0003 and its USBD `ROUTEComponent` points at a separate ROUTE session
    (239.255.54.1:15007, source 172.16.200.1) whose SLS bundle is an envelope plus an S-TSID only (TOI
    0x20007) listing the same five packages for `WNUV:1409`. WMAR's HELD is broadband-only
    (`bbandEntryPageUrl` on ota.ewscloud.com, no package). UniSoft usc01 names `uscapp.zip.multipart`
    (1092 B, entry `uscapp/home.html`: a canvas drawing "ATSC 3.0"), carouselled ≈19 times per second
    with `afdt:appContextIdList="uscapp"` on the FDT-Instance rather than the File. Package parts carry
    `Content-Location` and `Content-Transfer-Encoding` but no `Content-Type`; the media type comes from
    the `metadataEnvelope` item (`contentType`, `version`). `App.pkg` is a React build ("Broadcast
    Application", 315 files, `app-version.json` 20230228.7) whose receiver client reads `wsURL` from the
    page query (default `ws://localhost:8889`), opens a plain `WebSocket` and calls `org.atsc.query.service`,
    `query.baseURI`, `query.deviceInfo`, `query.cc`, `query.alerting`, `subscribe` (`alertingChange`,
    `requestKeyTimeout`), `request.keys`, `relinquish.keys`, `scale-position`, `setRMPURL` and the
    non-standard `org.atsc.query.MPDUrl`; its navigation switches on `KeyboardEvent.keyCode`.
    **Observed SGDU layout (BWI, Digicap encoder):** big-endian `unit_header` = `extension_offset`(32)
    + one reserved byte + `n_o_service_guide_fragments`(32), then n × {`fragmentTransportID`(32),
    `fragmentVersion`(32), `offset`(32)}; the payload slots start right after the header, each slot is
    `fragmentEncoding`(8) `fragmentType`(8) + the XML fragment (newline-terminated), `offset` is
    relative to the payload start and `extension_offset` is 0. `sgdu_service_schedule_4153` holds 5
    Service + 5 Schedule fragments, `sgdu_content_3378` 458 Content fragments; `sgdd_1919` (version 30)
    describes both units under one `DescriptorEntry` with `TimeGroupingCriteria` in NTP seconds and
    `Transport transmissionSessionID="60"`. Service fragments carry `globalServiceID` equal to the SLT's
    (two services use `https://doi.org/10.5239/…` ids, three `tag:sinclairplatform.com,…`), `ServiceType`
    228, `Name/@text`, `ATSC3ServiceExtension` with `Icon` (an NRT file name on TSI 80) and
    `MajorChannelNum`/`MinorChannelNum`; Content fragments use `Name/@text`, `Length` (xs:duration),
    `Genre/@href` with the A/153 classification scheme, `ContentAdvisoryRatings` (`RatingDescription`
    "TV-G"/"TV-PG" plus one `RatingDimVal`) on 112 of 458, and `ContentIcon` with an **http URL**
    (tmsimg.com), not a broadcast file. Schedules reference ~95 windows per service for the 2.8 days of
    the grouping window. The whole guide carousels about every 8 s (SGDD) / 35 s (SGDUs).
    **Observed (UniSoft SGDD):** the root carries no default namespace (`xmlns:ns2` only),
    `TimeGroupingCriteria` and fragment times are **Unix seconds**, not NTP, and the `Transport` element
    carries `ipAddress`/`port`/`srcIpAddress`; the SGDU sits at TOI 0 of its session. The stack treats any
    OMA timestamp below the NTP value of 1970-01-01 as Unix seconds.
19. **Protected service:** WBAL is `protected="true"` with `drmSystemID=urn:uuid:edef8ba9-…`
    (Widevine); its MPD carries `ContentProtection` (`urn:mpeg:dash:mp4protection:2011` with
    `cenc:default_KID` and the Widevine system id with `cenc:pssh`). The stack passes these through.
20. **MMT MPU mode in practice (UniSoft `usc03m` and Sinclair WNUV, both Bento4 packagers behind ATEME
    encoders):** one movie fragment per MPU; FT=0 and FT=1 DUs are never fragmented, FT=2 MFUs are
    (f_i 1/2/3, `frag_counter` counting down to 0, up to 238 fragments for a 270 kB IDR) and **the 14-byte
    MFU DU header is repeated in every fragment**; aggregation, non-timed MFUs and FT≥3 never occur. The
    DU-header `offset` is 0 on every MFU; the usable position and length sit in a **34-byte ISO 23008-1
    `MMTHSample` that prefixes every MFU payload** (`sequence_number`, `trackrefindex`, `mf_seq`,
    `samplenumber`, `priority`, `dependency_counter`, `offset` = sum of the previous trun sizes, `length`,
    then an 11-byte `muli` box); the media sample follows. FT=0 = `ftyp(mpuf) mmpu moov` where the moov
    carries a second **hint track** (`hdlr hint`, sample entry `mmth`, trex `default_sample_size` 34) and
    is byte-identical for every MPU except the `mmpu` box (which repeats the MPU sequence number; in the
    Sinclair feed its asset id differs from the MP table's 16-byte id — only `packet_id` binds them). FT=1
    = `moof` + the 8-byte `mdat` header; the moof has a media `traf` and a hint `traf` whose trun
    `data_offset` is wrong on every MPU; the announced mdat size equals 8 + sum(trun sizes) + 34·n.
    **Emission order differs**: UniSoft sends FT0 < FT1 < MFUs, Sinclair sends FT0 < all MFUs < FT1 (the
    moof about 1 s after the first MFU). **tfdt**: UniSoft continuous across MPUs on one clock for audio
    and video; Sinclair 0 in every moof. The `MPU_timestamp_descriptor` travels in the subset MP table on
    the asset's own packet_id one packet before the MPU's first packet; UniSoft's advances exactly 2.000 s
    per 2.002 s MPU (a coarse wall clock), Sinclair's exactly the MPU duration; both encoders bind it to
    the fragment's `tfdt` (decode origin), not to the first sample in presentation order as A/331 §8.1.2.3
    says (one frame of difference on video). The MMTP packet `timestamp` is unrelated to presentation
    (UniSoft's clock is off by 32 384 s; Sinclair's is the UTC send time, 4–5 s *after* presentation). UniSoft
    sends every packet_id from a different UDP source port (key flows by destination only) and its MP table
    declares the AAC-LC audio asset as `ac-4`: codecs must come from the `stsd` sample entry. HRBM (0x0204)
    says `fixed_end_to_end_delay` 1000 ms on both; HRB removal (0x020A) appears only on UniSoft. Browsers
    reject any unknown top-level box (`mmpu`, `evti`) and bare 4CC codecs strings, so the init handed to
    MSE is `ftyp(iso6) moov` without the hint track and the fragment is `styp moof' mdat'` with the hint
    traf removed, `data_offset` repaired and only the media samples in the mdat.
20. **Schema deviations on air** (all must parse): SLT with no namespace at all; `Name lang="eng"`
    instead of `xml:lang`; `MediaInfo contentType="text"` (not in the schema enumeration); AEAT
    text containing HTML-escaped markup; `S-TSID/RS` without any attributes; UDST private tables
    describing 15 "OMLradio" services carried as RTP/MPEG-TS.

---

## 4. Architecture

### 4.1 Layer diagram

```
   RF tuner (BBP)           RF tuner (ALP)                Pcap virtual tuner (5.1.2)       UdpSource
          |                        |             +---------------------------------------+     |
          |                        |             | detect: DLT 289 -> ALP                |     |
          |                        |             |         STLTP in UDP -> StltpDemux    |     |
          |                        |             |         BBP container -> BBP          |     |
          |                        |             |         plain IP (not STLTP) -> BYPASS|     |
          |                        |             | pace:   real-time | non-real-time     |     |
          |                        |             +----+--------+---------+---------+-----+     |
          |                        |                  |BBP     |ALP      |STLTP    |IP         |STLTP/IP
          v                        |                  v        |         v         |           |
   +-------------+                 |                  |        |  +---------------+|           |
   | BbpDeframer |<----------------|------------------+--------|--| StltpDemux    |<-----------+
   +-------------+                 |                           |  +---------------+|           |
          |  ALP packets (plp_id, bytes)                       |                   |           |
          +-------------+----------+---------------------------+                   |           |
                        v                                                          |           |
               +------------------+   LMT/RDT   +-------------------+              |           |
               |   AlpDecap       |------------>| LinkSignaling     |              |           |
               +------------------+             +-------------------+              |           |
                 | IPv4 | type 010 (ROHC) | type 111 (TS) | other                  |           |
                 v      v                 v                                        |           |
            +-----------+  +----------+  +---------+                               |           |
            |  IpDemux  |<-| Rohc     |  | TsSink  |                               |           |
            +-----------+  +----------+  +---------+                               |           |
                 ^                                                                 |           |
                 +------------------- IP bypass path (I4) --------------------------+-----------+
                 |  UDP datagrams routed by (dst ip, dst port[, src ip])
      +----------+-------+------------------+-----------------------+---------------+
      v                  v                  v                       v               v
 +---------+     +--------------+   +--------------+        +------------+   +-----------+
 | LLS     |     | RouteSession |   | MmtpSession  |        | RawFlowTap |   | (future)  |
 +---------+     +--------------+   +--------------+        +------------+   +-----------+
      |                 |                  |
      |   SLT/STT/AEAT  | objects, SLS     | MPUs, MP table, mmt_atsc3_message
      v                 v                  v
 +------------------------------------------------------------------+
 |                        ServiceManager                             |
 |   services, SLS binding, session lifecycle, ESG/EA/NRT routing    |
 +------------------------------------------------------------------+
                                   |
                                   v
                      Application API (callbacks + queries)
                                   |
         +-------------------------+---------------------------+
         |                                                     |
 +-------+--------------------+                 +--------------+-------------------+
 | integration: web (5.17)    |                 | integration: VLC plugin (5.19)   |
 | REST + SSE + WebSocket +   |                 | atsc3:// MRL, programs = services|
 | local DASH origin          |                 | CMAF/MPU fragments -> VLC ES     |
 +-------+--------------------+                 +--------------+-------------------+
         |  HTTP (localhost)                                   |  libvlccore
   Vue 3 frontend / Playwright e2e (5.18)              VLC / cvlc / python-vlc e2e
```

The two boxes below the Application API are **integrations**: programs that embed the stack, drive it through
the public API and present the services through an interface of their own — today the web server with its daemon
and frontend (5.17, 5.18), the VLC plugin (5.19) and the GStreamer set-top-box player (5.20), later a DTV middleware. They live under
`implementation/integrations/` (Section 8), consume only the public API, and nothing in the core depends on them.
Tuners (5.1, implementations under `tuners/`) feed the stack from below, integrations consume it from above, and the test-only
drivers (the cvlc runner, Playwright, `RecordingSink`) are harnesses.

The IP bypass path is the only way plain-IP captures enter the stack; it is also usable by a
real deployment that receives ATSC 3.0 IP multicast over a LAN (e.g. from an external gateway).

### 4.2 Data-flow and ownership rules

- Packets travel downward as `ByteView` (pointer + length) into pooled buffers owned by the
  source. A layer that must retain bytes beyond the call (segmentation reassembly, IP fragment
  reassembly, object recovery) copies into its own storage.
- Assembled deliverables (objects, MPUs, tables) are handed to the application as
  `std::shared_ptr<const Object>`; the application may keep them without copying.
- Every layer has `reset()` (channel change) and `stats()`.

### 4.3 Threading model

- One **pipeline thread per source** (tuner or file). All layer code runs on it; no locks inside
  the pipeline.
- Application callbacks are invoked on the pipeline thread by default and **must not block**.
  An optional `DispatchQueue` adapter marshals callbacks to an application thread.
- Control calls from other threads (`selectService`, `subscribeFlow`, `reset`) post commands to
  the pipeline thread's command queue; they return futures where a result is needed.
- Broadband helpers (5.13) run on caller-provided executors; the stack never creates network
  sockets except the UDP sources in 5.1.

### 4.4 Error philosophy

Each parser returns `Result<T>` (`std::variant<T, Error>`-style, no exceptions on the packet
path). Errors are counted per layer (`Stats`), logged at a rate-limited level, and reported to the
application through `IEventSink::onError` when they affect a selected service. Exceptions are used
only for programming errors and construction-time failures.

---

## 5. Module specifications

Module names below double as namespaces (`atsc3::phy`, `atsc3::alp`, ...). Header sketches are
illustrative; the normative content is the behaviour text.

### 5.1 Tuners

Every source, real or virtual, implements the engine's tuner contract (`include/atsc3/service/tuner.hpp`;
implementations live under `tuners/`, Section 8). A tuner delivers exactly one *kind* of unit — Baseband
Packets, ALP packets or IP datagrams — and states which kinds it can deliver. The engine picks one when it
opens the tuner and couples it to the receive chain at that layer, building only the layers from there up
(BBP → deframer → ALP decap → link signaling / IP demux; ALP → decap → …; IP → demux only).

```cpp
namespace atsc3::io {                        // delivery units, shared with the layers (io/units.hpp)
struct UnitTime       { int64_t t_ns; int64_t capture_ns; };                    // stack time; capture time (0 = unknown)
struct BasebandPacket { uint8_t plp_id; bool error; UnitTime time; ByteView bytes; };
struct AlpPacket      { uint8_t plp_id; UnitTime time; ByteView bytes; };       // plp_id may be kUnknownPlp
struct IpDatagram     { uint8_t plp_id; UnitTime time; ByteView bytes; };       // starts at IPv4 header
struct L1Info { … };                                                            // PLPs, lls flags, LMT entries (sidecar or L1)
}

namespace atsc3::service {                   // the contract lives with the engine (service/tuner.hpp)
enum class TunerOutput { Bbp, Alp, Ip };     // the layer a tuner couples at
enum class TunerError { None, NoAtscContent, FileNotFound, BadFile, AlreadyStarted, NotOpen, UnsupportedOutput, Driver };
struct TunerState { bool ready; std::string error; bool paused, finished; int64_t position_ns, duration_ns; SignalStats signal; };

class ITunerSink {                           // implemented by the engine; called on the tuner's thread
public:
    virtual void onBbp(const io::BasebandPacket&) = 0;   // -> BbpDeframer (5.3)
    virtual void onAlp(const io::AlpPacket&) = 0;        // -> AlpDecap (5.4)
    virtual void onIp(const io::IpDatagram&) = 0;        // -> IpDemux (5.6), bypassing PHY/ALP/link
    virtual void onTunerEvent(const TunerEvent&) = 0;    // lock/unlock, format detected, time jump, wrap, end of stream
};

class ITuner {                               // implemented per demodulator driver and by the pcap virtual tuner
public:
    virtual std::string name() const = 0;                     // "pcap", "sony"
    virtual std::string source() const = 0;                   // capture path, frequency, multicast address
    virtual std::set<TunerOutput> outputs() const = 0;        // every kind it can deliver
    virtual TunerError start(ITunerSink&, TunerOutput) = 0;   // deliver that kind until stop(); UnsupportedOutput otherwise
    virtual void stop() = 0;
    virtual std::optional<io::L1Info> l1() const = 0;         // PLP list, L1D_plp_lls_flag; nullopt if unknown
    virtual TunerState state() const = 0;
    virtual bool pause(bool) { return false; }                // replay tuners only
};
}
```

The engine (`service::Engine::open(std::shared_ptr<ITuner>, OpenOptions)`) keeps the tuner until `close()`,
chooses the output (`OpenOptions::output`, else the highest layer the tuner offers — IP, then ALP, then
BBP: every layer above BBP is work the tuner has already done), applies the sidecar LMT from `l1()` to the
PLP map, and reports the tuner's name, source, output and state in its status; `tuner()` hands the tuner
back for implementation-specific controls (replay pacing, position, statistics). Units delivered for a
layer the chain was not built for are counted and dropped (`Status::misrouted_units`).

`L1Info` is optional but valuable: PLPs with `L1D_plp_lls_flag=1` are where LLS/LMT must appear
(A/330 §7.1.1); the stack uses it to decide where to look first after tuning. The stack ships no
vendor RF driver; it ships this contract, the pcap virtual tuner (`tuners/pcap`), and the UDP source.

#### 5.1.1 RF tuners

Thin wrappers around a demodulator driver. They deliver `Bbp` or `Alp` (never `Ip`), attach the
PLP id to every unit, and raise `TunerEvent::Unlock/Lock` so the stack can flush and resync.
Timestamps (`t_ns`) are taken from the host monotonic clock at delivery.

#### 5.1.2 Pcap virtual tuner

`pcap::VirtualTuner` (`tuners/pcap`, library `atsc3::tuner_pcap`) implements `ITuner` over a capture file.
It is the source used for all automated tests and for running the stack on a Linux PC without an RF
tuner. To the layers above it is indistinguishable from a real tuner, except that its one output may be `Ip`.

**File reading.** Classic pcap (both magic numbers, both endiannesses, µs and ns resolution) and
pcapng (SHB/IDB/EPB/SPB, one IDB per interface, per-record link type). Own implementation, no
libpcap dependency. Records longer than the file (truncated tail) end the stream cleanly.

**Format detection.** At `start()` the virtual tuner probes the file (first `probe_packets`,
default 256 records, or `probe_seconds`, default 2 s of capture time, whichever ends first; the
probe is rewound before delivery begins) and picks one of five operating modes:

| Detected content | Operating mode | `outputKind()` | Path into the stack |
|------------------|----------------|----------------|---------------------|
| Link type 289 `ATSC_ALP` | **ALP** | `Alp` | `onAlp` → AlpDecap. PLP id = `kUnknownPlp` unless a vendor marker format is configured (Q2) |
| Link type 147–162 with the BBP container header (6.2) | **BBP** | `Bbp` | `onBbp` → BbpDeframer |
| Ethernet / raw IPv4 / Linux SLL / SLL2 whose UDP payloads are CTP tunnel packets: RTP version 2 and payload type 97 (STLTP), 82 (ALPTP) or 81 (DSTP), with consistent sequence numbers on at least `stltp_min_packets` (default 8) datagrams of one (dst, port) | **STLTP** | `Bbp` (PT 97, via the embedded StltpDemux), `Alp` (PT 82), `Ip` (PT 81) | tunnel packets → StltpDemux (5.2) → `onBbp` / `onAlp` / `onIp` |
| Ethernet / raw IPv4 / SLL / SLL2 with IPv4 UDP datagrams that are **not** a CTP tunnel and look like ATSC 3.0 content (LLS on 224.0.23.60:4937, or multicast UDP whose payload parses as LCT v1 or MMTP v01) | **IP bypass** | `Ip` | `onIp` → IpDemux. PHY, ALP and link layers are not instantiated for this source |
| None of the above (LAN chatter only: SSDP/mDNS multicast, TCP streams, HTTPS) | **Unsupported** | — | `start()` fails with `TunerError::NoAtscContent` and a human-readable summary of what was seen |

Reference verdicts: the five small samples → ALP; `BWI_20230307_1742.pcap` → STLTP (outer PT 97 on
239.232.11.51:30000, `protocol_version` 0 accepted); both UniSoft captures → IP bypass (LLS on
224.0.23.60:4937 present); the synthetic SSDP/mDNS-only capture of the tests → Unsupported.

Detection rules in detail:

1. The link type decides between ALP, BBP-container and "IP-bearing" files. For IP-bearing files
   the link header (incl. 802.1Q tags, SLL/SLL2 headers) is stripped and only IPv4 is considered;
   IPv6, ARP and other ethertypes are ignored (counted).
2. Among IPv4/UDP datagrams, a flow is classified as a CTP tunnel when the UDP payload starts
   with an RTP v2 header (`byte0 & 0xC0 == 0x80`), CC=0, PT ∈ {97, 82, 81}, and the 16-bit
   sequence advances by 1 (modulo reordering within `stltp_reorder_window`) across the probe.
   A configured `stltp.address/port` overrides detection.
3. If at least one tunnel flow is found the mode is **STLTP**; remaining non-tunnel UDP datagrams
   in the same file are dropped and counted (`mixedContent` warning), unless `allow_mixed=true`
   in which case they are additionally injected through the IP bypass.
4. Otherwise, if the probe contains at least one IPv4/UDP datagram that is recognisably ATSC 3.0
   (an LLS datagram to 224.0.23.60:4937, or a multicast datagram whose payload parses as an LCT
   header with V=1 or an MMTP header with version 01 and type 0x00/0x02), the mode is
   **IP bypass**. All IPv4/UDP datagrams are then injected (not only the recognised ones), so
   private flows such as the RTP/TS streams in the samples still reach `RawFlowTap`s. Non-UDP IP
   packets are dropped and counted. An optional `ip_filter` (dst address/port list) restricts
   injection. Ordinary LAN chatter (SSDP, mDNS, DNS) alone does not qualify.
5. Otherwise the file is **Unsupported**; `force_mode=IpBypass` overrides this for captures of
   unusual content. The analyzer tool (6.1) prints the same verdict and the per-flow evidence.
6. The chosen mode is reported through `TunerEvent::FormatDetected{mode, linkType, details}` and
   logged. `force_mode` in the configuration disables detection (used by tests and by the
   converters).
7. Non-IP/UDP packets that appear inside ATSC captures (ARP, ICMP such as the DekTec marker packet
   `bootAddress=…;stltpAddress=…;stltpPort=…;` sent from 0.0.0.0) are ignored and counted; the
   marker's text is logged because it names the LLS and tunnel addresses.
8. **Sidecar metadata (DekTec `.xa3`).** The virtual tuner supports exactly one sidecar format: the
   DekTec StreamXpress ATSC 3.0 modulation profile (`ModulationParameters` XML, extension `.xa3`).
   Resolution order: `sidecar=<path>` option, else `<capture>.xa3` / `<capture-without-extension>.xa3`
   beside the capture, else any `.xa3` in the capture's directory whose
   `Atsc3/Subframe/Plp/Source/PcapFile@filename` ends with the capture's file name (the profiles
   store Windows paths such as `U:\pub\…\UniSoft-3xROUTE-24-October-2019.pcap`); `sidecar=off`
   disables the lookup. Fields used:
   - `Atsc3/General@lls_flag`, `@bandwidth`, `@minor_version`; `Atsc3/Preamble@fft_size`,
     `@guard_interval`, `@pilot_dx`, `@l1_detail_version` → informative entries of `L1Info`.
   - `Atsc3/Subframe/Plp@plp_id`, `@modulation`, `@code_rate`, `@lls_flag`, `@layer`,
     `@fec_code_length`, `@fec_outer_code`, `@plp_type`, `@ti_mode` → one `L1Info::Plp` per element
     (`L1D_plp_id`, `L1D_plp_lls_flag`, modulation/code rate/FEC type/length); `Source/PcapFile` ties
     the PLP to the capture, so a profile with several `Plp` elements sourcing different pcaps maps
     each capture to its PLP id, and all IP datagrams of this capture are tagged with that PLP id
     instead of `kUnknownPlp`.
   - `Atsc3/Signaling/Lmt/Plp[@plp_id]/Multicast/Src{ip0..ip3,port}` and `Dst{…}` → a synthesized
     LMT instance (`signaling_version` 0, `SID_flag`/`compressed_flag` 0) delivered through the normal
     link-signaling path (5.5.1) with origin `Sidecar`, so the PLP map exists for IP-bypass captures
     exactly as if the modulator's ALP output had been captured. `Lmt@gen_mode="from_plp_source"`
     means the modulator derived the entries from the pcap; the tuner additionally verifies them
     against the flows actually seen and reports mismatches.
   - `ChannelModelling`, `IqOutput`, `TimeInfo` and the `ti_*`/`hti_*`/`ldm_*` details are ignored.
   Unknown elements never fail the load; a malformed profile is logged and ignored. When a sidecar is
   used, `TunerEvent::FormatDetected.details` names it, and the analyzer (6.1) prints the same
   information. No other vendor sidecar format is supported; if one appears later it gets its own
   reader behind the same `ISidecar` interface.
9. pcapng files with several interfaces may mix link types (e.g. one IDB 289 and one Ethernet).
   Each interface is classified independently; the file's mode is the "highest" present in the
   order BBP > STLTP > ALP > IP bypass, and records from interfaces of other modes are dropped
   and counted, unless `allow_mixed=true`.

**Pacing modes.**

- **Real-time** (`Pacing::RealTime`): delivery is throttled so that the inter-packet spacing of
  the capture is reproduced. The reference is the record timestamp of each pcap/pcapng record
  (in STLTP mode the inner RTP timestamps may optionally refine it, `use_rtp_timing`). Rules:
  - Delivery time of record *n* = `t_start_host + (ts_n − ts_0) / speed`, with `speed` default 1.0
    (0.5 = half speed, 2.0 = double; `speed` is the only knob that makes "real time" faster/slower).
  - Negative gaps (non-monotonic timestamps, Observed finding 12) are treated as 0.
  - Gaps larger than `max_gap` (default 2 s) are clamped to `max_gap` so that concatenated or
    sparse captures do not stall the pipeline; a `TunerEvent::TimeJump` is raised with the real gap.
  - Sleeping uses a deadline-based wait (`clock_nanosleep`/`condition_variable::wait_until`) and
    delivers all records whose deadline has passed in one burst (no per-packet oversleep
    accumulation). Target accuracy: ±1 ms median, ±5 ms p99 on a non-RT Linux kernel.
  - `t_ns` on every unit is the host monotonic time at which the record *should* have been
    delivered, so upper-layer timing (segment latency, MDE presentation time) behaves as on air.
  - The SystemTime/STT received from the file is mapped to the host clock by 5.15 as for a real
    tuner; wall-clock dates inside the capture are not adjusted.
- **Non-real-time** (`Pacing::NonRealTime`): records are read and injected as fast as the upper
  layers consume them (no sleeping). `t_ns` is set to the record's own timestamp (converted to
  an artificial monotonic base), so all time-based logic in the stack still sees the captured
  timeline, only compressed. Object timeouts, carousel timers and statistics intervals therefore
  use `t_ns` (data time), never the host clock, in this mode. An optional `max_bitrate` cap
  exists for consumers that cannot keep up.
- Pacing is selectable at construction and changeable while stopped. Default: `RealTime` for the
  application, `NonRealTime` for tests and tools.

**Other behaviour.**

- `loop=true` replays the file repeatedly for soak tests; at each wrap the tuner raises
  `TunerEvent::Wrap`, the stack resets session state as on a retune, and (in real-time mode) the
  timeline continues without a gap.
- `start_offset`/`duration` select a window of the file by capture time.
- `signal()` returns synthetic values (lock = true while records remain, SNR = +inf) so that
  application UIs behave; `l1()` returns PLP information only in STLTP mode (from Preamble
  packets) or in BBP mode (set of PLP ids seen).
- End of file raises `TunerEvent::EndOfStream` after all pending units have been delivered; the
  stack then flushes reassembly state so that objects completed by the last packets are still
  delivered.
- Thread model: the virtual tuner owns one reader thread; delivery into `ITunerSink` happens on
  that thread, which is the pipeline thread for this source (4.3).

**Configuration summary** (`PcapVirtualTunerConfig`): `path`, `pacing`, `speed`, `max_gap`,
`loop`, `start_offset`, `duration`, `force_mode`, `allow_mixed`, `probe_packets`,
`probe_seconds`, `stltp{address,port,min_packets,reorder_window,use_rtp_timing}`, `ip_filter`,
`descramble` (BBP mode), `sidecar` (`auto` | path | `off`, default `auto`), `plp_marker_format` (Q2).

#### 5.1.3 `UdpSource`

Joins one or more IPv4 multicast groups (SSM optional) on a given interface and delivers
datagrams either as CTP tunnel packets (→ StltpDemux, `outputKind` Bbp/Alp) or as plain IP
(→ IP bypass, `outputKind` Ip), using the same detection logic as the virtual tuner on the first
datagrams, or a configured mode. Uses `recvmmsg` where available. This is the live counterpart
of the IP bypass path for deployments that receive the broadcast IP multicast over a LAN.

#### 5.1.4 Live capture

`PcapVirtualTuner` can also open a network interface through libpcap (optional dependency) and
apply the same detection and injection rules to live traffic (real-time by definition). This is
a convenience for lab setups; `UdpSource` is preferred in production.

### 5.2 STLTP demultiplexer (A/324)

Purpose: turn an STLTP tunnel stream into per-PLP BBPs (and optionally L1 information).

Behaviour:

1. **Outer tunnel (CTP, A/324 Table 6.1).** Accept RTP v2 packets with PT 97 (STLTP). Also accept
   PT 82 (ALPTP) and PT 81 (DSTP) as separate ingest modes (ALPTP yields ALP packets directly;
   DSTP yields IP datagrams). The SSRC word is repurposed: for PT 97 it is `timestamp()` (A/324
   Table 9.2); the 4 bytes after it hold `protocol_version(2) redundancy(2) number_of_channels(2)
   reserved(10) packet_offset(16)`. Marker bit = a tunneled packet starts in this payload at
   `packet_offset`.
2. **Sequence handling.** Reorder window of N packets (default 64) on the 16-bit RTP sequence;
   missing sequence → declare loss, drop the partially reassembled inner packet.
3. **SMPTE ST 2022-1 FEC** streams (same address, port N+2 row, N+4 column): Phase 3. Phase 1
   ignores them.
4. **Inner reframing (A/324 §6.3.1, §9.4).** Concatenate tunnel payloads; at `packet_offset`
   parse an inner IPv4/UDP/RTP packet; its IP total length gives its size; continue until a packet
   straddles the tunnel boundary, then wait for the next tunnel packet. Inner packets are
   multicast to 239.0.51.48 (A/324 §9) with ports:
   - 30000–30063: BBP stream for PLP 0–63 (PT 78)
   - 30064: Preamble (PT 77; L1 Basic + L1 Detail, A/324 Table 9.1)
   - 30065: Timing & Management (PT 76)
   - 30066: Security Data Stream (PT 79/80)
   - +100 per additional RF channel when `number_of_channels > 0` (channel bonding).
5. **BBP reassembly (A/324 §9.3.2.1).** For each PLP port: an RTP packet with M=1 starts a new
   Baseband Packetizer Packet Set; its SSRC is the total BBP length; accumulate consecutive
   sequence numbers until that many bytes are collected; any gap or premature M=1 discards the
   set. Output `BasebandPacket{plp_id = port-30000}` with `t_ns` from the RTP timestamp
   (Table 9.2: seconds LSBs + fraction; converted using the T&M bootstrap time when available).
6. Inner RTP header extensions (X=1, GMAC signing, A/324 §6.4) are skipped, not verified.
7. **Preamble parsing** (optional, `L1Parser`): decodes L1-Basic/L1-Detail fields needed above:
   `L1D_num_plp`, `L1D_plp_id`, `L1D_plp_lls_flag`, `L1D_plp_layer`, bsid is not in L1 (comes from
   SLT). Everything else is exposed raw for diagnostics.

Observed on the Baltimore gateway feed (`BWI_20230307_1742.pcap`, validated with
`stltp2alp` and `atsc3scan`): outer `protocol_version` 0 **must be accepted** (log once); `redundancy` and
`number_of_channels` 0; inner packets straddle tunnel packets almost always, so the reframer must
hold partial inner packets across tunnel packets and use `packet_offset` only to (re)synchronise;
BPPS sets consist of 4–5 RTP packets of 1372-byte tunnel payload for a 5916-byte BBP; Preamble and
T&M each arrive once per frame (~4 Hz) and can be used as frame-boundary and PLP-list hints.

Output: BBPs to 5.3. The STLTP module is also used by the `stltp2alp` tool (Section 6).

### 5.3 Baseband Packet deframer (A/322 §5.2)

Input: one complete BBP per call with its PLP id. State: per PLP.

Header parsing (A/322 Figure 5.4):

```
byte0: MODE(1) | Pointer_LSB(7)
MODE=0: header is 1 byte, pointer = Pointer_LSB, no optional/extension fields
MODE=1: byte1: Pointer_MSB(6) | OFI(2); pointer = Pointer_LSB | Pointer_MSB<<7 (13 bits)
  OFI=00: no optional field
  OFI=01: optional byte: EXT_TYPE(3) | EXT_LEN(5)                 (short, 0–31 bytes)
  OFI=10: optional 2 bytes: EXT_TYPE(3) | EXT_LEN_LSB(5) | EXT_LEN_MSB(8)   (long)
  OFI=11: optional 2 bytes: NUM_EXT(3) | EXT_LEN_LSB(5) | EXT_LEN_MSB(8)    (mixed; then NUM_EXT 2-byte sub-headers)
  Extension field of EXT_LEN bytes follows; EXT_TYPE 000 = 2-byte counter (+padding), 111 = all padding
```

Behaviour:

1. Validate header length ≤ packet; `pointer == 8191` means no ALP packet starts in this BBP.
2. Payload = bytes after header + extension. Bytes `[0, pointer)` complete the ALP packet left
   pending from the previous BBP of this PLP (if any; otherwise they are discarded and counted).
3. From `pointer` on, parse ALP base headers (5.4.1) to find packet lengths; emit complete ALP
   packets; a packet that runs past the payload end is kept pending (`pending_needed` bytes).
   Splitting is at byte granularity (A/322 §5.2.1), so even the ALP header can be split: keep up to
   the first 8 bytes pending and re-evaluate the length when more bytes arrive.
4. **Counter extension** (EXT_TYPE 000): track the 16-bit per-PLP counter; a jump → `discontinuity`,
   which flushes the pending ALP fragment.
5. `error` flag from the tuner (uncorrectable FEC) → flush pending and resync at the next BBP with
   a valid pointer.
6. Optional descrambling (A/322 §5.2.3; 16-bit PRBS, polynomial and init per the standard) when
   `descramble=true`.

Observed (BWI feed): every BBP uses `MODE=1`, `OFI=10` (long extension) with `EXT_TYPE=111` all-padding
extensions of varying length to fill `Kpayload`; no counter extension; pointer 8191 in 10 % of BBPs
(whole BBP continues a large ALP packet) and 0 in 2 %; split points fall inside ALP headers often
enough that a deframer without the "incomplete header → pending" rule above fails on a two-byte
header remainder. Tests must include these cases.

Output: `AlpPacket{plp_id}` to 5.4. Tests: synthetic BBPs from the `alp2bbp` tool (6.1) built from
the ALP samples with every split pattern (header split, 1-byte remainder, pointer 8191, padding only),
plus the real BBPs recovered from the STLTP capture.

### 5.4 ALP decapsulation (A/330 §5)

#### 5.4.1 Header parsing (A/330 Table 5.1–5.9)

```
byte0: packet_type(3) | PC(1) | HM or S/C(1) | length[10:8](3) ; byte1: length[7:0]
packet_type: 000 IPv4, 010 compressed IP (ROHC), 100 link-layer signaling, 110 type extension,
             111 MPEG-2 TS (different header, see 5.4.4), 001/011/101 reserved
PC=0, HM=0: 2-byte header
PC=0, HM=1: + byte2: length_MSB(5) | reserved(1) | SIF(1) | HEF(1)      length = MSB<<11 | length
PC=1, S/C=0 (segmentation): + byte2: segment_sequence_number(5) | LSI(1) | SIF(1) | HEF(1)
PC=1, S/C=1 (concatenation): + byte2: length_MSB(4) | count(3) | SIF(1)
             + (count+1) 12-bit component_length fields, 4 stuffing bits if count+1 is odd
packet_type 100: + signaling_information_hdr: signaling_type(8) signaling_type_extension(16)
             signaling_version(8) signaling_format(2) signaling_encoding(2) reserved(4)
packet_type 110: + extended_type(16)
then: if SIF: SID(8); if HEF: extension_type(8) extension_length_minus1(8) bytes[]
payload follows; payload length = `length` (total across concatenated components)
```

Requirements:

- Bounds-check every field; a header that exceeds the buffer → `Error::Truncated`, packet dropped.
- `length == 0` is accepted (Observed finding 2) and surfaced as an event; no payload processing.
- Reserved packet types are counted and skipped.
- Concatenation: the last component length is implied (`length − Σ component_length`); must be > 0.
- SID: if an SID filter is configured for the PLP, non-matching packets are dropped before any
  further processing (cheap service filtering, A/330 §5.1.3.1 and LMT `SID`).
- Header extensions are skipped and reported (`extension_type`, bytes) — types 0xF0–0xFF are user
  private.

#### 5.4.2 Segmentation reassembly (A/330 §5.1.2.2)

State per (PLP, packet_type, SID-or-none). `segment_sequence_number` must start at 0 and increase
by 1; LSI=1 completes the packet. Any gap, a new SN=0 before LSI, or a size above
`max_reassembly_bytes` (default 65535) discards the partial packet. Maximum 32 segments.

#### 5.4.3 Dispatch by packet type

| type | Action |
|------|--------|
| 000 | `IpDemux::onIpv4(plp, bytes)` |
| 010 | `RohcDecompressor::onPacket(plp, cid-context)`; output IPv4 → `IpDemux` (Phase 2) |
| 100 | `LinkSignaling::onTable(signaling_type, ext, version, format, encoding, payload)` |
| 110 | count, expose raw via `onTypeExtension(extended_type, bytes)` |
| 111 | `TsReassembler` (5.4.4) → `onTsPackets(plp, 188-byte packets)` |

#### 5.4.4 MPEG-2 TS encapsulation (A/330 §5.4)

Header: `packet_type(3)=111 | NUMTS(4) | AHF(1)` [+ `HDM(1) | DNP(7)`]. NUMTS=0 means 16.
Payload: NUMTS TS packets of 187 bytes (sync byte removed), or with HDM=1 the first packet carries
the 3-byte header and the following NUMTS−1 carry 184-byte payloads only (header fields copied,
continuity counter incremented). DNP null packets (PID 0x1FFF) are re-inserted **before** the
packets of this ALP packet (DNP=0 means 128 when HDM=0, none when HDM=1). Output is a stream
of 188-byte TS packets with sync byte restored. This is a pass-through sink; no TS demux is in scope.

#### 5.4.5 Output and events

`AlpDecap` emits `IpDatagram{plp}`, link signaling tables, TS packets, and
`AlpEvent{ZeroLength, PrivateExtension, ReassemblyDrop, Truncated, ReservedType}` with counters.

### 5.5 Link-layer signaling, LMT/RDT and ROHC (A/330 §6–7)

#### 5.5.1 LMT (A/330 Table 7.2)

```
num_PLPs_minus1(6) reserved(2)
for each PLP: PLP_ID(6) reserved(2) num_multicasts(8)
  for each: src_IP(32) dst_IP(32) src_port(16) dst_port(16) SID_flag(1) compressed_flag(1) reserved(6)
            [SID(8)] [context_id(8)]
```

- Keyed by `signaling_version`; identical versions are ignored.
- Produces the **PLP map**: `(dst_ip, dst_port[, src_ip]) → {plp_id, sid?, cid?}`. `src_ip`
  0.0.0.0 is a wildcard (Observed: LMTs with `0.0.0.0:0` sources and LMTs with full 4-tuples both occur).
- The map is advisory for routing (which PLP to decode for a flow) and mandatory for ROHC (CID →
  RDT context). The LLS flow (224.0.23.60:4937) is never in the LMT (A/330 §7.1.1).
- Exposed to the application (`ILinkInfo::plpMap()`) so a tuner that decodes a subset of PLPs
  can be reconfigured when a selected service needs another PLP.

#### 5.5.2 RDT (A/330 Table 7.4) and ROHC-U (RFC 3095)

```
PLP_ID(6) max_CID(8) adaptation_mode(2) context_config(2) reserved(6) num_context(8)
for each: context_id(8) context_profile(8) [context_length(8) static_chain/dynamic_chain bytes]
```

ROHC-U decompression (profiles 0x0001 RTP/UDP/IP, 0x0002 UDP/IP, 0x0003 ESP/IP, 0x0004 IP;
unidirectional mode only; large CID, 8-bit) with the three adaptation modes:

- Mode 1: full ROHC stream (IR/IR-DYN present); RDT carries no context — plain RFC 3095 decompressor.
- Mode 2: IR packets replaced by IR-DYN; static chain comes from the RDT.
- Mode 3: IR and IR-DYN removed; static and dynamic chains from the RDT.

The decompressor is an **in-house implementation** (decision Q4): RFC 3095 ROHC-U decompressor only
(no feedback channel), large CIDs (8-bit, A/330 §7.1.2), profiles 0x0001 RTP/UDP/IP, 0x0002 UDP/IP
and 0x0004 IP (0x0003 ESP/IP and 0x0006 TCP are rejected with a counter), packet types IR, IR-DYN,
UO-0, UO-1 (and UO-1-ID/-TS/-RTP for profile 1), UOR-2 (and -ID/-TS/-RTP), with W-LSB decoding,
CRC-3/7/8 verification, IP-ID offset handling and the RDT-supplied static/dynamic chains for
adaptation modes 2 and 3. Context repair follows RFC 3095 §5.3.2.2.3 (state machine No Context →
Static Context → Full Context; corrupted contexts fall back and wait for the next IR/IR-DYN or RDT).
Estimated 3–4 kLOC plus tests built from RFC 3095 worked examples and from a compressor written for
the tests only. It is Phase 2 (no sample uses compressed IP). Phase 1 delivers the RDT parser, the
context store and the decompressor interface.

### 5.6 IP/UDP demultiplexer and flow router

- IPv4 header validation: version 4, IHL ≥ 5, total length ≤ buffer (a longer buffer is trimmed;
  shorter → drop), header checksum verified (configurable, default on for file input, off for
  tuner input where the PHY already protects data).
- Fragmented IPv4 (MF or offset ≠ 0): reassembly per (src, dst, id, proto) with a 2 s timeout and
  a 64 kB cap. Rare in broadcast (ALP segmentation is used instead) but legal.
- UDP: length and (if non-zero) checksum validation; checksum failures are counted and the packet
  is dropped only when `strict_udp_checksum` is set (default off: PHY-protected data, and some
  encapsulators have been seen with wrong checksums).
- Non-UDP protocols are counted and dropped.
- **Flow table**: subscribers register `FlowKey{dst_ip, dst_port, src_ip?}` with a priority.
  Lookup order: exact (dst, port, src) → (dst, port) → wildcard taps. Multiple subscribers per
  flow are allowed (e.g. a ROUTE session and a diagnostic dumper).
- Built-in subscriptions: LLS (224.0.23.60:4937, always), each selected service's SLS flow, each
  session flow derived from S-TSID/MP table, application `RawFlowTap`s.
- Datagrams with no subscriber are counted per flow (`unroutedFlows()` is exposed for the analyzer
  and for the service manager's "what is on air" view).
- PLP id is carried along with each datagram so that sessions can detect PLP changes.

### 5.7 Low Level Signaling (A/331 §6)

#### 5.7.1 Table header (A/331 Table 6.1)

`LLS_table_id(8) LLS_group_id(8) group_count_minus1(8) LLS_table_version(8)` then payload. One
`LLS_table()` per UDP datagram. Payload for XML tables is gzip (RFC 1952); accept raw deflate and
uncompressed XML as fallbacks (detect by magic `1F 8B` / `<`).

| id | Table | Handling |
|----|-------|----------|
| 0x01 | SLT | parse (5.7.2) |
| 0x02 | RRT | parse `RatingRegionTables` (Annex F, `RRT-1.0` schema): up to 2 `RatingRegionTable@regionIdentifier` each with `RegionIdText`, `Dimension{dimensionIndex, dimensionLevels, dimensionGraduated}/DimensionTitle, Rating@ratingValue/RatingValueAbbrev, RatingValueString`, all texts `xml:lang` (default EN) |
| 0x03 | SystemTime | parse (`SYSTIME-1.0`): `@currentUtcOffset` (short, required), `@ptpPrepend`, `@leap59`, `@leap61`, `@utcLocalOffset` (xs:duration, required, e.g. `-PT4H`), `@dsStatus`, `@dsDayOfMonth`, `@dsHour`; feed `Clock` (5.15) |
| 0x04 | AEAT | parse (`AEAT-1.0`): `AEAT@aeaTableId`; `AEA{aeaId, issuer, audience public/restricted/private, subAudience list, aeaType alert/update/cancel, refAEAId, priority 0–4, category, wakeup}`; `Header{effective, expires}/EventCode@type, EventDesc, Location@type FIPS/SGC/polygon/circle`; `AEAText@xml:lang` (one per language); `LiveMedia{bsid list, serviceId}/ServiceName`; `Media{xml:lang, mediaDesc, mediaType EventDescAudio/AEAtextAudio/EventSymbol, url (may be relative → resolved against the AEA media channel's Content-Location space), alternateUrl, contentType, contentLength, mediaAssoc}`; expose structured and raw XML |
| 0x05 | OnscreenMessageNotification | parse (`ONSCREEN-1.0`; note the schema spells the root element `OnScreenMessageNotification`): `KeepScreenClear{bsid list, serviceId, serviceIdRange (default 0), notificationDuration (default PT1M), kscFlag (default true), version}` |
| 0x06 | CDT (CertificationData, A/360) | store raw; hand to Security (5.14) |
| 0x07 | DRCT (A/323) | store raw, expose |
| 0xFE | SignedMultiTable (Table 6.16) | `LLS_payload_count(8)`, per entry `id(8) version(8) length(16) payload`, then `signature_length(16) signature` (CMS, A/360 §5.2.2.3). Each inner payload is processed as its own table with its own version; the signature is verified by 5.14 when enabled and the result attached to the inner tables |
| 0xFF | UserDefined | store raw XML; expose `onUserDefinedTable` |
| other | reserved | store raw; expose generically |

- Table instances are keyed by `(LLS_group_id, LLS_table_id)`; a new `LLS_table_version` replaces
  the previous instance and triggers callbacks; equal versions are deduplicated (a hash of the
  payload is compared too, because some encoders bump content without bumping the version).
- `group_count_minus1` tells how many groups to expect in this PLP; the service manager uses it to
  decide when the initial scan is complete.
- Source IP and PLP of each table are recorded (different broadcasters use different source IPs).
- LLS repetition is ≤ 5 s; `expiry = 3 × 5 s` without repetition marks a group stale (events only;
  nothing is deleted without an explicit `reset`).

#### 5.7.2 SLT (A/331 §6.3, Table 6.2)

Parsed into:

```cpp
struct Slt {                                   // SLT-1.0-20211209.xsd
  std::vector<uint16_t> bsid;                  // @bsid: list of unsignedShort (required)
  std::optional<Capabilities> sltCapabilities; // sa:CapabilitiesType = string, A/332 grammar (5.11)
  std::vector<InetUrl> sltInetUrl;             // {url, urlType 1..4}
  std::vector<SltService> services;
};
struct SltService {
  uint16_t serviceId; std::optional<std::string> globalServiceId; uint8_t sltSvcSeqNum;
  bool protectedFlag=false;                    // @protected default false
  std::optional<uint16_t> majorChannelNo, minorChannelNo;   // 1..999
  uint8_t serviceCategory;                     // 1 A/V, 2 audio, 3 app, 4 ESG, 5 EA, 6 DRM data
  std::optional<std::string> shortServiceName; // ≤ 7 characters
  bool hidden=false, hideInGuide=false, broadbandAccessRequired=false;
  std::optional<bool> essential;               // no schema default
  std::optional<std::string> drmSystemId;      // single anyURI (not a list)
  enum class Configuration { Broadcast, Broadband } configuration = Configuration::Broadcast;
  std::optional<SimulcastTsid> simulcast;      // value = TSID, @simulcastMajor/MinorChannelNo
  std::optional<Capabilities> svcCapabilities;
  std::optional<BroadcastSvcSignaling> bss;    // @slsProtocol 1 ROUTE / 2 MMTP; @slsMajorProtocolVersion
                                               // default 1, @slsMinorProtocolVersion default 0;
                                               // slsDestinationIpAddress/UdpPort required; slsSourceIpAddress
  std::vector<InetUrl> svcInetUrl;
  std::vector<OtherBsid> otherBsid;            // value = list of bsid, @type 1..3
  std::vector<OtherRf> otherRf;                // @otherBsidRf @otherBsid @lat @long @elev @erp, Directional{heading, strength, haat}
  std::string rawXml;                          // element subtree for forward compatibility
};
```

Unknown attributes/elements are preserved in `rawXml`. Services are identified by `(bsid, serviceId)`.
Element order follows the schema sequence (`SimulcastTSID, SvcCapabilities, BroadcastSvcSignaling,
SvcInetUrl*, OtherBsid*, OtherRf*`), but the parser does not depend on order. The three SLT examples
in `specs/schemas/lls/` (multi-bsid, prefixed namespace `slt:`, `OtherRf` with `Directional`) are golden vectors.

### 5.8 ROUTE / LCT object recovery (A/331 Annex A, RFC 5651, RFC 5775)

#### 5.8.1 LCT header (RFC 5651 §5.1, A/331 A.3.6)

```
byte0: V(4)=1 | C(2) | PSI(2) ; byte1: S(1) | O(2) | H(1) | reserved(2) | A(1) | B(1)
byte2: HDR_LEN (32-bit words) ; byte3: Codepoint
CCI: 32*(C+1) bits ; TSI: 32*S + 16*H bits ; TOI: 32*O + 16*H bits
extensions until HDR_LEN*4: HET(8) [HEL(8) if HET<128] ...
FEC Payload ID: 32-bit start_offset (source flows, A.3.5.1) or SBN(8)+ESI(24) (repair flows)
```

ATSC constraints (V=1, C=0, PSI=10, S=1, O=01, H=0) are **checked but not enforced**: packets with
other legal RFC 5651 combinations are still parsed (A/331 says "shall", encoders have been seen
with PSI=00). V≠1 → drop. A packet whose length equals the LCT header (no FEC payload ID) is a
data-less packet (A.3.5); it carries only flags (B: close object, A: close session).

Extension headers (IANA registry):

| HET | Name | Format | Use |
|-----|------|--------|-----|
| 0 | EXT_NOP | — | skip |
| 1 | EXT_AUTH | var | keep raw |
| 2 | EXT_TIME | RFC 5651 §5.2.1 (use-bits: SCT-High/SCT-Low/ERT/SLC) | SCT for MDE mode |
| 64 | EXT_FTI | RFC 5775 §4.3: `HET HEL TransferLength(48)` + scheme FTI. For Compact No-Code (RFC 5445): `Reserved(16) EncodingSymbolLength(16) MaxSourceBlockLength(32)`; for RaptorQ (RFC 6330 §3.3): `F(40) reserved(8) T(16) Z(12) N(12) Al(8)` → object length, FEC parameters |
| 66 | EXT_ROUTE_PRESENTATION_TIME | `HET HEL reserved(16)` + NTP64 | MDE presentation time |
| 67 | EXT_TOL (48-bit) | `HET HEL reserved(16)` + length(48)... (A/331 Fig. A.3.7) | transfer length |
| 192 | EXT_FDT | FLUTE V(4) FDT-Instance-ID(20) | FDT instance id |
| 193 | EXT_CENC | content encoding | gzip indication |
| 194 | EXT_TOL (24-bit) | `HET` + length(24) | transfer length |
| other | — | keep raw, count |

The FEC scheme is **not** derived from the codepoint. It is: RaptorQ if the flow is declared a
RepairFlow (S-TSID) or EXT_FTI announces a non-zero FEC Encoding ID; otherwise Compact No-Code with
a 32-bit `start_offset` (A.3.5.1). `SrcFlow.Payload@srcFecPayloadId` (0 = direct offset) overrides.

#### 5.8.2 Session and channel model

```
RouteSession  = (src_ip?, dst_ip, dst_port)         ← one UDP flow
  LctChannel  = TSI                                 ← SrcFlow and/or RepairFlow, EFDT, Payload rules
    DeliveryObject = TOI                            ← byte map + metadata
```

Sessions are created (a) for the SLS flow from the SLT (`TSI 0` channel implied), (b) for every
`RS/LS` in the S-TSID of a selected service, (c) explicitly by the application (diagnostics).
A channel with no S-TSID description (unknown TSI on a known session) is still assembled in
"opportunistic" mode (needed for the ESG sample where TSI 1/2 are only described once the SLS is
decoded) but delivered only if the application opted in to `deliverUndescribedObjects`.

#### 5.8.3 Object recovery (A/331 A.3.10.2)

For every packet of (TSI, TOI):

1. Allocate or look up the object. Expected length `T` comes, in priority order, from EXT_FTI,
   EXT_TOL, `EFDT.File@Transfer-Length`, `EFDT.File@Content-Length` (only when no
   Content-Encoding), `EFDT@maxTransportSize` (upper bound only). Without any: grow dynamically
   up to `max_object_bytes` (default 64 MiB, per-channel override via `SrcFlow@minBuffSize`).
2. Place `payload` at `start_offset` (reject if `start_offset + len > T` when `T` is known).
   Maintain a received-range set (interval list; not a per-byte bitmap for large objects).
3. Completion is declared by the first of:
   - ranges cover `[0, T)` with `T` from in-band extension or EFDT;
   - `B` (close object) flag seen and ranges are contiguous from 0 (then `T` = highest byte);
   - EFDT `Content-MD5` matches (checked when present; mismatch → `ObjectError::ChecksumMismatch`);
   - carousel rotation: a packet with `start_offset == 0` for the same TOI arrives again while
     the object is contiguous → treat the previous instance as complete (used for TSI 0 and NRT
     carousels that use neither TOL nor B).
4. Timeout: an object with no new packet for `object_timeout` (default 10 s streaming, 60 s NRT)
   is reported incomplete (`ObjectError::Timeout` with missing ranges) and freed. If a RepairFlow
   is active, FEC decoding is attempted first (Phase 3).
5. Duplicate packets (same offset, same bytes) are counted, not errors.
6. Partial delivery ("MDE mode", A.3.7): when the channel is real-time (`SrcFlow@rt=true`) and the
   application enabled `onObjectFragment`, contiguous prefixes are delivered as they arrive with
   `EXT_ROUTE_PRESENTATION_TIME`/SCT timing. Out-of-order packets are buffered until contiguous.

#### 5.8.4 Delivery modes and codepoints (A/331 Table A.3.2, A.3.6)

| CP | Meaning | formatId / frag / order | Receiver action |
|----|---------|------------------------|-----------------|
| 1 | NRT file mode | 1 / 0 / true | file with EFDT metadata |
| 2 | NRT entity mode | 2 / 0 / true | parse HTTP entity header (RFC 7231) → metadata; body = file |
| 3 | NRT unsigned package | 3 | `multipart/related` → unpack each part as a file |
| 4 | NRT signed package | 4 | `multipart/signed` → verify (5.14), then unpack |
| 5/6/7 | Init segment (new timeline / continued / redundant) | 1 / 0 | `InitSegment` events with timeline flag |
| 8 | Media segment, file mode | 1 / 1 | `MediaSegment` |
| 9 | Media segment, entity mode | 2 / 1 | entity header then `MediaSegment` |
| 10–127 | reserved | — | assemble as opaque file, flag `unknownCodepoint` |
| 128–255 | per `SrcFlow.Payload[@codePoint]` | from S-TSID | as declared; before the S-TSID is known assume formatId 1 |

`frag=1` (sample-based fragmentation) only affects MDE delivery; object assembly is identical.

#### 5.8.5 EFDT and file metadata (A/331 A.3.3.2, RFC 6726)

- Sources: (a) `SrcFlow/EFDT/FDT-Instance` in the S-TSID; (b) an object on TOI 0 of the same
  channel, which is either a bare `fdt:FDT-Instance` (Sinclair) or an `EFDT` element in the S-TSID
  namespace wrapping the `FDT-Instance` (UniSoft) — both forms are accepted; (a) and (b) are
  mutually exclusive by spec but both are tolerated, the newer `@efdtVersion` wins.
- On the SLS channel, the TOI 0 EFDT describes the SLS object itself (`Content-Location` "SLS" or
  "sls", `Content-Type` `application/mbms-envelope+xml` or `multipart/signed`, `Content-Length`,
  `Transfer-Length`); the SLS TOI still follows Annex C (observed 0x700DA/0x700DB = USBD+S-TSID+MPD,
  versions 218/219; 0x470001 = +HELD; 0x30001 = USBD+S-TSID).
- `FDT-Instance` attributes: `Expires`, `@efdtVersion`, `@maxExpiresDelta`, `@maxTransportSize`,
  `@appContextIdList`, `@fileTemplate`, `@filterCodes`, `@maxCacheMemory`, `@order`, default
  `Content-Type`/`Content-Encoding`/FEC-OTI attributes.
- `File` attributes: `TOI`, `Content-Location`, `Content-Length`, `Transfer-Length`, `Content-Type`,
  `Content-Encoding`, `Content-MD5`, `FEC-OTI-*`, ATSC `@appContextIdList`, `@filterCodes`.
- `@fileTemplate` (`ATSC-FDT-1.0` pattern: literal text, `$TOI$` or `$TOI%0<n>d$` zero-padded width,
  `$$` for a literal dollar sign; example `path/my$$Video$TOI%05d$.m$$ps`) generates
  `Content-Location` for TOIs without a `File` entry (common for media segments).
- ATSC attributes live in the `afdt:` namespace on `FDT-Instance` (`efdtVersion`, `maxExpiresDelta`,
  `maxTransportSize`, `fileTemplate`, `appContextIdList`, `filterCodes`) and on `File`
  (`appContextIdList`, `filterCodes`). 3GPP FLUTE extension children (`mbms2007:Cache-Control`
  with `no-cache`/`max-stale`/`Expires`, `mbms2012:Alternate-Content-Location-1/2`, `mbms2012:Base-URL-1/2`)
  are parsed into `cacheControl` / `alternateLocations` / `baseUrls` and otherwise preserved raw.
- Expiry: object metadata older than `Expires`/`maxExpiresDelta` is dropped; objects already
  assembled are not deleted.
- `Content-Encoding: gzip` → object bytes are inflated before delivery; both compressed and
  inflated lengths are reported.

#### 5.8.6 Repair protocol (A/331 A.4) — Phase 3

Parse `RepairFlow` (Table A.4.1); repair packets arrive on their own TSI with an SBN+ESI FEC
payload ID (A.3.5.2); RaptorQ (RFC 6330) decoding of FEC super-objects. Interface
`IFecDecoder` is defined in Phase 1 so that a RaptorQ library can be plugged in later.

### 5.9 ROUTE Service Layer Signaling (A/331 §7.1)

#### 5.9.1 Acquisition

1. From the SLT `BroadcastSvcSignaling` open `RouteSession(slsSourceIpAddress, slsDestinationIpAddress,
   slsDestinationUdpPort)` and channel TSI 0.
2. TOI filtering (Annex C): `TOI = G<<31 | R<<24 | D<<23 | H<<22 | A<<19 | M<<18 | S<<17 | U<<16 | version`.
   G=1 → the package is gzip-compressed. Use the bits to prioritise (USBD+S-TSID first), the
   `version` byte to detect updates. Observed TOI 0x300DD = USBD+S-TSID, version 221.
3. TOI 0 on the SLS channel is an EFDT (Observed finding 9). If present, its `File` entries define
   `Content-Type` of the SLS object (`multipart/related` or `multipart/signed`) and `Content-Length`.
4. Assemble the object (5.8), inflate if G=1 or `Content-Encoding: gzip`.

#### 5.9.2 Package parsing

- `multipart/related` (RFC 2387): split on the boundary, parse part headers (`Content-Type`,
  `Content-Location`, `Content-ID`, `Content-Encoding`). The first part is the MBMS
  `metadataEnvelope` (`application/mbms-envelope+xml`) listing `item` elements
  (`metadataURI`, `version`, `contentType`, `validFrom`, `validUntil`, ATSC `@nextURL`, `@availAt`).
- `multipart/signed` (RFC 1847, A/360): first part = the `multipart/related` above, second part =
  `application/pkcs7-signature`. Verification is delegated to 5.14; the SLS is used regardless,
  with `signatureStatus` attached (Unverified / Valid / Invalid / NoCerts).
- Fragment types by part `Content-Type`:

| Content-Type | Fragment | Parser |
|--------------|----------|--------|
| `application/route-usd+xml` (on air), `application/mbms-user-service-description+xml` | USBD (Table 7.3, `ROUTEUSD-1.0`): root `BundleDescriptionROUTE/UserServiceDescription{serviceId, serviceStatus}` with `Name@xml:lang*`, `ServiceLanguage*`, `DeliveryMethod*/BroadcastAppService|UnicastAppService/BasePattern+` | typed |
| `application/route-s-tsid+xml` (on air), `application/s-tsid+xml` | S-TSID (Table 7.4, A.3.1) | typed |
| `application/dash+xml` | MPD (DASH-IF, profile `urn:mpeg:dash:profile:isoff-broadcast:2015`) | light-typed: `MPD{type, availabilityStartTime, publishTime, minimumUpdatePeriod, BaseURL}`, Periods, AdaptationSets (`contentType, lang, mimeType, Role, Accessibility, Rating, SupplementalProperty` incl. `dash-atsc-cgcompatibility` and `dash-atsc-closedcaption`, `RandomAccess@interval`), Representations (`id, codecs, bandwidth, width/height, frameRate, AudioChannelConfiguration`), `SegmentTemplate{initialization, media, timescale, $Time$/$Number$/$RepresentationID$}` with `SegmentTimeline/S{t,d,r}`, `EventStream`/`InbandEventStream` + raw XML |
| `application/atsc-held+xml` | HELD (Table 7.6) | typed |
| `application/atsc-dwd+xml` | DWD (Table 7.7) | typed |
| `application/mbms-associated-procedure-description+xml` | APD (Table 7.5) | typed |
| `application/atsc-rsat+xml` | RSAT (A/200) | raw + id list |
| other | — | raw, delivered as `UnknownSlsFragment` |

Media type parameters (`; charset=utf-8`) are stripped before matching. Parts are matched to
envelope `item@metadataURI` by `Content-Location` (observed names: `usbd.rusd`, `stsid.sls`,
`mpd.mpd`, `usbd.xml`, `stsid.xml`, `media.mpd`, `held.xml`, `envelope.xml`); an item without a
matching part is reported, not fatal. Media types are matched case-insensitively and with tolerance
for vendor variants; when the media type is generic (`text/xml`, `application/xml`) the fragment is identified by root element name:
`BundleDescriptionROUTE`, `BundleDescriptionMMT`, `S-TSID`, `MPD`, `HELD`, `DWD`,
`AssociatedProcedureDescription`, `RSAT`, `FDT-Instance`, `metadataEnvelope`.

Segment-to-representation mapping uses `S-TSID/.../MediaInfo@repId` first; when absent, the object's
`Content-Location` is matched against each Representation's resolved `SegmentTemplate@media`
(`$Time$` from `SegmentTimeline`, `$Number$`), as in the Triveni sample
(`video-$Time$.mp4v`, `audio1-$Time$.mp4a`, `data-$Time$.mp4s`). Init segments match `@initialization`.

#### 5.9.3 Session setup from the S-TSID

For each `RS` (defaults to the SLS session's src/dst/port when absent) and each `LS@tsi`:
subscribe the flow, create the channel with its `SrcFlow` (`@rt`, `@minBuffSize`, `EFDT`,
`ContentInfo/MediaInfo{@repId,@contentType,@lang,@startup}`, `Payload[]{@codePoint,@formatId,@frag,
@order,@srcFecPayloadId}`) and `RepairFlow`. Map `MediaInfo@repId` to the MPD Representation to
derive codec, bandwidth, mime type, role; deliver `ChannelAdded/Changed/Removed` events on
version changes (diffing the previous S-TSID; channels not present anymore are closed after
`channel_linger` = 2 s so in-flight objects complete).

Schema-derived rules (`S-TSID-1.0-20220708.xsd`):

- `RS@sIpAddr/@dIpAddr/@dPort` optional (default to the SLS session); `LS@tsi` required,
  `@bw`, `@startTime`, `@endTime` optional (channels outside their window are not opened).
- `SrcFlow@rt` default false, `@minBuffSize` in kB. `SrcFlow/Payload` is **mandatory and
  repeatable**: `@codePoint` (≥128, default 128), `@formatId` (1–4, required), `@frag` (0–2, default 0),
  `@order` (default true), `@srcFecPayloadId` (only 0 is allowed), `@fecParams` (12-byte hex FEC OTI).
  Codepoints below 128 are never declared; they use Table A.3.6 semantics.
- `ContentInfo` is a choice: `MediaInfo{@repId required, @contentType audio|video|subtitles,
  @xml:lang, @startup}` with `ContentRating{@schemeIdUri default tag:atsc.org,2016:carating:1, @value}`
  (value in the content-advisory string grammar, 5.11) — or `AEAMedia/AEAId*`.
- `RepairFlow/FECParameters{@maximumDelay ms (default 0), @overhead ‰ (≤1000), @minBuffSize,
  @fecOTI 12-byte hex (required)}/ProtectedObject{@tsi, @sessionDescription}/SourceTOI{@x default 1,
  @y default 0}`. `@sessionDescription` is either a URL or a comma-separated `key=value` list
  (`sIpaddr, dIpaddr, dport, bw, startTime, endTime, srcFecPayloadId`); both forms appear in the
  five `RepairFlow-Example*.xml` files and both must parse (whitespace/newlines inside the list are tolerated).
  Several `ProtectedObject`s may reference the same TSI with different `SourceTOI` selectors (`x`, `y`
  select TOIs with `TOI mod x == y`).

Observed S-TSID practice to support: `RS` with no attributes at all (defaults to the SLS session);
init segments as an explicit `File` (TOI 1 or TOI 2100000000) plus `fileTemplate` for media
segments, with TOI equal to the MPD `$Number$`; `MediaInfo@contentType` values `video`, `audio`,
`subtitles` **and** the non-schema `text`; `repId` strings such as `Video1_1`, `a02_2`, `d4_4`, `0`;
ESG channels with per-file `Content-Encoding="gzip"` and `Content-Type`
`application/vnd.oma.bcast.sgdd+xml` / `application/vnd.oma.bcast.sgdu` / PNG; NRT package channels
(`Content-Type="multipart/related"`, `afdt:appContextIdList`, CP 3 or CP 130, `maxTransportSize` up
to 4.5 MB) on five-digit TSIs; `LS@bw` up to 8.5 Mbit/s.

Object typing for delivery: `Content-Type`/`ContentInfo` + codepoint decide
`InitSegment | MediaSegment | NrtFile | SlsFragment | EsgFragment | EaMedia | AppFile`.

### 5.10 MMTP (ISO/IEC 23008-1:2017, A/331 §7.2 and §8.1.2)

#### 5.10.1 Scope

MMTP sessions are opened per SLT service with `slsProtocol=2` on `(slsDestinationIpAddress,
slsDestinationUdpPort)` and additionally for every `MMT_general_location_info` found in the MP
table (asset locations on other UDP flows). Both MPU mode (type 0x00) and signaling mode
(type 0x02) are supported; GFD mode (0x01) and repair symbols (0x03) are counted and ignored.

#### 5.10.2 Packet header

Observed on two independent encoders (UniSoft 2019 and the Sinclair/WNUV service in the 2023 STLTP
feed):

```
UniSoft  : 40 00 | 00 14 | 24 32 28 f6 | 00 15 4d 22 | 80 00 | 05 14 2c 9b 00 00 03 e3 ...
Sinclair : 62 00 | 00 64 | ca 99 38 63 | 32 9b 67 3c | 51 f9 1d 4b | a4 00 | 05 ac 2c 1e 63 9a cc 64 ...
Sinclair : 61 02 | 00 00 | bc 1c 4b ff | 02 32 68 26 | 2b 10 ea 7b | 9b ff | 3c 00 | 81 00 01 00 00 02 89 ...
           b0 b1   pid     timestamp     packet_seq   [packet_cnt]  QoS     MPU/signaling payload header
```

Parsing rules (A/331 §8.1.2.1.3 requires `version='01'`):

- byte0: `version(2) packet_counter_flag(1) FEC_type(2) f2(1) f1(1) f0(1)`; byte1: `reserved/flags(2)
  type(6)` (types 0x00 MPU, 0x01 GFD, 0x02 signaling, 0x03 repair). Then `packet_id(16)
  timestamp(32, NTP short) packet_sequence_number(32) [packet_counter(32) if C]`.
- **For version 01 a 16-bit QoS/flow word always follows**: `reliability_flag(1) type_of_bitrate(2)
  delay_sensitivity(3) transmission_priority(3) flow_label(7)`. Both encoders emit it with every
  combination of f2..f0 seen (`000`, `010`, `001`), and the MPU payload `length` equals the remaining
  bytes only when it is consumed. It is not gated by a flag.
- f2..f0: f2 is treated as `extension_flag` (never set in the corpus; when set, parse
  `header_extension_type(16) header_extension_length(16) data`). f1 and f0 are exposed as raw flags
  (`flagA`, `flagB`): one encoder sets f1 on every MPU packet and f0 on every signaling packet, the
  other sets neither, so neither can be relied upon as a RAP indicator. RAP is derived from the MPU
  payload instead (FT 0/1 packets and `moof` boundaries).
- For version 00 (ISO 1st edition) the classic layout applies: byte0 bits 2..0 = `reserved,
  extension_flag, RAP_flag`, no QoS word.
- Self-check: after the header, the MPU-mode `length` field must equal the remaining UDP payload
  length; a mismatch is counted (`mmtpHeaderMismatch`) and the packet dropped.
- Per `packet_id`: sequence-number continuity check (loss → mark current MPU damaged); the
  `packet_counter` is global across packet ids and is checked when present.

#### 5.10.3 MPU mode payload (ISO 23008-1 §9.3.2)

`length(16) FT(4) T(1) f_i(2) A(1) frag_counter(8) MPU_sequence_number(32)` then, if `A=1`, repeated
`DU_length(16) DU_header DU`; else one DU.

- FT 0 (MPU metadata): `ftyp`, `mmpu`, `moov`, … boxes — the init segment of the MPU.
- FT 1 (movie fragment metadata): `moof` + `mdat` header.
- FT 2 (MFU): DU header for timed media = `movie_fragment_sequence_number(32) sample_number(32)
  offset(32) priority(8) dep_counter(8)`; non-timed = `item_id(32)`.
- `f_i`: 00 complete, 01 first fragment, 10 middle, 11 last — reassemble per (packet_id, MPU seq,
  FT) using `frag_counter` for ordering; drop on gaps.
- Output per `packet_id`: (a) `MpuInitSegment` when FT 0 completes; (b) `MpuFragment` = `moof` +
  `mdat` reconstructed by placing MFU sample data at `offset` in the `mdat` (gaps → zero-fill +
  `damaged=true`), emitted when the next MPU/fragment starts or on timeout; (c) optional raw MFU
  stream for low-latency consumers (`onMfu`).
- Presentation time: `MPU_timestamp_descriptor` (from the MP table) gives the NTP time of the first
  sample of each MPU sequence number; sample times are added from `trun`/`tfdt`.

Implementation note: `mmtp/mpu.hpp` — `MpuAssembler`, one per media packet_id, created by
`Session` when `setMpuAssembly(true)` (the engine enables it for selected services). DU reassembly per
(packet_id, MPU sequence, FT) with packet-sequence continuity and the repeated MFU DU header stripped from
middle/last fragments; aggregated DUs (`DU_length` prefixed) and non-timed MFUs accepted (the latter
counted and ignored). FT=0 → `media::cleanInitSegment` (hint track, `mmpu` and every other top-level box
dropped, `ftyp` rewritten) → `MpuInitSegment{bytes, info, media track with codecs, hint_track_id,
hint_sample_size}`, emitted only when the DU without its `mmpu` box changed. FT=2 → the 34-byte
`MMTHSample` prefix is detected (`muli` box at bytes 23–33 and `samplenumber` == the DU header's) and
stripped; samples are kept by sample number. FT=1 → `media::parseMoof`. A fragment is complete when the
init, the moof and samples 1..`trun.sample_count` are present; emission is strictly in MPU order
(`drain()`): when a newer MPU completes, an older incomplete one is resolved at once (zero-filled and
`damaged` when it has its moof and sample 1, dropped otherwise, `discontinuity` on the next fragment), the
same happens beyond `max_pending_mpus` (2) / `max_pending_bytes`, and DUs of an already resolved MPU are
ignored. `media::buildFragment` removes the hint `traf`, repairs `data_offset`, rebuilds the mdat from the
media samples only and prepends `styp`. Timeline per asset: `TimelineMode::Keep` when the first tfdt is
non-zero and later ones increase (UniSoft); `Rewrite` when tfdt is 0 or stops increasing (Sinclair):
tfdt' = round((pt − anchor) × timescale) with pt from the descriptor and the anchor = the first
descriptor seen on the service (`Session::anchorNtp()`), or previous tfdt' + duration when the MPU has no
descriptor. `timestampOffsetSeconds()` places a Keep-mode asset on the same timeline
((pt_first − anchor) − tfdt_first/timescale); it is frozen at the first emitted fragment and never changes
(a later Keep→Rewrite switch rewrites tfdt relative to it), fragments emitted before any descriptor sit on
a local timeline that the first descriptor continues rather than restarts, and a backward MPU-sequence jump
beyond 65 536 is treated as an encoder restart (state reset, `restart` flag on the next fragment, which
makes the engine bump its media generation). Hard bounds (spec 1.3, found by the review's fuzzing): a DU
under reassembly ≤ 16 MiB / 1024 fragments, sample numbers ≤ 65 536 and ≤ the trun count, a moof is
refused above 65 536 samples or 256 MiB of declared sizes, 64 B are charged per stored sample so
header-only MFUs hit `max_pending_bytes`, the byte cap resolves the oldest MPU even when it is the only
one, the zero-fill never exceeds `max_pending_bytes` (and `media::buildFragment` never synthesises more
than the announced mdat or 64 MiB; `parseTrun` clamps `sample_count` at 65 536), a present sample whose
size differs from the trun is zero-filled and counted, and DUs of an already resolved MPU are ignored.
Verified on both captures (unit tests on the on-air DUs, randomized robustness tests under ASan/UBSan,
corpus tests, ffprobe-decoded reconstructions, Firefox MSE playback). Not implemented: AL-FEC (0x03),
GFD (0x01), FT=3 hint-sample DUs, the HRBM de-jitter model (the player buffers by itself), several movie
fragments per MPU (the DU-header `movie_fragment_sequence_number` is read but one moof per MPU is assumed).

#### 5.10.4 Signaling mode payload (ISO 23008-1 §9.3.3, §10)

`f_i(2) reserved(4) H(1) A(1) frag_counter(8)` then `[MSG_length(16 or 32 by H)] message`;
aggregation and fragmentation handled like MPU mode. Messages:

| message_id | Message | Action |
|------------|---------|--------|
| 0x0000 | PA (Package Access) | list of tables; drives MPT/MPI parsing |
| 0x0010–0x001F | MPI | MPI table (presentation information) raw + MP table if embedded |
| 0x0011–0x0020 | MPT | **MP table**. Complete table: `table_id` 0x11 (UniSoft, message 0x0011, with `MMT_package_id` e.g. `usc03m-1`) **or** 0x20 (Sinclair, message 0x0020) — accept both; subset tables 0x12–0x1F (messages 0x0012–0x001F) carried on the asset's own `packet_id`, one asset each, updated with `MPU_timestamp_descriptor` (tag 0x0001: `mpu_sequence_number`, NTP64) every MPU. Asset loop: `identifier_type` 0 → `asset_id_scheme(32) asset_id_length(32) asset_id` (16-byte binary ids or ASCII such as `videoasset01`), `asset_type` 4CC (`hev1`, `ac-4`), `default_asset_flag`, `asset_clock_relation_flag` → `asset_clock_relation_id`, `asset_timescale_flag`, `asset_timescale` (90000 observed), `location_count` + `MMT_general_location_info` (type 0 = same flow + packet_id; type 1 = IPv4 src/dst/port + packet_id), `asset_descriptors_length` + descriptors |
| 0x0200 | CRI | clock relation (optional) |
| 0x0201 | DCI | device capability (ignored) |
| 0x0204 | HRBM, 0x020A HRBM removal | buffer model (expose values); both observed on every asset packet_id |
| 0x8100 | `mmt_atsc3_message` (A/331 Table 7.9) | `service_id(16) content_type(16) content_version(8)
  compression(8: 1 none, 2 gzip) URI_length(8) URI content_length(32) content` → dispatch by
  content type: 0x0001 USBD(MMT) (`MMTUSD-1.0`: root `BundleDescriptionMMT/UserServiceDescription{serviceId, serviceStatus}` with `Name*`, `ServiceLanguage*`, `ContentAdvisoryRating*` (sa:CARatingType), `OtherRatings*`, `Channel{serviceGenre, serviceIcon required}/ServiceDescription{serviceDescrText, serviceDescrLang}`, `MPUComponent{mmtPackageId, contentIdSchemeIdUri, contentIdValue, nextMMTPackageId, nextContentIdSchemeIdUri, nextContentIdValue}`, `ROUTEComponent{sTSIDUri, apdUri, sTSIDDestinationIpAddress, sTSIDDestinationUdpPort, sTSIDSourceIpAddress, major/minor version}` → opens a ROUTE session for locally-cached content, `BroadbandComponent{fullMPDUri}/BroadbandComponentInfo{repId, complementaryAssetId, dependentAssetId, simulcastAssetId}`, `ComponentInfo{componentType, componentRole, componentProtectedFlag, componentId, componentName}`), 0x0002 MPD, 0x0003 HELD, 0x0004 AEI (A/337), 0x0005 Video
  Stream Properties, 0x0006 Staggercast, 0x0007 Inband Event Descriptor, 0x0008 Caption Asset,
  0x0009 Audio Stream Properties, 0x000A DWD, 0x000B RSAT, 0x000C Security Properties; others raw |
| other | — | raw, counted |

`mmt_atsc3_message` contents observed: USBD (gzip, with and without `URI`), HELD (gzip), Video
Stream Properties, Caption Asset and Audio Stream Properties descriptors (binary, keyed by the
16-byte `asset_id` that also appears in the MP table), no MPD (broadcast-only service). Signaling
messages were never aggregated or fragmented in the corpus but both must be supported.
Message versions are tracked per (packet_id, message_id, content_type, URI); duplicates dropped.
`signed_mmt_message()` (A/331 Table 7.39) is unwrapped and verification delegated to 5.14.

#### 5.10.5 Timing

Packet timestamps and MPU timestamps are NTP short/64 formats. The session converts to the
stack clock (5.15) and exposes `presentationTime` per MPU fragment. AL-FEC (ISO 23008-1 Annex C)
is Phase 3.

### 5.11 Service manager

Responsibilities:

1. **Discovery.** Maintain `ServiceTable = {(bsid, serviceId) → ServiceEntry}` from all SLTs of all
   LLS groups; merge `OtherBsid`/`essential` information; emit `onServiceListChanged` with diffs.
   A service disappears only when its SLT version drops it or its LLS group goes stale.
2. **Selection.** `selectService(bsid, serviceId, Options)` → `ServiceHandle`. Multiple
   simultaneous selections are allowed (PiP, background ESG/EA, recording). Selecting binds the
   SLS session (ROUTE or MMTP), waits for SLS, sets up component sessions, and streams events.
   `Options` include component filters (`video|audio|subtitle|app|nrt`), language preferences,
   `deliverUndescribedObjects`, fragment delivery, and a PLP policy callback (tuner may need
   reconfiguration when the S-TSID/LMT shows a component on a PLP that is not being decoded).
3. **Always-on consumers** independent of selection: LLS (SLT/STT/AEAT/RRT/OMN) → application;
   ESG services (`serviceCategory=4`) and EA services (`=5`) can be auto-selected by policy.
4. **ESG (A/332).** ESG service objects are OMA BCAST SGDU/SGDD files delivered as NRT (CP 1/3).
   The manager delivers `EsgFragment{type: Service|Schedule|Content|..., xml, version, validity}`;
   a `SgduParser` splits SGDU containers into fragments. The OMA data model (Service, Schedule and
   Content fragments with the A/332 extensions, programme index) is the core's `esg` layer (5.17.5).
   Implementation note: `service/esg.hpp` holds `parseSgdu` (layout per the 3.2 observation,
   non-zero `extension_offset` tolerated, non-XML encodings and bad offsets skipped and counted),
   `parseSgdd` and `parseEsgFragmentHeader` (type from the root element, `id`, `version`, `validFrom/To`).
   The engine opens the NRT sessions of every serviceCategory-4 ROUTE service by policy
   (`OpenOptions::auto_open_esg`, default on) without marking it selected, classifies its objects by EFDT
   `Content-Type` (falling back to the root element, file name or binary sniffing, and inflating undeclared
   gzip), keeps the latest version of every `(type, id)` fragment (carousel repeats with identical XML are
   counted as duplicates and do not bump `Status::esg_version`), the SGDD descriptors and a bounded cache
   (48 MiB) of the service's other NRT files for icon resolution, and exposes `esg()`, `esgVersion()` and
   `esgFile(name)` snapshots.
5. **EA.** AEAT tables plus `AEAMedia`-tagged channels (S-TSID `SrcFlow/ContentInfo/AEAMedia`) →
   `onEmergencyAlert`, `onEaMediaFile`. Wake-up bits are a tuner concern (A/321) — exposed from L1Info.
6. **App-based services (A/344 hooks).** HELD (`HELD-1.0`) → `onHeld` with one entry per
   `HTMLEntryPackage{appContextId (required), appId, requiredCapabilities, appRendering,
   clearAppContextCacheDate, bcastEntryPackageUrl, bcastEntryPageUrl, bbandEntryPageUrl, validFrom,
   validUntil, coupledServices list, lctTSIRef list, default}`; the manager evaluates validity windows
   and capabilities and marks the currently applicable package. DWD (`DWD-1.0`) →
   `DistributionWindow{contentLabel, startTime, endTime, lctTSIRef list}/AppContextId@dwFilterCode list`
   drives when the referenced LCT channels are opened for NRT delivery. NRT files tagged with
   `appContextIdList`/`filterCodes` are delivered with those values so an application cache can
   store them (the HTML5 runtime itself is out of scope). Implemented (5.18.4): `ServiceInfo::apps`
   (one `AppInfo` per HTMLEntryPackage, from the ROUTE HELD or the MMT 0x0003 message, with the cache state),
   the per-service Application Context Cache behind `Engine::apps()` / `appFile()`, and the ROUTE session
   named by the MMT USBD `ROUTEComponent`; validity windows and `requiredCapabilities` are reported, not
   enforced, and DWD/filter codes are not applied.
7. **Updates.** SLT version → re-evaluate selections (SLS address change → rebind). SLS version →
   diff S-TSID / MP table and adjust sessions. LMT change → re-evaluate PLP policy.
8. **Capabilities and ratings.** `CapabilitiesEvaluator` parses the A/332 capability expression
   grammar used by `SLT/SLTCapabilities`, `Service/SvcCapabilities`, `HELD@requiredCapabilities` and
   the User-Agent string: postfix notation of 4-hex-digit capability codes and `ubyte=string`
   string-codes combined with `&` / `|` (e.g. `050E 058E |`, `50B 512 &`). The application
   supplies its capability set; the manager flags services/apps it cannot present
   (`ServiceEntry::presentable`). `ContentAdvisoryParser` parses the content-advisory string grammar
   (`ContentRating-20211208.abnf`: `[region],'description',{dim 'level'}...`, e.g.
   `1,'TV-PG D-L-S-V',{0 'TV-PG'}{1 'D'}{2 'L'}{3 'S'}{4 'V'}`) and the structured `sa:CARatingType`
   form (`RegionIdentifier, RatingDescription, RatedDimensions, RatingDimVal{RatingDimension,
   RatingValueString}`) into one `ContentRating` type, resolved against the RRT for display strings.
9. **Reset.** `reset(Reason)` on retune; everything above the source is cleared; application gets
   `onSessionReset`.
10. **Statistics** per service and per session (bytes, packets, objects complete/incomplete/timed
   out, segment latency, PLP), snapshot on demand and periodic (`stats_interval`, default 1 s).

### 5.12 Application API

Two complementary interfaces: event callbacks (push) and query objects (pull). Both are plain C++17
(no exceptions across the boundary, no RTTI requirement, `std::string_view`/`std::optional`/
`std::variant`; a small `ByteView` type replaces `std::span`).

```cpp
namespace atsc3 {

class Stack {
public:
    explicit Stack(const Config&);
    void attach(std::shared_ptr<service::ITuner>);              // an RF tuner, the pcap virtual tuner or UdpSource (today: Engine::open)
    void setEventSink(IEventSink*);                             // may be a DispatchQueue adapter
    void run();                                                 // pipeline loop (blocking) …
    void start(); void stop();                                  // … or threaded
    ServiceHandle selectService(ServiceKey, const SelectOptions& = {});
    void deselect(ServiceHandle);
    std::vector<ServiceEntry> services() const;                 // snapshot
    const LinkInfo& link() const;                               // PLP map, LMT/RDT, L1 info
    FlowTap tapFlow(FlowKey, IFlowSink*);                       // raw UDP payloads
    Stats stats() const; void reset(ResetReason);
};

class IEventSink {                                              // all optional, default no-op
public:
    // stream-wide
    virtual void onServiceListChanged(const ServiceListDiff&) {}
    virtual void onSystemTime(const SystemTime&) {}
    virtual void onRatingRegionTable(const Rrt&) {}
    virtual void onEmergencyAlert(const Aeat&) {}
    virtual void onOnscreenMessage(const Omn&) {}
    virtual void onLlsTable(const LlsTableInfo&, ByteView xml) {}   // every table incl. CDT/UserDefined/unknown
    virtual void onLinkInfoChanged(const LinkInfo&) {}
    virtual void onSessionReset(ResetReason) {}
    // per selected service (handle identifies it)
    virtual void onSlsFragment(ServiceHandle, const SlsFragment&) {}      // USBD, S-TSID, MPD, HELD, DWD, APD, RSAT, raw
    virtual void onComponentsChanged(ServiceHandle, const ComponentDiff&) {}
    virtual void onInitSegment(ServiceHandle, const InitSegment&) {}
    virtual void onMediaSegment(ServiceHandle, const MediaSegment&) {}
    virtual void onObjectFragment(ServiceHandle, const ObjectFragment&) {}  // opt-in, MDE/low latency
    virtual void onNrtFile(ServiceHandle, const NrtFile&) {}
    virtual void onEsgFragment(ServiceHandle, const EsgFragment&) {}
    virtual void onMpu(ServiceHandle, const MpuFragment&) {}               // MMTP
    virtual void onMmtSignaling(ServiceHandle, const MmtMessage&) {}       // MP table, mmt_atsc3_message contents
    virtual void onEvent(ServiceHandle, const AppEvent&) {}                // A/337 (MPD EventStream, emsg, AEI)
    virtual void onObjectError(ServiceHandle, const ObjectError&) {}
    virtual void onStats(const Stats&) {}
    virtual void onError(const StackError&) {}
};
}
```

Deliverable types (fields are indicative):

- `MediaSegment{component, representationId, toi, contentLocation, bytes, transferLength,
  presentationTime?, sct?, codepoint, complete, damaged, arrivalTime}`;
  `InitSegment{…, timelineChanged}`.
- `NrtFile{contentLocation, contentType, contentEncoding, bytes (inflated), md5Ok?, appContextIds,
  filterCodes, expires, packageMember?}`.
- `MpuFragment{packetId, assetId, assetType, mpuSequence, movieFragmentSequence, init?, bytes
  (moof+mdat), presentationTime, damaged}`.
- `Component{id, kind (video/audio/subtitle/app/nrt/unknown), transport (route/mmtp), tsi|packetId,
  representationId|assetId, codecs, bandwidth, lang, role, plp}`.

Design rules: callbacks carry `std::shared_ptr<const T>` payloads; big buffers are never copied
into the callback; the sink can retain them. All callbacks are on the pipeline thread unless a
`DispatchQueue` is installed.

### 5.13 Hybrid / broadband hooks

The stack does not fetch, but it computes and exposes what a fetcher needs:

- SLT `SLTInetUrl`/`SvcInetUrl` with `urlType` (ESG, SLS signaling server, …), and the A/331 §6.9
  path rules for the signaling server (`<base>/<serviceId>/…`).
- MPD `BaseURL`s and `BroadbandComponent` information from the USBD, `HELD` broadband entry pages,
  `DWD` windows, `APD` file-repair timing (`offsetTime`, `randomTimePeriod`) with
  `EFDT`/`fileTemplate` to compute repair URLs and byte ranges of missing data (A/331 §8.3).
- `UserAgentBuilder` produces the A/344 `ATSCUser-Agent` string
  (`UserAgentString-20211208.abnf`): `ATSC3/<yyyy[-mm]> (<capabilities>; <vendor>; <model>; <sw>; <family>[; <hw>])`,
  e.g. `ATSC3/2022-06 (50B 512 &; TVco; Model55; R4.2; com.tvco.55; 5.234)`, from the receiver
  capability set and identity strings supplied in `Config`.
- `IBroadbandFetcher` interface the application can implement; the stack calls it only when the
  application enables `fileRepair` or `signalingServer` features.

### 5.14 Security (A/360)

Optional module behind `ISecurityProvider` (OpenSSL implementation provided, can be disabled at
build time):

- CDT (LLS 0x06) → certificate store (`CertificationData/ToBeSignedData/...`, OCSP refresh).
- SignedMultiTable signature (CMS SignedData over the payload loop), `multipart/signed` SLS,
  signed NRT packages (CP 4), `signed_mmt_message()`.
- Results are attached to the data (`SignatureStatus`) and reported through `onError`/stats; the
  stack **never** drops or delays data because of a missing or invalid signature (decision Q5). There
  is no reject policy; an application that wants enforcement filters on `SignatureStatus` itself.
- Content protection (CENC) is only signaled through to the application (`@protected`,
  `drmSystemId`, Security Properties descriptor, `ContentProtection` in MPD). Observed: WBAL with
  `protected="true"`, `drmSystemID="urn:uuid:edef8ba9-79d6-4ace-a3c8-27dcd51d21ed"` and an MPD with
  `ContentProtection` (`urn:mpeg:dash:mp4protection:2011`, `cenc:default_KID`) plus the Widevine
  `cenc:pssh`; segments are delivered unchanged and the application's DRM client decrypts.

### 5.15 Clock and timing

- `Clock` combines the host monotonic clock, LLS `SystemTime` (UTC offset, leap seconds, DST,
  `currentUtcOffset`), ROUTE SCT/presentation time (NTP 64), MMTP timestamps (NTP short / 64) and
  pcap timestamps in replay. It offers `toWallclock(ntp)`, `now()`, and a drift estimate.
- Object and segment deliverables carry both arrival time and signaled presentation time.

### 5.16 XML signaling model and schema set

The directory `specs/schemas/` holds the A/331 schema release of 2021-12-08 (plus the A/332 `SA-1.0`
schema it depends on) and example instances. Per the release notes, **the schemas are normative and
take precedence over the A/331 text**; the typed structures in this specification are therefore
derived from the schemas, and every parser cites its schema file. The directory is not versioned: `setup.sh --schemas`
downloads `A331-2022-09-Schema.zip` from atsc-schemas.org and sorts its 49 files into `lls/` and
`sls/{application,emergency,mmtp,route}/` (the DWD schema with its examples and the ReadMe are filed twice, giving
54 files), then checks the tree against the aggregate SHA-256 the tests were written for.

| Namespace | Schema file | Root element(s) | Used by |
|-----------|-------------|-----------------|---------|
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/SLT/1.0/` | `lls/SLT-1.0-20211209.xsd` | `SLT` | 5.7.2 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/SYSTIME/1.0/` | `lls/SYSTIME-1.0-20211208.xsd` | `SystemTime` | 5.7.1 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/RRT/1.0/` | `lls/RRT-1.0-20210128.xsd` | `RatingRegionTables` | 5.7.1 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/SA/1.0/` | `lls/SA-1.0-20210312.xsd` | `Capabilities`, `ContentAdvisoryRatings`, `OtherRatings`, `ATSC3ServiceExtension`, `Components`, `ContentIcon`, `Preview` (ESG private extensions) | 5.7.2, 5.10.4, 5.11 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/AEAT/1.0/` | `sls/emergency/AEAT-1.0-20210322.xsd` | `AEAT` | 5.7.1 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/OSMN/1.0/` | `sls/application/ONSCREEN-1.0-20210401.xsd` | `OnScreenMessageNotification` | 5.7.1 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ROUTEUSD/1.0/` | `sls/route/ROUTEUSD-1.0-20170920.xsd` | `BundleDescriptionROUTE` | 5.9.2 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/S-TSID/1.0/` | `sls/route/S-TSID-1.0-20220708.xsd` | `S-TSID`, `SrcFlow`, `RepairFlow` | 5.9.3 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-FDT/1.0/` | `sls/route/ATSC-FDT-1.0-20210428.xsd` | attributes on `fdt:FDT-Instance` / `fdt:File` | 5.8.5 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/MMTUSD/1.0/` | `sls/mmtp/MMTUSD-1.0-20210401.xsd` | `BundleDescriptionMMT` | 5.10.4 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/AppSignaling/HELD/1.0/` | `sls/emergency/HELD-1.0-20210312.xsd` | `HELD` | 5.11 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/AppSignaling/DWD/1.0/` | `sls/application/DWD-1.0-20180830.xsd` (identical copy under `emergency/`) | `DWD` | 5.11 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/APD/1.0/` | `sls/application/APD-1.0-20170209.xsd` | `AssociatedProcedureDescription` | 5.13 |
| `tag:atsc.org,2016:XMLSchemas/ATSC3/Delivery/ATSC-META/1.0/` | `sls/application/ATSC-META-1.0-20190122.xsd` | attributes `nextURL`, `availAt` on envelope `item` | 5.9.2, 5.13 |
| external, not in the release | `urn:ietf:params:xml:ns:fdt` (RFC 6726), `urn:3GPP:metadata:2007/2012:MBMS:FLUTE:FDT`, `urn:3gpp:metadata:2005:MBMS:envelope`, `urn:mpeg:dash:schema:mpd:2011`, `xml.xsd` | `FDT-Instance`, `metadataEnvelope`, `MPD` | must be obtained separately for validation |
| non-XML grammars | `ContentRating-20211208.abnf`, `UserAgentString-20211208.abnf`, `MakeContentAdvisoryString-20210402.xsl` | — | 5.11, 5.13 |

Parsing policy common to all fragments:

1. **Namespace handling.** Match on local names; accept the schema namespace, prefixed forms
   (`slt:SLT`), and the legacy `http://www.atsc.org/XMLSchemas/...` variant observed on air.
   Record the namespace actually used for diagnostics.
2. **Extension points.** Every complex type ends with `xs:any namespace="##other"` and allows
   `xs:anyAttribute`. Unknown elements and attributes are never errors; they are kept as raw XML
   on the parent structure (`extensions` field) so that applications and future versions can read them.
3. **Defaults.** Schema defaults are applied when attributes are absent (e.g. `protected=false`,
   `configuration=Broadcast`, `slsMajorProtocolVersion=1`, `Payload@codePoint=128`, `@order=true`,
   `kscFlag=true`, `notificationDuration=PT1M`, `ptpPrepend=0`). Attributes without a schema default
   are `std::optional`.
4. **Lists.** `xs:list` attributes (`bsid`, `OtherBsid`, `lctTSIRef`, `coupledServices`,
   `subAudience`, `dwFilterCode`, `appContextIdList`, `filterCodes`) are whitespace-separated and
   may be empty strings.
5. **Types.** `xs:dateTime` with time zones and fractional seconds, `xs:duration` (`-PT4H`, `PT1M`),
   `xs:hexBinary` (`fecOTI`), `xs:anyURI` (kept verbatim, no normalisation), ranges enforced
   leniently (out-of-range values are clamped and counted, not rejected).
6. **Input hygiene.** Tolerate UTF-8 BOM (present in one AEAT example), CRLF, leading whitespace,
   missing XML declaration, `xsi:schemaLocation`, and placeholder text (`?????`) in string fields.
7. **Validation.** Runtime parsing is schema-less (fast, lenient). XSD validation is a test-time and
   `atsc3scan --validate` feature using libxml2 with the schema set plus the external schemas listed
   above; it reports but never gates.
8. **Golden vectors.** Every example instance in `specs/schemas/` is a unit-test input with an expected
   typed result; every on-air table decoded from `samples/` (SLT, SystemTime, AEAT, CDT, UDST, EFDT)
   is a second set.

### 5.17 Web server integration (`atsc3::web`)

An optional integration (4.1) under `integrations/web/`, enabled with the CMake option `ATSC3_WEB=ON`, that
embeds an HTTP server in the same process as the stack and exposes the stack's state, signaling and media to a browser. It is a
consumer of the public API only (`IEventSink` + queries, 5.12): nothing in the core depends on it.
Its purpose is (a) the end-to-end tests of Section 5.18 and (b) a developer UI; it is not a
product UI and makes no attempt at DRM, multi-user or Internet exposure.

#### 5.17.1 Process and binding

- Delivered as the daemon `atsc3d` (stack + chosen source + web server) and as a library class
  `web::Server` that an application can attach to its own `Stack`.
- Binds to `127.0.0.1:8080` by default; any other bind address requires `--web-allow-remote` and an
  optional bearer token (`--web-token`). CORS is enabled for `http://localhost:*` so the Vite dev
  server can talk to it during frontend development.
- Runs its own I/O thread(s); all stack interaction goes through the thread-safe command queue and
  through an `IEventSink` adapter that copies deliverables into the server's own bounded caches.
  The pipeline thread never blocks on a socket.
- Implementation: **cpp-httplib** (header-only, MIT) for HTTP/1.1, static files, SSE and byte-range
  responses; **IXWebSocket** (BSD) for the binary media WebSocket. Both are optional dependencies
  pulled only when `ATSC3_WEB=ON`.
- Serves the built frontend (`integrations/web/frontend/dist`) from a configurable directory or from resources
  embedded at build time, so a single binary is enough for the e2e run.

#### 5.17.2 REST API (`/api/v1`, JSON)

| Method / path | Purpose |
|---------------|---------|
| `GET /status` | source kind and mode (RF tuner / virtual tuner file, pacing, position), lock, bsid list, uptime, stack version |
| `GET /time` | the stack clock (5.15) as `xs:dateTime` and NTP; used as DASH `UTCTiming` source so dash.js runs on the capture timeline even in replay |
| `GET /services` | `ServiceEntry[]`: bsid, serviceId, globalServiceId, major/minor, shortName, category, slsProtocol, protected, drmSystemId, hidden, hideInGuide, essential, presentable, selected, sls address |
| `POST /services/{bsid}/{serviceId}/select`, `DELETE …/select` | select / deselect (several may be selected) |
| `GET /services/{bsid}/{serviceId}` | service detail: components (kind, transport, tsi/packetId, repId/assetId, codecs, bandwidth, lang, role), SLS fragment versions, MPD summary, HELD/DWD, statistics |
| `GET /services/{bsid}/{serviceId}/sls/{usbd|stsid|mpd|held|dwd|apd|rsat|mpt|envelope}` | raw fragment (XML or binary) as last received, with version and arrival time |
| `GET /lls` and `GET /lls/{slt|stt|aeat|rrt|omn|cdt|userdefined}[?group=n]` | current LLS tables per group, parsed JSON and raw XML |
| `GET /link` | LMT/PLP map, RDT summary, L1 info (incl. sidecar-derived), PLPs seen |
| `GET /alerts` | AEAT entries in effect (parsed), plus history for the session |
| `GET /esg/services`, `GET /esg/schedule?from=&to=[&service=]`, `GET /esg/content/{id}`, `GET /esg/icons/{name}` | EPG data from the ESG parser (5.17.5) |
| `GET /stats` | per layer and per service counters (same `Stats` as the API) |
| `GET /source`, `POST /source` | virtual tuner control: list capture files under a configured directory, open a file, set pacing (`realtime`/`nonrealtime`), speed, loop, seek to a capture time; for RF tuners: tune parameters if the tuner supports them |
| `GET /events` | **Server-Sent Events** stream: `serviceList`, `serviceDetail`, `lls`, `alert`, `segment` (metadata only), `mpu` (metadata), `stats` (1 Hz), `log` (rate-limited), `source` |
| `GET /log?level=` | ring buffer of recent log lines |

All responses carry `Cache-Control: no-store`. JSON field names are camelCase versions of the C++
API types so the frontend's TypeScript types can be generated from one source.

Implementation note: `integrations/web/server.cpp` implements `GET /status` (with `?since=<version>&wait=<ms>`
long-poll on the engine's state version, used by the frontend instead of SSE), `GET /services`,
`GET /services/{bsid}/{sid}` (detail with raw fragments and recent objects), `GET …/sls/{name}`,
`POST`/`DELETE …/select`, `GET /lls`, `GET /link`, `GET /source` (capture directory listing with the
detector verdict) and `POST /source` (`path`, `pacing`, `speed`, `loop`, `start`, or `action=close|pause|resume`,
form-encoded; paths must lie under a configured capture directory), `GET /time` (`wallUnixMs`,
`captureUnixMs` of the last delivered unit, position, paused/finished — the EPG's "now" is the capture
clock while a capture is open), `GET /esg` (guide summary: stats, Service entries joined to the SLT by
`globalServiceID` then `major.minor`, the SLT services of category 4, SGDD descriptors, cached icon files),
`GET /esg/services`, `GET /esg/schedule?from=&to=[&service=<ESG service id>]` (programmes overlapping the
window, Unix seconds), `GET /esg/content/{id}` (full Content fragment incl. XML), `GET /esg/icons/{name}`
(NRT file bytes of the ESG service) and `GET /esg/fragments?type=&limit=&xml=1` (raw fragments for
inspection); `/status` carries `esgVersion` and `captureUnixMs`. The guide (`esg::Guide`) is rebuilt from the
engine's fragment snapshot only when `esgVersion` moved. The media endpoints of 5.17.3–5.17.4 are implemented
with the player; `/alerts`, `/stats`, `/events` and `/log` are not. Also served: the Application Context Cache origin
`/apps/{bsid}/{sid}/index.json` and `/apps/{bsid}/{sid}/{n}/{path}` of 5.18.4 (outside `/api`, no token,
like `/media`) and the `apps` array on every service entry and detail. POST/DELETE requests
need a body (possibly empty) because cpp-httplib reads it before routing. The server sits on
`service::Engine` (5.11), which keeps all stack state behind one mutex and bumps a version counter on
every change.

#### 5.17.3 Media delivery for ROUTE/DASH services (dash.js path)

The server acts as a **local DASH origin** fed by broadcast objects:

- Segment store: for each selected ROUTE service a ring buffer per LCT channel holding the init
  segment and the last `N` media segments (default `N` = max(12, 2 × `timeShiftBufferDepth` /
  segment duration)), keyed by `Content-Location` as produced by the EFDT / `fileTemplate`, plus
  a secondary key `(repId, number|time)` derived from the MPD `SegmentTemplate`. Objects are stored
  as `shared_ptr<const Object>` (no copy).
- `GET /media/{bsid}/{serviceId}/mpd.mpd` returns the **rewritten MPD**: all `BaseURL`s replaced by
  `/media/{bsid}/{serviceId}/`; `UTCTiming` replaced by `urn:mpeg:dash:utc:http-xsdate:2014` →
  `/api/v1/time`; a `Location` element pointing back to this URL; `minimumUpdatePeriod` clamped to
  ≤ 1 s when `SegmentTimeline` is used (broadcast MPDs update via SLS every segment while advertising
  30 s, as in the BWI feed); `ContentProtection` elements kept (dash.js will fail to play protected
  services without a license server, which the UI reports as "protected"); everything else untouched.
- `GET /media/{bsid}/{serviceId}/{path}` serves init and media segments with correct `Content-Type`
  (`video/mp4`, `audio/mp4`, `application/mp4`) and `Content-Length`. A request for a segment that
  is being received is held for up to `media_wait_ms` (default 1500 ms, long-poll) and served when
  complete; a segment that is unknown or expired returns 404 so dash.js retries or skips. Byte-range
  requests are honoured (dash.js uses them for `sidx`-indexed content).
- Because dash.js takes "now" from `/api/v1/time`, replay of a capture works without rewriting
  `availabilityStartTime`; with a real tuner the stack clock equals wall-clock time (STT-disciplined).
- Recommended dash.js settings are published at `GET /media/{bsid}/{serviceId}/player.json`
  (`liveDelay` = 2 × max segment duration + 0.5 s, `liveCatchup` off, `utcSynchronization`
  via the `UTCTiming` above, `abr.autoSwitchBitrate` off because broadcast has one representation
  per adaptation set).
- Captions (`stpp` / IMSC1) flow through dash.js's own TTML rendering. A/337 `EventStream`s stay in
  the MPD; `emsg` boxes stay in segments.

Implementation note: `service::Engine` keeps, per **selected** ROUTE service, a segment store
(`OpenOptions::media_cache_bytes`, default 96 MiB, init segments pinned and not counted, oldest media
segments evicted, cleared on deselect); objects are classified with `sls::matchSegment` (channel
`MediaInfo@repId` template first, then every template, then TOI as `$Number$`), an `ftyp` object is an
init segment even when the template misses. `integrations/web/server.cpp` serves `GET /media/{bsid}/{sid}/mpd.mpd`
(`sls::makeOriginMpd`: one `BaseURL` = `/media/{bsid}/{sid}/`, `Location`, `UTCTiming http-xsdate` →
`/api/v1/time/xsdate`, `minimumUpdatePeriod` clamped to 1 s for SegmentTimeline MPDs, everything else
including `type`, `availabilityStartTime` and `ContentProtection` untouched), `offline.mpd` (the static
rewrite over the cached segments), `player.json` (`liveDelay` = 2 × `maxSegmentDuration` + 0.5 s,
catch-up off, ABR off, `protected`), `index.json?since=` (representations with init/segment counts and the
cached segments in arrival order) and `{segment}` (bytes with `Content-Type` from the MPD kind, `Range`
honoured by cpp-httplib, a 1.5 s long-poll for a name not cached yet, else 404). `GET /api/v1/time/xsdate`
returns the capture clock as `xs:dateTime` while a capture is open. The MPD's `UTCTiming` however points at
`GET /media/{bsid}/{sid}/time.xsdate`, the **service clock**: the availability time (per the MPD's
`availabilityStartTime`, period start, `startNumber` and segment duration, or `$Time$`) of the newest cached
segment plus 100 ms, falling back to the capture clock until segments exist. **Observed:** both encoders
transmit a segment several seconds before its nominal availability (about 7 s on the UniSoft feed, 2–5 s on
the Sinclair feed), and a receiver holds nothing from before it opened the sessions; a player clocked by the
capture time therefore asks for segments that were never received, gets 404s and dash.js gives up after three
retries. Clocking the player by the newest received segment keeps its live edge inside the cache for both
the Sinclair (epoch-anchored, epoch-second numbers) and UniSoft (encoder-start-anchored) MPDs without
rewriting times; `index.json` reports both clocks (`liveEdgeMs`, `captureMs`). `player.json` also carries
`minSegments` = ceil(liveDelay / segment duration) + 1, the number of cached segments per A/V representation
the player waits for before starting. Replay must run in real time for playback; non-real-time pacing moves
the clocks faster than the player can follow. `/media` is outside the bearer-token check. The WebSocket push
path of 5.17.4 is still unimplemented.

**`$Number$` calibration.** The origin checks the MPD's numbering against the media timeline. For every
`$Number$` Representation with a cached media segment it compares where the numbering puts the newest segment,
`(number − startNumber) × duration`, with where its media really is, `tfdt` (in the init segment's `mdhd`
timescale) minus `presentationTimeOffset`. When the two differ by more than half a segment, the difference
rounded to whole segments becomes `numberShift` and the Representation's `startNumber` moves by it
(`index.json`: `startNumber`, `effectiveStartNumber`, `numberShift`); `mpd.mpd` carries the shifted value on the
Representation's own `SegmentTemplate` (a copy of the inherited AdaptationSet/Period template when it has none)
and the service clock is computed with it. `presentationTimeOffset` is never rewritten: a per-representation
offset would move audio and video apart, whereas a `startNumber` shift only changes which file the player fetches
for a given presentation time. **Observed:** UniSoft `usc01` (4K HEVC) signals `Period@start="PT0S"`, no
`presentationTimeOffset` and `startNumber` 67 (video) / 65 (audio) while every segment's `tfdt` equals
`number × 2.002 s`; dash.js fetched the right files for its playhead but MSE placed them 134 s later, so the
buffer filled ahead of the playhead and playback never started. `usc02` declares `Period@start` and
`presentationTimeOffset` and is still one segment (2.1 s) early, which dash.js tolerated. The Sinclair
epoch-anchored numbering agrees with its `tfdt` and is left alone.

#### 5.17.4 Media delivery for MMT services and generic MSE (push path)

dash.js has no MMT input, so MMT services are played by a small MSE player in the frontend fed over
a binary WebSocket. The same path is available for ROUTE services (`?mode=push`) as the dash.js-free
reference.

- `WS /ws/media/{bsid}/{serviceId}` — after connect the server sends a JSON `init` message listing
  the components (id, kind, codecs string in RFC 6381 form derived from the MP table / VSPD / ASPD or
  from the MPD, timescale, language) and then, per component, a binary message `[4-byte header: component id(16) | flags(16)] + payload`:
  - flags bit 0 = initialization segment (`ftyp`+`moov`), bit 1 = media fragment (`moof`+`mdat`),
    bit 2 = discontinuity, bit 3 = damaged (gaps zero-filled).
  - For MMT, fragments are the `MpuFragment` output of 5.10.3; the server normalises them for MSE:
    the `mmpu` box is removed, `ftyp` brands are rewritten to `iso6`/`dash`/`msdh`, a `styp` is
    prepended to each fragment, and `tfdt` is set from the `MPU_timestamp_descriptor`-derived
    presentation time so the browser timeline is continuous across MPUs.
  - For ROUTE push mode the objects are forwarded as-is (they are already CMAF).
- A JSON `clock` message (every second) carries the stack clock and the latest presentation time
  per component so the player can position `currentTime` near live edge.
- Backpressure: the socket send queue is bounded per component (default 8 fragments); when the
  client lags, the oldest unsent media fragments are dropped and a `discontinuity` flag is set.
- Browser codec reality (documented in the UI): HEVC `hev1/hvc1` plays in Chrome/Edge with platform
  decoding and in Safari; AC-4 is not decodable by any browser MSE today, so WNUV-style audio is
  signalled as `unsupported` and the player runs video-only with the audio track skipped; AAC
  (`mp4a.40.2`) and E-AC-3 (`ec-3`, Edge/Safari) play. `MediaSource.isTypeSupported()` is checked per
  component and the result shown.

Implementation note (**HTTP polling instead of the WebSocket — decision Q11**): the engine keeps,
per selected MMTP service, the latest init segment per packet_id and the reconstructed fragments in one
bounded ring (`OpenOptions::media_cache_bytes`, oldest fragment of any component evicted first, cleared on
deselect); init segments and fragments share one monotonic `seq` per service. `GET
/media/{bsid}/{sid}/player.json` answers `{"mode":"push","indexUrl":…,"liveDelay":2×MPU duration+0.5,
"minFragments":2}` for MMTP services (`"mode":"dash"` for ROUTE). `GET /media/{bsid}/{sid}/mmt/index.json?
since=` lists the components (packet_id, MP-table asset id/type, kind/sample entry/codecs/mime/timescale/
language/size from the init's media track, `init{seq,bytes,url}`, `timestampOffsetS` (null until the component's first fragment froze it: the
player appends nothing before), timeline `mode`,
counts), a `generation` that changes whenever the origin's timelines start over (source reopen, loop wrap
of the pcap tuner, encoder restart; the player then restarts itself) and the fragments with `seq > since` (`startS`/`durationS` on the service timeline, `damaged`,
`discontinuity`, `tfdtRewritten`, presentation time), plus the anchor, live edge and cache state; `GET
…/mmt/{pid}/init.mp4` and `…/mmt/{pid}/{mpu}.m4s` serve the bytes. Assets announced on other flows by
`MMT_general_location_info` type 1 are subscribed into the same session. `integrations/web/frontend/src/player/
MsePushPlayer.ts` polls the index every 500 ms, picks one video and one audio component that pass
`MediaSource.isTypeSupported` (after every A/V component has an init, or 3 s after the first), creates one
`SourceBuffer` per component in `segments` mode with the component's `timestampOffset`, appends the init
then fragments strictly in `seq` order (one append in flight, never twice), starts after `minFragments`
per buffer with `currentTime` at the buffered start, keeps near the live edge (`liveDelay`), jumps gaps
(Chrome never jumps ≥ 25 ms, Firefox up to 0.5 s), trims 30 s behind the playhead, recovers from
`QuotaExceededError`, sets `MediaSource.duration` to Infinity, and reports per-component state;
`PlayerPanel.vue` uses it when `player.json` says `push` and keeps dash.js for ROUTE. Verified: headless
Firefox plays `usc03m` (1080p HEVC + AAC, 0 dropped frames) and WNUV (720p HEVC, AC-4 skipped) through
the daemon; `e2e/playback.spec.ts` covers the MMT path. No EME on the push path yet.

#### 5.17.5 ESG data model (core layer `esg`)

The EPG needs the OMA BCAST Service Guide data model; it is the core's `esg` layer (`src/esg`, header
`atsc3/esg/esg.hpp`, namespace `esg`; Q6): it consumes the `EsgFragment`s of 5.11 (SGDD
`application/vnd.oma.bcast.sgdd+xml`, SGDU `application/vnd.oma.bcast.sgdu`, both gzip-inflated by
the ROUTE layer) and builds `Service`, `Schedule`, `Content` fragments with the A/332 extensions
(`ATSC3ServiceExtension/Icon|MajorChannelNum|MinorChannelNum`, `Components`, `ContentIcon`,
`ContentAdvisoryRatings`, `Preview`). SGDU parsing follows the OMA BCAST unit header
(`extension_offset`, `fragment_encoding/type`, per-fragment `fragmentTransportID`, `fragmentVersion`,
`offset`, `size`). Fragments are versioned and expire by `validTo`. Icons referenced from fragments
are resolved against the NRT files of the ESG service (TSI 80 PNGs in the BWI feed).

Implementation note: `src/esg` (namespace `esg`) implements `parseService`,
`parseSchedule`, `parseContent` (Name/Description as A/332 `@text` or OMA element text, `Length` as
xs:duration, `Genre/@href`, `ContentAdvisoryRatings` with `RegionIdentifier`/`RatingDescription`/
`RatingDimVal`, `OtherRatings`, `Icon`/`ContentIcon` with MIME and size, `Components`, `Capabilities`)
and `Guide`: a versioned store keyed by fragment id (newer or equal versions replace), `build()` joins
Schedule `ContentReference/PresentationWindow`s with Content fragments into `Programme`s (unresolved
content references are kept as untitled rows, identical windows across schedule fragments collapse),
`programmes(from, to, service)` answers overlap queries and `span()` the covered interval. `Preview`
and the `validTo` expiry are not applied yet (the BWI guide carries no validity attributes).

#### 5.17.6 Non-goals and limits

No HTTPS, no authentication beyond the optional token, no transcoding, no recording, no multi-tenant
state; at most four concurrently selected services; media caches bounded (default 256 MiB total).

### 5.18 Vue 3 frontend of the web integration and end-to-end tests

#### 5.18.1 Stack and structure

`integrations/web/frontend/` is a Vite + **Vue 3** (Composition API, `<script setup>`) + TypeScript project with
Pinia for state, Vue Router, and **dash.js** (BSD-3) as the only media dependency. No component
framework is required; plain CSS with a dark/light theme. Node 20 LTS, `npm ci`, `npm run build`
produces `integrations/web/frontend/dist`, which the server embeds or serves.

Implementation note: `src/stores/stack.ts` long-polls `/status` and refreshes `/services` when the version changes; views
Source, Services, service detail (`/services/:bsid/:sid`: components with codecs, MPD summary, SLS and
MMT fragments as pretty-printed XML, recent objects), Signaling (LLS), Link and Stats are implemented,
as is the EPG view (`EpgView.vue` + `components/EpgGrid.vue`: services × time grid from `/esg/schedule`
with a movable 3–24 h window, "now" line from `/time`, programme detail from `/esg/content` with icon, rating,
genre, components and raw XML, channel click selects the SLT service and opens its page, a "Guide delivery"
panel with SGDD/SGDU/icon statistics; the view refreshes when `/status` reports a new `esgVersion`).
The service detail's component table carries a "Browser" badge per track (`src/util/capabilities.ts`):
`MediaSource.isTypeSupported` plus `navigator.mediaCapabilities.decodingInfo` (type `media-source`, with the
component's resolution, bitrate and frame rate) give playable / not smooth / no decoder, protected tracks show
DRM with the Widevine CDM presence from `requestMediaKeySystemAccess`, caption tracks show "renderer", and
bare MMT fourccs are probed with a typical profile string (marked ≈). Results are cached per codec string and
parameters. `components/PlayerPanel.vue` (dash.js 5 from npm) is shown in the service detail of a
selected ROUTE A/V service and once per selected service in the Player view; `initialize(video, mpd.mpd,
autoplay)` with settings from `player.json` (live delay, catch-up and ABR off,
`capabilities.useMediaCapabilitiesApi` so unsupported codecs such as AC-4 are filtered instead of failing),
autoplay handling (starts with sound when the document already has user activation, otherwise muted; on
dash.js `playbackNotAllowed` it retries muted and, if the browser refuses even that, shows a click-to-play
overlay — Firefox's "block audio and video" setting does, verified with Playwright `firefoxUserPrefs`; a
Mute/Unmute button and a "muted by the autoplay policy" badge), a startup gate (retries `player.json` until
the MPD exists, then waits for `minSegments`
per A/V representation), an automatic restart on dash.js error 27 (segment download failed: cache gap or
eviction, up to five times), audio and caption track selectors, buffer / latency / dropped-frame stats, the
cache state and both clocks from `index.json`, and EME through dash.js's protection controller: a license URL field (kept in
localStorage) is passed as `setProtectionData` for Widevine and PlayReady, key-session and license events
are shown, and a protected service without a license URL says so. Alerts is a placeholder. Node 18 is
sufficient to build the frontend (Vite 6 line); the Playwright e2e suite (`e2e/playback.spec.ts`, verified against the UniSoft capture in
Playwright's Firefox: HEVC + AAC decode, frames rendered) needs
Node 20+ and `npx playwright install firefox`. dash.js's ESM `MediaPlayer` export carries no `events`
property; the panel spells the event names out.

```
frontend/
  src/api/        generated TypeScript types + fetch/SSE/WebSocket clients for 5.17.2–5.17.4
  src/stores/     services, selection, lls, alerts, esg, stats, source (Pinia)
  src/player/     DashPlayer (dash.js wrapper), MsePushPlayer (WebSocket → SourceBuffer), codec checks
  src/views/      Services, Player, Epg, Alerts, Signaling, Link, Stats, Source
  src/components/ ServiceCard, ComponentList, XmlViewer, EpgGrid, AlertBanner, StatChart
  e2e/            Playwright specs and fixtures (capture names, expected service lists)
```

#### 5.18.2 Views and behaviour

- **Services**: table of all services from all LLS groups (channel number, name, category badge,
  protocol ROUTE/MMT, protected/hidden/essential badges, presentable flag). Click selects the service
  and navigates to the player; multiple selection allowed for PiP-style tests.
- **Player**: a `<video>` element driven by MSE. ROUTE services use `DashPlayer`
  (dash.js `MediaPlayer().initialize(video, '/media/{bsid}/{sid}/mpd.mpd', true)` with the settings
  from `player.json`); MMT services (and ROUTE in push mode) use `MsePushPlayer`, which creates one
  `SourceBuffer` per supported component (`sequence` mode off; `timestampOffset` from the first
  fragment's `tfdt`), appends init then fragments, trims buffered ranges behind `currentTime − 30 s`,
  seeks to the live edge on start and shows per-component codec support, buffer levels, dropped
  frames and fragment counters. Audio language and caption track selection follow the components.
  Protected services show a DRM notice and attempt playback anyway so the failure mode is visible.
- **Service detail** (`/services/{bsid}/{sid}`): the service's components, MPD summary, SLS fragments,
  recent objects and the HELD application table; once selected, its PlayerPanel with the broadcaster
  application runtime of 5.18.4 laid over the video. The Player view deliberately hosts no application:
  it shows several selected services at once and the remote control's input would have no single target.
- **EPG**: a grid (services × time) from `/esg/schedule`, with programme detail (title, description,
  ratings, icon) from `/esg/content`; "now" from `/time`; jumping to a service selects it.
- **Alerts**: current AEAT alerts with text per language, locations, media links; a banner on any view.
- **Signaling**: LLS and SLS browser with raw XML (pretty-printed) and parsed JSON side by side,
  version history, and the MMT MP table / `mmt_atsc3_message` list for MMT services.
- **Link**: LMT/PLP map, RDT, L1 info, flows seen, unrouted flows.
- **Stats**: live charts (1 Hz SSE) of bitrate per PLP/service, object completion, MMTP packet loss,
  segment latency (completion time minus availability time).
- **Source**: the virtual tuner panel: pick a capture file, choose real-time / non-real-time, speed,
  loop, seek; or show RF tuner parameters. Non-real-time mode disables playback (segments arrive
  faster than real time) and is intended for signaling/EPG inspection only.

#### 5.18.3 End-to-end tests

Playwright (Chromium, plus Chrome/Edge channels for HEVC on capable hosts) drives the frontend
against `atsc3d` running the pcap virtual tuner in real-time mode. Each spec opens a capture via
`/api/v1/source`, waits for the expected service list, then exercises a scenario:

| Capture | Scenario | Assertions |
|---------|----------|------------|
| `BWI_20230307_1742.pcap` (STLTP) | discovery | 6 services listed (WBFF 45.1, WBAL 11.1 protected, WMAR 2.1, ESG hidden-from-list unless "show all", WMPT 22.1, WNUV 54.1 MMT) within 6 s; LLS group 1; LMT with 3 PLPs |
| same | ROUTE playback (WMAR) | dash.js reaches `PLAYBACK_PLAYING`, `video.currentTime` advances ≥ 5 s in 10 s, no `MEDIA_ERR_DECODE`; buffered ranges grow; captions track `d4_4` present |
| same | protected service (WBAL) | UI shows protected + Widevine system id; dash.js emits a protection error; no crash |
| same | MMT playback (WNUV) | `MsePushPlayer` receives init + fragments for pids 100/200/201/300; video SourceBuffer appends without `QuotaExceeded`/decode error; AC-4 components flagged unsupported; `currentTime` advances on HEVC-capable browsers (skipped on plain Chromium) |
| same | EPG | `/esg/schedule` returns programmes for the four A/V services; grid renders; content detail has rating and icon |
| same | alerts | the WNUV test AEAT (`BNA`, 11 FIPS codes) appears in Alerts |
| `UniSoft-3xROUTE-24-October-2019.pcap` (IP bypass) | playback usc01/usc02/usc03 | 4K HEVC segments served by `$Number$` names; AAC audio plays in all browsers; IMSC captions on usc03; app package `uscapp.zip.multipart` visible under NRT |
| `UniSoft-2xROUTE+1MMT-5-November-2019.pcap` | MMT usc03m | MP table shows `usc03m-1` with `hev1` + `ac-4`; push player plays video |
| `UniSoft-3xROUTE-24-October-2019.pcap` | broadcaster application usc01 (`e2e/apps.spec.ts`, implemented) | HELD table lists `uscapp`; the app is launched over the player within 30 s of selection with `?wsURL=…&rev=20240403`, its canvas renders; from inside the app `wsURL/atscCmd` answers `org.atsc.query.service`, `query.baseURI` (the cache base), `query.deviceInfo` (ArrowUp 38, Select 13), an unknown method with -32601, `query.rmpPlaybackState`; `wsURL/atscVid` fails to connect; before any grant a navigation key reaches the app as BAAppear 1001; `request.keys` accepts the arrows, Select and Numeric and refuses ChannelUp, after which the virtual remote delivers those keys with the promised keyCodes, the keyboard takes the same route, an ungranted key stays with the receiver, Back hides and App shows the app (delivering BAAppear again), Terminate removes the frame |
| small samples | detection + signaling only | service list matches Appendix B; playback not attempted (captures too short) |
| virtual tuner modes | pacing | switching to non-real-time disables the player; loop wraps reset the service list without duplicates |

CI runs the Chromium subset headless on every change (signaling, EPG, alerts, MSE append-level
assertions); the HEVC playback assertions run on a self-hosted runner with hardware decoding. The
e2e suite is the acceptance test of Phases 1 and 2.

#### 5.18.4 Broadcaster applications (A/344) in the service view

Implemented. An A/344 Broadcaster Application is the HTML5 package a HELD names for a service
(5.11 item 6). The frontend runs it on the service's own page only, laid over that service's player; the
Player view, which shows several selected services side by side, hosts none, because remote-control input
would have no single target there. The A/344 runtime proper (a certified user agent with the complete API
set) stays out of scope (1.2); this is the harness's slice of it, sized to run the applications on air in
the corpus (3.2 item 18).

**Signaling and the Application Context Cache (`service::Engine`).** `ServiceInfo::apps` holds one
`AppInfo` per `HTMLEntryPackage` of the service's HELD — the ROUTE SLS fragment, or for MMTP services the
`mmt_atsc3_message` 0x0003 (and a HELD found in the `ROUTEComponent` session's bundle) — with
`appContextId`, `appId`, the entry package / page / broadband URLs, `appRendering`, `coupledServices`,
`lctTSIRef`, the validity window, and the cache state: `ready` (the broadcast entry page is cached),
`files`, `bytes`, and the progress of the entry package while it is still on the carousel — `package_tsi`
and `package_bytes` from the S-TSID EFDT entry whose Content-Location is the `bcastEntryPackageUrl`,
`package_received` from the ROUTE session's pending object (`route::Session::pending()`: the merged ranges
held so far). A 3.9 MB package at ≈245 kbit/s takes ≈126 s, long enough that silence looked like a fault. The Application Context Cache (A/344 6.2) is kept per service while it is selected:
every delivery object that carries an `appContextIdList` or is a package (`formatId` 3/4 or a
`multipart/*` Content-Type) is stored by its cache path (Content-Location with `./`, leading `/` and any
query stripped; paths with `..` are refused). A `multipart/related` package is unpacked into its parts,
each keyed by the part's `Content-Location`; the media type is the part's `Content-Type`, else the
`metadataEnvelope` item's `contentType` (which also supplies `version`), else guessed from the extension
(`mimeFromName`). The parts stay views into the delivered object (no copy); a newer arrival of the same
package or file replaces what it carries, parts an older version had and the new one lacks are dropped.
The cache is bounded by `OpenOptions::app_cache_bytes` (64 MB per service; objects that would exceed it
are counted as `rejected`), survives deselecting (the carousel of a 3.9 MB package is slow) and is freed
with the capture. For MMTP services the USBD `ROUTEComponent` (`sTSIDUri`, destination IP/port, source
IP; new `mmtp::MmtRouteComponent`) opens a second `sls::RouteReceiver` on that address: its SLS bundle
(an envelope plus an S-TSID on air) is applied like a ROUTE service's, its channels show up as ROUTE
components of the MMT service and the fragment as `stsid.sls (ROUTEComponent)`, and its NRT channels
are opened while the service is selected. `Engine::apps()` returns the `AppIndex` (apps, files, cache
counters) and `Engine::appFile()` one file; `ServiceDetail` lists the component session's fragments.

**Origin (`integrations/web`).** `GET /apps/{bsid}/{sid}/index.json` — `selected`, `wsUrl`, `rev`, `apps[]`
(the `AppInfo` fields incl. `packageTsi`/`packageBytes`/`packageReceived`, plus `index`, `baseUrl` =
`/apps/{bsid}/{sid}/{index}/` and `entryUrl`, null for broadband-only entries), `files[]` (path, contentType, bytes, version, package, appContextIds, tsi, toi,
receivedNs), `cachedBytes`, `cacheLimit`, `objects`, `packages`, `rejected`. `GET
/apps/{bsid}/{sid}/{index}/{path}` serves a cached file with its media type (`charset=utf-8` added to
text types) and `Cache-Control: no-cache`; 404 for unknown services, application indexes and paths. Both
sit outside `/api` and the bearer check, like `/media`, because the application's iframe sends no header.
The entry page is launched as A/344 8.2 requires, `…/{page}?wsURL=ws://{Host}/api/v1/a344/{bsid}/{sid}&rev=20240403`
(the release date of A/344:2024-04), and the server injects `<script src="/a344-shim.js"></script>` as
the first child of its `<head>` (after `<html>` or at the start when there is none). The `wsURL` names no
listening socket: the shim turns it into a channel to the hosting page (Q11 keeps WebSockets out of the
daemon). A broadcast application that bypasses the shim (a worker, a native socket) gets a refused
connection and is logged.

**Runtime (`integrations/web/frontend/src/components/AppOverlay.vue`, `a344/host.ts`, `a344/keys.ts`,
`components/RemoteControl.vue`, `public/a344-shim.js`).** PlayerPanel takes an `apps` prop that only the
service view sets; with it, and a non-empty `apps` list, it renders AppOverlay below its rows and gives
it the `.screen` element that wraps the `<video>`. AppOverlay polls `index.json` every 2 s, picks the
first ready application (else the first with an entry URL) and launches it when `auto-launch` is on:
an `<iframe class="app-frame">` with `sandbox="allow-scripts allow-same-origin allow-forms
allow-modals"`, transparent background, positioned over the video's box — the A/344 4.2.1 rendering
model for `appRendering="false"`: the receiver's player is the RMP, the application a graphics plane on
top. Launch, Hide/Show, Terminate buttons and a status badge (`waiting for the package`, `receiving App.pkg`
with a progress bar fed by `packageReceived`/`packageBytes`, `ready`, `running`, `running, hidden`,
`terminated`, `broadband only`); the HELD table of the service page shows the same percentage; the frame ignores the mouse unless
"mouse to app" is ticked, so the player's own controls underneath stay usable (A/344 applications are
remote-driven); hiding keeps the application alive and
tells it through `blur`/`focus` events, as 4.2.1 allows; deselecting the service, leaving the page or
Terminate removes the frame, resets the key grants and the video transform (coupled-service lifecycle).
The shim replaces `window.WebSocket` inside the application: a URL starting with the page's `wsURL` becomes a
`postMessage` bridge (open/send/close/opened/message/error/closed, the WebSocket API surface with
`readyState`, `on*` handlers and `addEventListener`); any other URL gets the browser's class. Only
`WSPath/atscCmd` is provided; `atscVid`, `atscAud`, `atscCap` and `atscCD` fail like the 404 of A/344
8.2.1. The shim also forwards uncaught errors, unhandled rejections and `console.error` lines to the
host's app console. `A344Host` answers JSON-RPC 2.0 (single and batch; notifications without `id` get no
reply; parse / invalid-request / internal errors use the reserved codes):

| Method | Answer |
|--------|--------|
| `org.atsc.query.service` | `{service: globalServiceId}` (null without one) |
| `org.atsc.query.baseURI` | the absolute cache base URL |
| `org.atsc.query.deviceInfo` | make/model, `deviceInput` key codes (ArrowUp 38, ArrowDown 40, ArrowLeft 37, ArrowRight 39, Select 13, Back 27, BAAppear `{label: "App", keycode: 1001}`, digits 48–57), `deviceSupportedWebSocketAPIs` |
| `org.atsc.query.cc` | `ccEnabled` = a caption track is selected in the player |
| `org.atsc.query.alerting` | `alertList` from the LLS tables (`AEAT` XML; OMN/OSN with `receiveTime`), filtered by `alertingTypes` |
| `org.atsc.query.signaling` | LLS tables and the service's SLS fragments by `names` (`SLT`, `SystemTime`, `AEAT`, `USBD`, `S-TSID`, `MPD`, `HELD`, `DWD`, `MPT`, …), all when empty |
| `org.atsc.query.languages`, `captionDisplay`, `ratingLevel`, `audioAccessibilityPref`, `serviceGuideUrls` | fixed defaults (`en`, not blocked, empty) |
| `org.atsc.request.keys` / `relinquish.keys` | `{accepted: [...]}` from the key router (below) / `{}` |
| `org.atsc.subscribe` / `unsubscribe` | per connection; `All` covers the Table 9.31 list; replies with the current set |
| `org.atsc.scale-position` | the video gets `transform: translate(xPos%, yPos%) scale(scaleFactor/100)` from a reset box (origin top-left; scale before position, 9.7.2); range errors -32602, no player -8 |
| `org.atsc.setRMPURL` | `startRmp` attaches the MPD to dash.js (`-21` on the MMT push path, `-15` without `rmpurl`, `-11` when it cannot be attached), `resumeService` returns to the service's origin MPD, `stopRmp`/`pauseRmp` pause, `resumeRmp` plays; a banner shows the foreign source |
| `org.atsc.query.rmpMediaTime` / `rmpPlaybackState` / `rmpPlaybackRate` | `currentTime` (+ `startDate` when the element implements `getStartDate`), the 9.13.3 state (-1 initializing, 0 playing, 1 paused/stalled, 2 ended) and `playbackRate` of the `<video>` |
| anything else (cache requests, event streams, xlink, DRM, `launchApp`, `acquire.service`, vendor methods such as `org.atsc.query.MPDUrl`) | `-32601 Method not found` |

Notifications (`org.atsc.notify`) go to subscribed connections: `rmpMediaTimeChange` (1 Hz while the
time moves, with `playbackState`), `rmpPlaybackStateChange`, `rmpPlaybackRateChange`, and
`alertingChange` when the AEAT/OSN set changes between status versions. Every request, response,
notification, key event and application error is appended to the app console under the player (last 200
lines), which is the debugging surface of this harness.

**Keys (A/344 9.11, 9.12).** The host page owns every key. Sources: the virtual remote (`RemoteControl`:
App = BAAppear, CH+/CH−, D-pad with OK = Select, Back, the four colour keys, digits) and the physical
keyboard on the page (arrows, Enter = Select, Escape/Backspace = Back, digits, PageUp/PageDown =
channels, F2 or Insert = BAAppear; keys typed into text fields are left alone). `KeyRouter` keeps the set
granted by `request.keys`: `Numeric` is a group, `Enter`/`Escape`/`GoBack`/`Backspace` alias
`Select`/`Back`, the colour keys are `ColorF0Red`…`ColorF3Blue` (key codes 403–406), unknown names are
ignored, and `ChannelUp`, `ChannelDown`, the volume keys and `Power` are receiver-reserved and refused
(the 9.11.1 example). A key is delivered into the frame as synthetic `keydown` + `keyup`
`KeyboardEvent`s of the frame's own window with `key`, `code` and explicit `keyCode`/`which` (the
values `deviceInfo` promised: arrows 37–40, Select = `Enter` 13, Back = `GoBack` 27, BAAppear 1001,
digits 48–57; `isTrusted` is false, which navigation libraries do not check). Routing: BAAppear is the
key dedicated to the application — the receiver launches a ready application or shows a hidden one, and
the key is delivered whether or not it was requested; a granted key goes to the application; while the
application holds nothing beyond BAAppear (its launch screen, 9.11), the navigation keys the receiver does
not use (arrows, Select) are issued to it as BAAppear; everything else is a receiver key — Back hides the
application, ChannelUp/ChannelDown select the next/previous A/V service and navigate to its page, digits
typed within 1.5 s tune by major channel number, the rest is logged. Keys pressed while the iframe has
focus are seen first by capture listeners installed in its window (same origin): a trusted key the
application holds passes through untouched, any other is swallowed there and routed as above, so the rule
holds wherever focus sits. `requestKeyTimeout` is accepted as a subscription but never sent (grants do
not expire).

**Limits.** Only `atscCmd`; no media WebSockets, cache-request, event-stream, DRM, xlink, filter-code or
launch APIs; `validFrom`/`validUntil` and `requiredCapabilities` are shown, not enforced; captions
rendered by dash.js stay under the application (A/344 4.2.2 wants them on top); broadband-only HELDs are
listed with their link, never launched (no broadband fetching, 1.2); app-only services without a video or
audio component are not hosted (none in the corpus); the same-origin iframe can read the page's storage
and call the REST API — acceptable for applications from captures, an untrusted application would need
a second origin and a `postMessage`-only bridge. Media types from the envelope are canonicalised
(`application/js` → `text/javascript`, which Firefox otherwise flags). Verified: usc01's `uscapp`
launches and renders within a second of selection and answers through the shim (`e2e/apps.spec.ts`,
Firefox); the Sinclair WBFF `App.pkg` is launched once its 3.9 MB package has been assembled (≈126 s after
selection, measured for selection at 0 s and at 60 s of the BWI capture), connects to `atscCmd`, queries the service (its globalServiceID equals the
appContextId), device info, base URI and the AEAT (the "Testing alerting system" alert lands in its
persisted store), subscribes to `alertingChange`/`requestKeyTimeout`, loads its station configs and logo
from the cache, and then fetches its menu feed from `prod-ba-api-gateway.sinclairplatform.com` — without
broadband its React root stays empty, which is the application's design, not the harness's. Its key
handler reads `KeyboardEvent.code` and converts the `deviceInput.BAAppear` key code into its launch
command only on mobile receivers; on a TV its launch screen is driven by the feed.

### 5.19 VLC plugin integration (`atsc3://` demux module)

The second integration (4.1), under `integrations/vlc/` and enabled with `ATSC3_VLC=ON`: a VLC **demux module**
(`libatsc3_plugin.so`, C++17 behind a `extern "C"` module descriptor) that links the stack
statically, owns a source (pcap virtual tuner, `UdpSource` or an RF tuner) and turns the selected
service's components into VLC elementary streams. VLC then decodes, renders, selects tracks and
shows EPG with its normal UI, which makes "open the capture in VLC and watch channel 2.1" a complete
test of the receiver core without any browser. Like the web server, it consumes only the public API.

Implementation note: built and verified against the **VLC 3.0 line** — the distribution's SDK (`libvlccore-dev`
3.0.16 through `pkg-config vlc-plugin`); every VLC 3.0.x shares the `3_0_0f` plugin ABI, so the module also loads
into a VLC built from the 3.0.24 source. `demux.hpp` carries the `vlc_tick_t` / `VLC_TICK_*` aliases the 3.0 line
added after 3.0.16, so either SDK compiles it.
`integrations/vlc/src/`: `module.cpp` (descriptor, options, Open/Close), `demux.cpp` (programs, feeding,
timeline, controls), `es_mapper.cpp` (track → `es_format_t`), `epg.cpp`, `alerts.cpp`, `mrl.cpp` (VLC-free,
unit-tested). The deviations from the VLC 4 wording below are listed where they occur.

#### 5.19.1 MRL and options

```
atsc3://<path-to-capture>[?opts]              pcap/pcapng through the virtual tuner (5.1.2)
atsc3://udp@<group>:<port>[?iface=…][?opts]   live UDP (STLTP or plain IP multicast, auto-detected)
atsc3://tuner/<name>[?freq=…&plp=…][?opts]    RF tuner registered with the stack
```

Options (also available as VLC `--atsc3-*` module options and in the Preferences page):
`service=<id>` or `channel=<major.minor>` (initial program; default: first A/V service of the first
SLT, or the one named by `--program`), `bsid=<n>`, `pacing=realtime|nonrealtime` (default
realtime; nonrealtime is only useful with `--sout` dumps), `speed=<f>`, `loop=0|1`,
`start=<seconds>`, `sidecar=<file>|off` (DekTec `.xa3`, auto-detected by default), `esg=0|1` (feed VLC EPG, default 1), `alerts=osd|log|off`,
`push-captions=0|1`, `verbose-stats=0|1`.

#### 5.19.2 Mapping of stack objects to VLC

| Stack concept | VLC concept | Notes |
|---------------|-------------|-------|
| Broadcast stream / service list | one VLC input with **programs**: `es_out_Control(ES_OUT_SET_GROUP_META)` per service, group id = `serviceId` (bsid disambiguated by adding `bsid << 16` when several bsids are present) | services appear in *Playback ▸ Program*; `--program` and `service=` select; hidden/ESG/EA/DRM-data services are listed only with `show-hidden=1` |
| Service name, channel, globalServiceID, category, protected | program meta (`vlc_meta_Title`, `Description`, `URL`), `ES_OUT_SET_GROUP_META` | protected services are listed with "(DRM)" and play only their clear components |
| Component (video/audio/subtitle) | `es_out_Add` with `es_format_t` from the init segment (`hvcC`/`avcC`/`esds`/`dac4`/`stpp`), language and role from S-TSID/MPD/MP table, `i_group` = program | one ES per component; VLC's track menus do the switching; only ES of the selected program are fed |
| ROUTE media segments (CMAF) | per component a **chained demux** `vlc_demux_chained_New(obj, "mp4", es_out)` fed with `vlc_demux_chained_Send(block)`: init segment once, then each complete media segment as one block, in arrival order | VLC's `mp4` demuxer handles `moof/mdat`, `tfdt`, `trun`, `sidx`, `stpp`; the plugin wraps the child `es_out` to map child ES to the parent ES and to rebase timestamps (below) |
| MMT MPU fragments | same chained-demux path after the MSE-style normalisation of 5.17.4 (drop `mmpu`, add `styp`, continuous `tfdt`) | AC-4 (`dac4`) is exposed as an ES but VLC/libavcodec has no AC-4 decoder: shown as unsupported, video continues |
| Presentation time / clock | `es_out_SetPCR` from the stack clock (5.15) converted to `vlc_tick_t`; component timestamps rebased so that the first delivered fragment of the selected program maps to `VLC_TICK_0 + liveDelay` (default 2 × max segment duration); `EXT_ROUTE_PRESENTATION_TIME` / MPU timestamps used when present, else `tfdt` | all components share one timeline so VLC's A/V sync works; a PCR gap ≥ 1 s or an SLS timeline change (CP 5) triggers `ES_OUT_RESET_PCR` |
| ESG schedule/content (via the `esg` layer, 5.17.5) | `vlc_epg_t` per program through `ES_OUT_SET_GROUP_EPG` / `ES_OUT_SET_EPG_TIME` | VLC's EPG dialog shows now/next and the schedule; icons are not transported |
| AEAT alerts | `alerts=osd`: a text ES (`VLC_CODEC_TEXT`) with the alert text per language for its validity window; `log`: `msg_Warn`; `off` | wakeup bit and rich media are logged only |
| NRT files, HELD, app packages | not surfaced to VLC (logged with `verbose-stats`) | out of VLC's scope |
| Stats | `DEMUX_GET_META` extra fields and periodic `msg_Dbg` lines (bitrate, objects complete/incomplete, PLP, MMTP loss); exposed also in *Tools ▸ Media Information ▸ Statistics* through the input's stats where VLC allows | drives the e2e assertions |

The chained-demux approach is the recommended implementation because it reuses VLC's own
fragmented-MP4 code. The alternative, a self-contained CMAF/MPU demuxer that parses `moov/moof`
and emits samples directly with `es_out_Send`, is allowed if timestamp rebasing through chained
demuxers proves unreliable in the chosen VLC version (open question Q10).

Implemented: the **self-contained path**. Two chained `mp4` demuxers would each drive the program
clock from their own fragment timing and the plugin would have to interpose an `es_out` to reconcile them;
the media layer already parses what VLC's demuxer would, so `media::decoderConfigs()` (per track: the
`TrackInfo`, the sample entry to key the decoder by — the `frma` original format for `encv`/`enca` —, the
extradata as libavcodec takes it: `hvcC`/`avcC`/`dac4` payload or the AudioSpecificConfig out of the `esds`,
the stpp namespace, the trex defaults) and `media::fragmentSamples()` (the samples of a traf with absolute
byte offsets, dts from `tfdt` plus the preceding durations, composition offset, duration, sync flag) feed
`es_out_Send` directly. Mapping as built: `hev1`/`hvc1` → `VLC_CODEC_HEVC` with the hvcC as extradata and
`b_packetized` (libavcodec consumes length-prefixed NALs), `mp4a` → `VLC_CODEC_MP4A` with the ASC, `stpp` →
`VLC_CODEC_TTML` (one document per block), `ac-4` → the bare fourcc (listed, "no suitable decoder" in VLC 3),
`i_original_fourcc` = the entry 4CC, language from the MPD/MP table else the mdhd, the Representation id or
`pid N` plus the first role as description, "(encrypted)" appended for CENC tracks. Blocks: `i_dts`/`i_pts` on
the shared timeline, `i_length` = sample duration, `BLOCK_FLAG_TYPE_I` for sync video samples,
`BLOCK_FLAG_DISCONTINUITY` on the first block after a (re)anchor. Programs: `ES_OUT_SET_GROUP_META` per service
(title "major.minor name", "(DRM)" when protected, description protocol + category, URL = globalServiceID) and
`ES_OUT_SET_GROUP` when the plugin selects, otherwise VLC's es_out stays on the first program it saw and never
creates decoders for the one being fed; a second bsid appearing switches the numbering to `bsid << 16 | id`
with `ES_OUT_DEL_GROUP` of the old ids. EPG: `vlc_epg_t` per program from the esg layer's programme index,
the ESG Service joined by globalServiceID then major.minor (the BWI guide yields 458 events for the five A/V
programs; UniSoft's ESG matches nothing: its SLT carries no globalServiceID and its Service fragments no
channel numbers), `ES_OUT_SET_EPG_TIME` = the capture clock. Alerts: every new AEA (keyed by group, id, type,
effective time) is `msg_Warn`-logged with its English text; `alerts=osd` adds a `VLC_CODEC_TEXT` ES "ATSC alerts"
and sends the text for 15 s. Timeline: the first fed fragment anchors `media time → VLC_TICK_0 + liveDelay`;
in real time the PCR is the capture clock since the anchor (`Status::capture_unix_ns`), in non-real-time it is
the slowest track's position minus the live delay (data-driven, VLC paces nothing then); the live delay is
2 × the longest segment clamped to 1.5–2.5 s and `DEMUX_GET_PTS_DELAY` reports at least 1.5 s, because VLC's
decoders refuse timestamps more than pts delay + 3 s ahead of the last PCR (`input/clock.c`, "Timestamp
conversion failed": a 4 s lead on 2.002 s segments lost every audio block on 3.0.24); a fragment arriving
behind the clock or more than 30 s ahead re-anchors with `ES_OUT_RESET_PCR`, as does a loop wrap or a resume.

#### 5.19.3 Demux control behaviour

- `pf_demux`: drains the stack's delivery queue (bounded, filled on the pipeline thread) and feeds
  the chained demuxers; returns `VLC_DEMUXER_SUCCESS` while the source is alive, `VLC_DEMUXER_EOF`
  at end of a non-looping capture after the last objects are flushed (5.1.2). It never blocks longer
  than 100 ms waiting for data, so VLC stays responsive.
- `DEMUX_CAN_SEEK`/`CAN_CONTROL_PACE` are false (live semantics); `DEMUX_CAN_PAUSE` is true for the
  virtual tuner (pausing freezes the tuner's real-time clock and the stack clock together) and false
  for UDP/RF sources; `DEMUX_GET_TIME`/`GET_LENGTH` return the capture position and duration for files
  (so VLC shows a progress bar) and 0 for live sources; `DEMUX_SET_GROUP` (program change) deselects
  the old service, selects the new one and recreates the ES set; `DEMUX_GET_FPS` unsupported.
- Real-time pacing lives in the virtual tuner, not in the demux: VLC receives data at broadcast pace
  and buffers through its usual `--network-caching` (recommended 1500 ms, set as the module default
  for `atsc3://`). In non-real-time mode the demux applies back-pressure by sleeping when VLC's
  `es_out` reports more than `nonrealtime-buffer` (default 10 s) buffered, so memory stays bounded.
- Loop wrap and retune produce `ES_OUT_RESET_PCR` and a program-meta refresh; the ES set is kept if
  the components are unchanged.
- Errors from the stack (`onError`) are logged via `msg_Err/msg_Warn` with the layer and counters;
  object timeouts for the selected program are logged at debug level.

Implemented (VLC 3.0): the module registers the `access_demux` capability (VLC 3's name for a
stream-less demuxer selected by the URL scheme; VLC 4 merged it into `demux` with `s == NULL`) with
shortcut `atsc3` and score 10, and refuses `b_preparsing` opens so playlist probing never starts a tuner.
`pf_demux` polls the engine: on a state-version change it re-lists services, announces programs and picks the
initial one (`service=`/`channel=`, then VLC's `--program`, then the first non-hidden A/V service, bsid filter
applied); it then feeds the selected service — ROUTE through `Engine::media()`/`mediaObject()` (inits of every
video/audio/subtitle Representation first, then the segments in arrival order), MMT through `mmtMedia()`/
`mmtInit()`/`mmtFragment()` with `MmtFragmentInfo::start_s` as the first sample's time (the engine's
per-component timestamp offset already applied) and a generation change restarting the timeline — sends the
PCR, polls alerts once a second and the ESG when `esg_version` moved, logs a stats line every 5 s
(objects, bytes, object errors or MMTP packets, fragments and samples per track, capture position), sleeps
10 ms when nothing was fed, and returns `VLC_DEMUXER_EOF` 2 s after a non-looping capture finished with
nothing left to feed. Controls: `CAN_SEEK` false, `CAN_PAUSE` true (`SET_PAUSE_STATE` pauses the engine's
tuner; the timeline re-anchors on resume), `CAN_CONTROL_PACE` = non-real-time, `GET_TIME`/`GET_LENGTH`/
`GET_POSITION` from the capture position and duration (so `--run-time` counts capture seconds), `SET_GROUP`
switches service (ES dropped, engine deselect/select, tracks re-added as their inits arrive), `GET_META`
title = the capture file. Non-real-time back-pressure: the tuner is held (`Engine::pause`) while more than 6
segments wait and released below 2. Observed: with non-real-time pacing every engine call starved behind the
delivery thread, which takes and releases the engine mutex once per packet and re-took it before a woken
caller ran — one segment per second reached VLC; the engine now counts waiting API callers (`ApiLock`) and
its delivery thread sleeps 50 µs after a packet while one waits, after which a `--sout` dump of usc02 ran at
≈57× real time (114 s of media in 2 s).

#### 5.19.4 Build and packaging

`integrations/vlc/` targets the **newest released VLC, the 3.0 line** (decision Q10: the
4.0 line has no release; a port is a bounded follow-up — capability `demux` with a NULL stream,
`vlc_tick_t` everywhere, the reworked `es_out`). `cmake/FindVLCPlugin.cmake` takes `pkg-config vlc-plugin`
when an SDK is installed, else a VLC **source tree**: `VLC_SOURCE_DIR`, defaulting to the newest
`../vlc/vlc-*` beside the repository, whose `include/` holds the plugin headers (compiled with `__PLUGIN__`,
`MODULE_STRING="atsc3"`, `_FILE_OFFSET_BITS=64`, `_REENTRANT`, `_THREAD_SAFE`, included as system headers
because the first-party warning set does not fit VLC's C macros), and `libvlccore` from `VLC_BUILD_DIR/src/.libs`
(an out-of-tree build of that source) or the system. The VLC headers must not be wrapped in `extern "C"`:
they carry their own C++ linkage blocks and templates. Build: `cmake --preset vlc [-DVLC_BUILD_DIR=…]`,
target `atsc3_vlc_plugin` → `build/vlc/integrations/vlc/libatsc3_plugin.so` (the stack linked
in statically, 26 MB in the debug preset); install dir `${VLC_PLUGINSDIR}/access` (the pkg-config
`pluginsdir`, else `lib/vlc/plugins`); developer workflow `VLC_PLUGIN_PATH=build/vlc/integrations/vlc cvlc
atsc3:///abs/path/capture.pcap?service=5004`. Building VLC 3.0.x from source needs
`./bootstrap` (autotools present), a `src/revision.txt` (the tree is not a git checkout), a `configure`
with the unavailable optional libraries disabled (`--disable-lua --disable-dvbpsi --disable-qt …`, libavcodec
kept), and `make`; the in-tree `bin/vlc` loads plugins only from `VLC_PLUGIN_PATH`, and its scanner skips
libtool's `.libs` directories, so a flat directory of symlinks to the `*_plugin.so` files plus
`libatsc3_plugin.so` is what to point it at. The module registers `access_demux` (priority 10) with the
`atsc3` shortcut only; `.pcap` files opened without the scheme keep VLC's behaviour, and both `atsc3:///abs`
and `atsc3://abs` forms resolve (VLC's MRL split drops the leading slash of the latter).

#### 5.19.5 End-to-end tests with VLC

Two harnesses, both headless, run against the capture corpus with the plugin installed:

1. **`cvlc` log-driven** (`cvlc -vv --play-and-exit --run-time=30 atsc3://…?service=5004`): the test
   parses the log for the plugin's lines (`atsc3: programs: 6`, `atsc3: es added video hev1 …`,
   `atsc3: segment video-$TOI$ … complete`), VLC's decoder lines (`avcodec decoder: … hevc`), and
   `main input: … stats` (decoded video blocks, displayed pictures > 0, lost pictures), and fails on
   any `main input error` or `atsc3: error` line. Optionally `--sout '#std{access=file,mux=mp4,…}'`
   dumps the remuxed service and the dump is checked with the stack's own analyzers.
2. **python-vlc (libvlc) driven**: `MediaPlayer.play()` on the MRL, wait for `MediaPlayerPlaying`,
   assert `get_time()` advances ≥ 5 s in 10 s, `video_get_track_description()`/`audio_get_…` list
   the expected tracks (`hev1`, `mp4a`/`ac-4`, `stpp`), switch program with
   `--program=<id>` media options and repeat, pause/resume on a pcap source, and read `get_stats()`
   (`decoded_video`, `displayed_pictures`, `lost_pictures`).

| Capture | Scenario | Assertions |
|---------|----------|------------|
| `BWI_20230307_1742.pcap` | programs | 6 programs with correct titles/channels; WMAR (5004) auto-selected when `service=5004` |
| same | ROUTE playback WMAR, WMPT, WBFF | playing within 5 s, video decoded and displayed, audio `ac-4` listed as unsupported, `stpp` subtitle track present; no input errors over 60 s |
| same | WBAL (protected) | program listed as DRM; video ES added; decoder errors expected and counted, no crash |
| same | WNUV (MMT) | HEVC video plays through the normalised-MPU path; two audio tracks listed (en/es) unsupported; captions track present |
| same | EPG | `vlc_epg_t` populated for the four A/V programs (checked through the log and the python-vlc 4 EPG/program APIs) |
| `UniSoft-3xROUTE…` | usc01–usc03 | 4K HEVC + AAC play; program switching between the three within 3 s; IMSC captions on usc03 |
| `UniSoft-2xROUTE+1MMT…` | usc03m | MMT video plays; MP table assets mapped to tracks |
| small samples | open/close | plugin opens, lists programs, reaches EOF cleanly without playback |
| any pcap | controls | pause/resume keeps A/V sync; `loop=1` wraps without duplicate programs; `pacing=nonrealtime` with `--sout` finishes faster than real time and the dump decodes |

The VLC suite is the second acceptance test of Phases 1 and 2 (the web suite is the first); a
release must pass both.

Implemented: `integrations/vlc/tests/cvlc/run.sh <plugin-dir> <capture> <mrl-opts> <seconds>
<expected.json> [vlc]` runs `-I dummy -vv --vout dummy --aout dummy --play-and-exit --run-time` and
`check_log.py` matches the log against `tests/expected/*.json` (programs, the selected line, `es added`
substrings, decoder lines, `Received first picture`, per-track fragment counts from the last stats line, EPG
events, alerts, EOF, forbidden lines). `tests/pyvlc/libvlc_test.py` drives `libvlc.so.5` through ctypes
(python-vlc is not installed): play, wait for Playing, time advances ≥ 4 s in 6 s, track lists, media
statistics (decoded/displayed/lost pictures), pause/resume with a frozen clock, then a second player with
`:program=N`. `tests/e2e/run-vlc.sh` chains them (ctest `e2e.vlc`, skip code 77 without VLC or corpus,
≈5 min of real-time replays). Results against the corpus on VLC 3.0.16: usc02 passes (also on a VLC built from the
3.0.24 source), as do usc01 (4K 10-bit), usc03 (video, AAC, `stpp` captions: 12 TTML
samples), WMAR (HEVC decoded, two AC-4 tracks listed without decoder, captions, 458 EPG events, 3 alerts),
WNUV (the MMT path: `hev1.2.4.L93.B0` decoded, two AC-4 and the captions listed), WBAL (encrypted ES listed
with "(encrypted)", no input error), the three-broadcaster SLT sample (programs listed, EOF), the libvlc
scenario (usc02 and program 3 = usc03: ≈420 pictures decoded, 0 lost, pause/resume clean) and a
non-real-time `--sout` dump of usc02 (`mux=mp4`, 114 s of HEVC+AAC written in 2 s). Observed: 3.0.16 does
not log "Received first picture" for usc03 although libvlc reports the pictures decoded and displayed, so that
expectation is off for usc03; "buffer deadlock prevented" appears once per run while VLC fills its decoder
FIFOs during the initial buffering and is benign.

---

### 5.20 Optional GStreamer set-top-box player (`atsc3stb`)

The third integration (4.1), under `integrations/stb/` and enabled with `ATSC3_STB=ON` (preset `stb`): a Linux
program that plays a capture the way a set-top box does — the stack on the pcap virtual tuner (5.1.2), GStreamer
decoding of the tuned service's video and audio in a window, and a minimal STB user interface drawn on the video.
It changes channels within the current mux only; it has no track selection and does not run broadcaster
applications (5.18.4).

#### 5.20.1 User interface

- **Channel list**: the A/V services of the mux (ROUTE and MMTP services of category 1 or 2 or with a video or audio
  component; hidden services only with `--show-hidden`), sorted by channel number, as a vertical list on the left
  half of the screen with a highlighted row, a marker on the playing channel, "(DRM)" on protected services and a
  scroll indicator. Return (also `l`, Menu, Tab) opens it on the tuned channel; Up/Down move the highlight, Page
  Up/Down by a page (clamped), Return tunes the highlighted channel and closes the list, Escape closes it.
- **Channel banner**: shown for `--banner-seconds` (5 s) on every channel change and on `i`: channel
  number and name, the capture clock, the present programme with its window and a progress bar, the following
  programme, and the codecs of the tracks. While nothing plays a status ("Tuning…", "No decoder for ac-4.02.01.01")
  is part of the banner, or a small pill in the corner once the banner is gone.
- With the list hidden, Up/Down (and Page Up/Down) change channel with wrap-around, Escape hides the banner, `q`
  quits; closing the window quits.
- Present/following come from the ESG (5.17.5): the ESG Service is matched by globalServiceID, else by major.minor,
  and the instant is the capture clock (`Status::capture_unix_ns`). Observed: the UniSoft ESG names its services
  `urn:atsc:serviceid:r1`–`r3` with channels 3.1–3.3 while its SLT carries `urn:atsc:serviceid:usc01`–`usc03` and
  24.1–24.3, so that capture shows "No programme information"; the Sinclair capture matches by globalServiceID.
- The model (`src/model.*`: channel list from the service table, key handling, banner timer, present/following) is
  GStreamer-free and unit-tested (`[stb]`); `src/overlay.*` draws a `UiSnapshot` with cairo on every frame.

#### 5.20.2 Pipeline and timeline

- One persistent pipeline keeps the window and the audio device open across channel changes:
  `videotestsrc` (black, 60 fps) and the decoded video → `compositor` → `videoconvert` → `cairooverlay` (the UI) →
  `videoconvert` → video sink; `audiotestsrc` (silence) and the decoded audio → `audiomixer` → `audioconvert` →
  `audioresample` → audio sink. The sinks default to `autovideosink` / `autoaudiosink`; `--video-sink fakevideosink
  --audio-sink fakesink` runs headless. The silence source keeps the audio clock running when a service has no
  decodable audio.
- A channel change deselects the old service, selects the new one and drops every branch. As the engine delivers
  init segments (`Engine::media` representations, `Engine::mmtMedia` components) a branch per decodable video or
  audio track is added: `appsrc` with caps from `media::decoderConfigs()` (`video/x-h265` with the hvcC payload as
  `codec_data`, `video/x-h264`, `audio/mpeg` mpegversion 4 with the AudioSpecificConfig, AC-3/E-AC-3, `audio/x-ac4`
  named only so that the verdict can say "no decoder") → `decodebin` → convert and scale (video) or convert and
  resample (audio) → a request pad of the mixer (video: the full frame, zorder 1, aspect ratio kept). Tracks
  without a decoder (AC-4 everywhere), encrypted tracks and subtitles are reported and skipped. Hardware decoders
  (`GST_ELEMENT_FACTORY_TYPE_HARDWARE`) are skipped unless `--hw-decode`: the VA-API path produced garbage on the
  development machine while the software decoders handle 10-bit HEVC.
- Feeding is the VLC plugin's scheme (5.19.2): the samples of every fragment (`media::fragmentSamples()`) become
  buffers with DTS/PTS on one timeline whose media time is anchored to the pipeline's running time plus a live
  delay at the first fragment (`--live-delay`, default twice the segment duration within 1.5–2.5 s); MMT fragments
  use the engine's service-timeline start. Data behind the clock re-anchors (timestamps jump forward), data more
  than 30 s ahead restarts the branches on a fresh anchor; loop wraps and MMT generation changes restart them too.
- GStreamer facts the implementation depends on: a branch added while another branch's `decodebin` is still
  completing its asynchronous state change must be set PLAYING explicitly (the pipeline reports PAUSED and
  `gst_element_sync_state_with_parent` leaves the new branch's `appsrc` idle); a branch is torn down with its state
  locked, set to NULL, unlinked, its mixer pad released and then removed, otherwise the pipeline's state
  continuation restarts it; `appsrc` carries a `max-latency` (2 s) or the mixers' latency query fails; keys arrive as
  GstNavigation key-press events that the X11, GL and Wayland sinks send upstream, caught by a pad probe on the
  overlay and handed to the main loop.

#### 5.20.3 Program, build and tests

- `atsc3stb [options] <capture>`: `--service ID|major.minor` (the first channel otherwise; an unknown service
  falls back to the first channel after 15 s), `--size WxH` (1280x720), `--loop`, `--start S`, `--duration S`,
  `--banner-seconds S`, `--live-delay S`, `--sidecar auto|off|PATH`, `--show-hidden`, `--no-esg`, `--hw-decode`,
  `--video-sink`, `--audio-sink`, `--keys "Down@5,Return@6"` (GstNavigation key names at seconds, for tests) and
  `--verbose` (timeline, decoder choice, GStreamer warnings; with `GST_DEBUG_DUMP_DOT_DIR` set the graph is dumped
  once video flows). It prints one line per channel list, tune, track verdict and present/following change,
  per-tune statistics (video frames at the compositor, audio buffers at the mixer) and a final `summary` line;
  exit status 1 on a GStreamer error, 2 on bad arguments.
- Build: `pkg-config` for gstreamer-1.0 ≥ 1.20, gstreamer-app-1.0, gstreamer-video-1.0 and cairo; targets
  `atsc3_stb_model` (static, linked by `atsc3_unit`) and `atsc3stb` (`build/stb/integrations/stb/atsc3stb`, install
  component `stb`).
- Tests: the `[stb]` unit cases (channel list from the service table, keys with the list hidden and shown, banner
  timer, present/following, key names) and `tests/e2e/run-stb.sh <atsc3stb> <corpus>` (ctest `e2e.stb`, label
  `e2e-stb`, skip code 77 without the corpus or the plugins): headless real-time replays — UniSoft usc02 with Up and
  a list-driven tune back (three tunes, decoded frames on the first two channels, four keys, banners), BWI WNUV over
  MMT (frames, AC-4 reported without a decoder, a present programme from the ESG) and the small ALP sample (channel
  list, clean exit).

## 6. Data conversion and analysis tools

Needed because (a) the sample set covers only DLT 289, (b) tuners deliver two different units,
and (c) STLTP must be testable without a broadcast gateway. All tools are thin CLIs over the
library modules, so they double as integration tests. Each tool is its own program in its own directory
under `implementation/tools/` (Section 8); there is no umbrella binary with subcommands.

### 6.1 Tools

| Tool | Direction | Purpose | Priority |
|------|-----------|---------|----------|
| `atsc3scan` (implemented) | any pcap → report | Runs the virtual tuner's format detector and prints its verdict (ALP / BBP / STLTP / IP bypass / unsupported), then the inventory over the stack's own parsers: tuner and STLTP counters, BBP header facts per PLP (lengths, OFI, extension types, pointer classes), ALP packet types/layouts/SIDs/header extensions with the odd packets listed (zero length, trailing bytes), decapsulation counters, the LMTs, IP/UDP counters and the UDP flow table with byte counts, LLS tables (SignedMultiTable entries; `--slt` prints the SLT XML), LCT flows per TSI/codepoint/HET/header shape with close flags, MMTP flows per packet_id with payload and fragmentation statistics, message ids, `mmt_atsc3_message` content types and (`--mmt`) the MP tables and messages, unclassified UDP flows, and object completeness over every (flow, TSI, TOI) from the received byte ranges against EXT_TOL/EXT_FTI (`--objs` lists them, `--sls` prints the TSI 0 objects). `--limit N` scans the first N records, `--start`/`--duration` a time window; exit 3 when the capture carries no ATSC 3.0 content. `tools/lib/scan.cpp` is the scanner, `tests/golden/scan_test.cpp` asserts its numbers on the corpus (Section 9). Schema validation (5.7 item 7) is still open. Used to validate every new capture before it enters the test corpus | P1 |
| `ip2alp` | Ethernet/raw-IP pcap → DLT 289 pcap | Wrap each UDP/IP datagram in an ALP single-packet header (HM as needed); optional SID; drops non-IPv4 | P1 |
| `alp2bbp` | DLT 289 pcap → BBP container (6.2) | Pack ALP packets into BBPs for a chosen `Kpayload` (from A/322 Table 6.1/6.2 by code length/rate, or arbitrary), with configurable splitting, padding, counter extension, pointer=8191 cases, and optional A/322 scrambling. Produces the test vectors for the deframer | P1 |
| `bbp2alp` | BBP container → DLT 289 pcap | Exercises `BbpDeframer`; round-trip equality with the input of `alp2bbp` is a CI test | P1 |
| `bbp2stltp` / `alp2stltp` | BBP or ALP → STLTP pcap (Ethernet) | Synthesize inner RTP BBP streams (PT 78, ports 30000+plp, M bit + SSRC length, segmentation to MTU), a minimal Preamble (PT 77) and T&M (PT 76), and the outer PT 97 tunnel with `packet_offset`/marker; optional packet loss/reorder injection | P1 |
| `stltp2alp` | STLTP pcap or live multicast → DLT 289 pcap (+ BBP container) | Exercises `StltpDemux` + `BbpDeframer`; also the practical way to archive an STL feed. Implemented; `atsc3scan` reads its output like any DLT 289 capture and `tests/roundtrip` converts packed ALP back byte for byte | P1 |
| `alp2ip` | DLT 289 pcap → raw-IP pcap (DLT 101) | Lets Wireshark's IP/UDP/ROUTE/MMTP dissectors be used on ALP captures; also feeds path I4 tests | P2 |
| `routedump` (implemented) | pcap → directory tree | Runs tuner → (deframer/decap) → IP demux → LLS → per-service `RouteReceiver` and writes `<out>/bsid-<bsid>/<serviceId>-<name>/`: `manifest.mpd` (the broadcast MPD rewritten `type="static"` with only the captured segments, absolute BaseURLs dropped, `$Number$` templates narrowed to the longest contiguous run with `presentationTimeOffset`, `$Time$` templates given an exact SegmentTimeline), init and media segments named by Content-Location, `sls/` (every SLS fragment plus stable `usbd.xml`/`stsid.xml`/`mpd.xml`/`held.xml` aliases), `nrt/` (NRT files, gzip inflated; multipart packages also unpacked into `<name>.d/`), and `summary.txt`. MMTP services are listed as skipped. Verified: VLC 3.0.16 plays the WMAR manifest from the BWI capture; MPU dumping remains for the MMTP phase | P1 |
| `ts2alp` | TS file → DLT 289 pcap with packet_type 111 | Test vectors for TS encapsulation incl. null-packet deletion and header deletion | P3 |
| `pcapmerge` / `pcapfix` | pcap → pcap | Normalize timestamps (the samples have non-monotonic/absurd gaps), concatenate, cut by time | P2 |

### 6.2 BBP container format

No public link type exists for baseband packets. Define:

- pcap with link type **`LINKTYPE_USER0` (147)**; each record = `plp_id(8) flags(8) reserved(16)`
  followed by one complete BBP (header + payload). `flags` bit0 = scrambled, bit1 = error.
  pcapng alternative: one IDB per PLP with `if_description = "ATSC3 BBP PLP n"`, same record prefix.
- Writer/reader live in `atsc3::pcap` (`tuners/pcap`); the tuners for BBP-output demods may use the same
  writer for field captures.

---

## 7. Non-functional requirements

| Area | Requirement |
|------|-------------|
| Throughput | ≥ 60 Mbit/s of ALP input (a full 6 MHz channel) on one core of a 2 GHz ARMv8 or x86-64, with 2 services selected; pcap replay in `AsFast` ≥ 300 Mbit/s |
| Latency | Packet → application callback for a complete media segment: ≤ 2 ms processing overhead beyond object completion; MDE fragments ≤ 1 ms |
| Memory | Bounded: per object cap, per session cap (default 64 MiB), global cap with LRU drop of unselected NRT; no unbounded maps (table versions, TOIs, flows are capped with eviction) |
| Robustness | Fuzz-clean (libFuzzer targets for every parser); malformed input never causes UB, exceptions on the packet path, or stalls |
| Determinism | Same input file → identical event sequence (used for golden tests) |
| Portability | Linux (glibc/musl), Android NDK, macOS; big/little endian safe; no platform APIs in the core |
| Observability | Structured logging (`spdlog`-compatible sink interface), per-layer counters, `--trace` hex dumps |
| Configuration | One `Config` struct (JSON/YAML loadable), all timeouts/caps/toggles documented with defaults |
| Standards versions | A/330:2024, A/331:2024, A/322:2024, A/324:2024, A/360:2024, ISO/IEC 23008-1:2017, RFC 5651/5775/5445/6726/6330/3095 |

---

## 8. Project layout, build and dependencies

The implementation lives in `implementation/` next to `specs/` (standards, with the schema release in `specs/schemas/`) and `samples/`.
The PDFs and zips in `specs/` and everything in `samples/` are not in version control (root `.gitignore`); the root
`setup.sh` creates them (README "Inputs"); `specs/schemas/`, which the unit tests read, is unpacked by
`setup.sh --schemas` from the A/331 schema zip (5.16). The GitHub Actions workflows are at the repository root in
`.github/workflows/`, the only place GitHub reads them from: `ci.yml` runs `./setup.sh --schemas` before building so
the schema cases do not skip, and its `corpus` job fetches the captures with the repository secret
`DEKTEC_FTP_PASSWORD` for the golden, tools and `[corpus]` tests (fork pull requests get no secrets, so that job skips
its steps and stays green); `e2e.yml` is the nightly end-to-end run on a self-hosted runner.
The root `README.md` is the quick start — what the project is, how to build and run every
target and how to run the tests, with a Run section per program and integration — and changes in the same edit as
anything it names; the ctest entry `docs.readme` (`scripts/readme-check.py`, label `docs`, in the default test run)
fails when a preset, option, program flag, MRL key, test label or name, npm script, Playwright project or path it
names no longer exists, and when a program built under `tools/` or `integrations/`, a flag of its usage text or a
VLC plugin option is not named in it. Its prose is not checked.
Everything is driven by one CMake tree with presets; the frontend is an npm project that CMake can
invoke but does not require. The core (`src/`, `include/`) is what applications link, together with a tuner
implementation from `tuners/` (the pcap virtual tuner for replay, a vendor tuner on a device); `tools/` holds the
standalone programs and `tools/lib`, the `atsc3::tools` library they are built from and the tests link; the **integrations** under `integrations/` (the web server with its daemon and frontend, the
VLC plugin) are complete programs that embed the stack and present it through an interface of their own (4.1) —
they consume only the public API and are built only with their option (`ATSC3_WEB`, `ATSC3_VLC`, `ATSC3_STB`).

```
implementation/
├── CMakeLists.txt                  project(atsc3 CXX), options, adds every subdirectory below
├── CMakePresets.json               configure/build/test presets: debug, release, asan, ubsan, tsan,
│                                   coverage, fuzz (clang), web, vlc, full, embedded-arm64
├── cmake/
│   ├── Options.cmake               ATSC3_BUILD_TOOLS, ATSC3_BUILD_TESTS, ATSC3_TUNER_PCAP, ATSC3_TUNER_SONY, ATSC3_WEB, ATSC3_VLC, ATSC3_STB,
│   │                               ATSC3_SECURITY (OpenSSL), ATSC3_FRONTEND (npm build), ATSC3_FUZZ,
│   │                               ATSC3_SANITIZERS, ATSC3_COVERAGE, ATSC3_CORPUS_DIR (default ../samples)
│   ├── CompilerWarnings.cmake      -Wall -Wextra -Wpedantic -Werror, per-target atsc3_set_warnings(); test targets take
│   │                               atsc3_set_test_warnings(), without GCC's optimizer-only null-dereference/stringop warnings
│   ├── Sanitizers.cmake / Coverage.cmake / Fuzzing.cmake
│   ├── Dependencies.cmake          find_package(ZLIB, OpenSSL, PkgConfig) + FetchContent for pinned
│   │                               pugixml, Catch2, cpp-httplib, IXWebSocket (only when enabled)
│   ├── FindVLCPlugin.cmake         pkg-config vlc-plugin → imported target VLC::plugin, pluginsdir
│   ├── Packaging.cmake             CPack (deb/rpm/tgz), components: core, tools, atsc3d, vlc-plugin
│   └── atsc3Config.cmake.in        exported config for find_package(atsc3) consumers
├── third_party/                    vendored sources when FetchContent is not allowed (offline builds):
│   └── README.md                   pinned versions and licences; same names as Dependencies.cmake
├── include/atsc3/                  PUBLIC API only (what applications and plugins include)
│   ├── stack.hpp  config.hpp  event_sink.hpp  types.hpp  byte_view.hpp  result.hpp  stats.hpp
│   ├── io/        units.hpp (BBP/ALP/IP units, UnitTime, L1Info, LMT entries)  inet.hpp
│   ├── service/   engine.hpp  tuner.hpp (the tuner contract: ITuner, ITunerSink, TunerOutput, TunerState)  esg.hpp
│   ├── signaling/ slt.hpp  system_time.hpp  aeat.hpp  rrt.hpp  omn.hpp  usbd.hpp  stsid.hpp  mpd.hpp  held.hpp  dwd.hpp  apd.hpp  mmt_tables.hpp
│   └── media/     media_segment.hpp  mpu_fragment.hpp  nrt_file.hpp  component.hpp
├── src/                            the core library  →  target atsc3::core (static by default)
│   ├── CMakeLists.txt              one OBJECT library per layer, linked into atsc3core
│   ├── util/      bit_reader, buffer_pool, gzip, logging, clock, ntp, uri, base64, md5/sha (or OpenSSL)
│   ├── xml/       pugixml wrapper, namespace-tolerant lookup, xs:dateTime/duration/list parsing, raw-extension capture
│   ├── io/        delivery units shared by the tuner contract and the layers (units), IPv4/UDP/RTP helpers (inet)
│   ├── stltp/     ctp_outer, inner_reframer, bpps_reassembly, preamble_l1, tm_parser
│   ├── phy/       bbp_deframer, bbp_descrambler
│   ├── alp/       header, segmentation, concatenation, ts_encap, decap (dispatcher)
│   ├── link/      lmt, rdt, plp_map, rohc/ (decompressor, contexts, profiles 1/2/4, wlsb, crc)
│   ├── ip/        ipv4, fragments, udp, flow_router
│   ├── lls/       table_header, signed_multi_table, slt, system_time, aeat, rrt, omn, registry
│   ├── route/     lct (header + extensions + FEC payload id), efdt (+ file_template), multipart, session (object store, completion),
│   │              entity_mode, package_mode (multipart), session, channel, fec_interface
│   ├── sls/       multipart_parser, envelope, usbd, stsid, mpd_light, held, dwd, apd, rsat, sls_session
│   ├── mmtp/      packet_header, mpu_mode, signaling_mode, mp_table, mmt_atsc3_message, descriptors, mmtp_session
│   ├── service/   service_table, service_manager, component_model, capabilities, content_advisory,
│   │              esg_fragments (splitting only), ea, app_signaling, statistics
│   ├── esg/       OMA BCAST SG data model (Service/Schedule/Content with the A/332 extensions), programme index (5.17.5)
│   ├── security/  cdt_store, cms_verify (OpenSSL; compiled only with ATSC3_SECURITY)
│   └── stack.cpp  pipeline wiring, command queue, dispatch queue, IEventSink fan-out
├── tools/                          standalone programs (6.1), one directory per program, each a thin main over
│   │                               lib/; the converters still planned in 6.1 are added the same way
│   ├── lib/       atsc3::tools (namespace atsc3::tools, headers atsc3/tools/) — the programs' bodies the mains and
│   │              the tests call (scan.cpp, routedump.cpp, stltp2alp.cpp) plus the packet builders (IPv4/UDP,
│   │              Ethernet, SLL, RTP) and the ALP→BBP and BBP→STLTP packers; built when tools or tests are on,
│   │              links the core and tuner_pcap
│   ├── atsc3scan/ capture inventory CLI: format verdict, BBP/ALP/LMT facts, LLS tables, UDP flows, LCT flows with
│   │              object completeness, MMTP flows and signaling (--limit --slt --sls --objs --mmt); lib/scan.cpp
│   ├── stltp2alp/ STLTP pcap → DLT 289 ALP pcap (+ optional BBP container, PLP filter, time window)
│   └── routedump/ pcap → per-service directory tree: static manifest.mpd, segments, sls/, nrt/, summary.txt
├── tuners/                         tuner implementations (5.1): each a library over the core implementing service::ITuner
│   ├── pcap/      atsc3::tuner_pcap (ATSC3_TUNER_PCAP, on): the pcap virtual tuner (5.1.2) and the capture-file code it is
│   │              built on — pcap/pcapng reader+writer, link-type strip, format detector, sidecar_xa3, bbp_container
│   └── sony/      Sony demodulator tuner (format TBD, Q1); build with ATSC3_TUNER_SONY=ON
├── integrations/                   programs that embed the stack and present it through their own interface (4.1);
│   │                               each consumes only the public API and is built only with its option
│   ├── web/       ATSC3_WEB — atsc3::web (5.17): web.cpp json.cpp server.cpp, include/atsc3/web/ — http server
│   │   │          (cpp-httplib), REST handlers, local DASH origin, MMT push origin, app origin, static files
│   │   ├── atsc3d/    daemon: stack + source selection (pcap/udp/tuner) + the web server + CLI flags (--no-web = headless)
│   │   └── frontend/  Vue 3 + Vite + TypeScript (5.18); built by npm, through CMake with ATSC3_FRONTEND
│   │       ├── package.json  vite.config.ts  tsconfig.json  playwright.config.ts
│   │       ├── src/   api/ (types.ts generated by scripts/gen-ts-types.py, rest client), stores/, player/ (dash.js,
│   │       │          MsePushPlayer), a344/ (receiver host, key router), views/, components/, router/, App.vue, main.ts
│   │       ├── public/a344-shim.js
│   │       └── e2e/   Playwright specs (discovery, playback, epg, alerts, source, apps)
│   ├── vlc/       ATSC3_VLC — atsc3_vlc_plugin → libatsc3_plugin.so (5.19, VLC 3.0 line)
│       ├── CMakeLists.txt         FindVLCPlugin, install to ${VLC_PLUGINSDIR}/access
│       ├── src/   module.cpp (descriptor, options), demux.cpp (programs, feeding, timeline, controls), es_mapper.cpp,
│       │          epg.cpp, alerts.cpp, mrl.cpp (VLC-free, unit-tested)
│       └── tests/ cvlc/ (run.sh + check_log.py log matchers), pyvlc/ (libvlc ctypes scenario), expected/*.json
│   └── stb/       ATSC3_STB — atsc3stb (5.20, GStreamer 1.20 + cairo): the set-top-box player
│       ├── CMakeLists.txt         pkg-config gstreamer-1.0/-app/-video + cairo; atsc3_stb_model (unit-tested), atsc3stb
│       └── src/   model.cpp (channel list, keys, banner, present/following; GStreamer-free), player.cpp (pipeline,
│                  branches, timeline), overlay.cpp (cairo drawing), main.cpp (options, polling, scripted keys)
├── tests/
│   ├── CMakeLists.txt              registers ctest labels: unit, golden, roundtrip, tools, http, fuzz, bench, docs, e2e-web, e2e-vlc, e2e-stb;
│   │                               docs.readme runs scripts/readme-check.py over the root README.md
│   ├── support/   test_main.cpp, pcap_builder, packet_builders (alp/bbp/lct/mmtp/stltp), fake_tuner,
│   │              event_recorder (serializes IEventSink calls), corpus.hpp (ATSC3_CORPUS_DIR lookup)
│   ├── unit/      one dir per layer mirroring src/ (io, stltp, phy, alp, link, rohc, ip, lls, route,
│   │              sls, mmtp, service, security, esg, web) — Catch2, one executable `atsc3_unit`
│   ├── vectors/   hex/JSON test vectors: standards' worked examples, bytes from Appendix A/B,
│   │              schema examples referenced from ../../specs/schemas (not copied)
│   ├── golden/    expectations/<capture>.json (event logs, counts) + runner `atsc3_golden`;
│   │              skipped automatically when ATSC3_CORPUS_DIR is absent
│   ├── roundtrip/ converter round-trip tests (ip2alp|alp2ip, alp2bbp|bbp2alp, alp2stltp|stltp2alp)
│   ├── fuzz/      libFuzzer targets per parser (alp, bbp, stltp, lct, lls, mmtp, xml fragments, multipart) + seed corpora
│   ├── bench/     throughput benchmarks (synthesized 60 Mbit/s stream, pcap replay AsFast)
│   ├── http/      REST/SSE contract tests against atsc3d without a browser
│   └── e2e/       run-web.sh (starts atsc3d, runs frontend Playwright), run-vlc.sh (cvlc + python-vlc)
├── scripts/
│   ├── build-vlc4.sh              fetch + build VLC 4 (libvlccore, headers) for plugin CI
│   ├── gen-ts-types.py            public API → TypeScript types for frontend/src/api
│   ├── format.sh  lint.sh         clang-format / clang-tidy / eslint / prettier drivers
│   ├── corpus-check.sh            verifies ../samples presence + checksums before golden/e2e runs
│   ├── readme-check.py            the root README.md against presets, options, flags, labels, paths, program coverage (ctest docs.readme)
│   └── ci/                        pipeline entrypoints (build-matrix.sh, run-golden.sh, run-e2e.sh) called by the
│                                  workflows in the repository root's .github/workflows/ (ci.yml, e2e.yml)
└── .clang-format  .clang-tidy  .editorconfig  .gitignore  .gitattributes (pcap = binary)
```


Build system rules:

- `CMakeLists.txt` requires CMake ≥ 3.21 (presets v3), C++17 without extensions, and defines the
  options in `cmake/Options.cmake`. Default build: core + tools + tests. `-DATSC3_WEB=ON`,
  `-DATSC3_VLC=ON`, `-DATSC3_STB=ON`, `-DATSC3_FRONTEND=ON` add the optional parts.
- Targets and namespaces: `atsc3::core` (every `src/` layer, `esg` included), `atsc3::tuner_pcap`, `atsc3::tools`, `atsc3::web`,
  executables `atsc3d`, `atsc3scan`, `stltp2alp`, `routedump`, `atsc3stb`, `atsc3_vlc_plugin`, test executables
  `atsc3_unit`, `atsc3_golden`, `atsc3_roundtrip`, `atsc3_http`, fuzz targets `fuzz_<parser>`.
- Each `src/<layer>` is an OBJECT library with its own `CMakeLists.txt` so a layer can be built and
  unit-tested in isolation; only `include/atsc3` is public, everything under `src/` is private.
- Corpus: `ATSC3_CORPUS_DIR` (default `${CMAKE_SOURCE_DIR}/../samples`) and
  `ATSC3_SCHEMA_DIR` (default `../specs/schemas`); golden, round-trip and e2e tests are registered with
  ctest labels and skip with a clear message when the corpus is missing.
- Presets: `cmake --preset debug && cmake --build --preset debug && ctest --preset debug`;
  `ctest --preset debug -L unit`, `-L golden`, `-L e2e-web`, `-L e2e-vlc`; a single Catch2 case:
  `build/debug/tests/unit/atsc3_unit "[lct]"` or `-c "parses EXT_TOL 24-bit"`.
- The frontend builds with `npm ci && npm run build` in `integrations/web/frontend/`; with `ATSC3_FRONTEND=ON`
  CMake runs it and `atsc3d` serves `integrations/web/frontend/dist` by default. Playwright runs via
  `tests/e2e/run-web.sh <atsc3d> <corpus>` (`ATSC3_E2E_PORT`, default 18080; `PLAYWRIGHT_PROJECT`, default chromium).
- The VLC plugin (`integrations/vlc`) builds against the VLC 3.0 plugin headers: `cmake --preset vlc` takes
  `pkg-config vlc-plugin` or a source tree beside the repository (5.19.4).
- Installation exports `atsc3Config.cmake` so external applications use `find_package(atsc3)` and
  link `atsc3::core`; CPack components package core, tools, daemon and the VLC plugin separately.

Dependencies (all optional except zlib and the XML parser):

- zlib (gzip/deflate), **pugixml** (XML; expat acceptable), Catch2 (tests), libxml2/xmllint
  (test-time schema validation only), cpp-httplib and IXWebSocket (only with `ATSC3_WEB=ON`),
  Node 20 / Vite / Vue 3 / Pinia / dash.js / Playwright (frontend and e2e only, never linked into the stack),
  libvlccore and the plugin headers from the VLC 3.0 line (`pkg-config vlc-plugin` or the source tree, 5.19.4; only with
  `ATSC3_VLC=ON`), GStreamer 1.20 (gstreamer-1.0, gstreamer-app-1.0, gstreamer-video-1.0) and cairo (only with
  `ATSC3_STB=ON`; the base, good, bad and libav plugins at run time),
  OpenSSL ≥ 1.1 (security module), libpcap (live capture only), a RaptorQ library (Phase 3).
  ROHC-U is implemented in-house (5.5.2); no external ROHC library.
- No Boost. No C++20 features (std::span, ranges, coroutines).

Coding standards: `clang-format` (LLVM base, 100 cols), `clang-tidy` core checks, every parser
function documents its spec table, `[[nodiscard]]` on `Result`, no raw `new`.

---

## 9. Test strategy

1. **Unit tests with hex vectors** per header type (ALP all four configurations and the TS form,
   BBP all OFI modes, LCT with each HET, LLS header, SignedMultiTable, MMTP MPU/signaling
   headers, STLTP RTP header). Vectors come from the samples (Appendix B) and from the standards'
   worked examples (A/322 pointer=130 → `10000010 00000100`; A/331 Annex C TOI 0x80470003).
2. **Golden pcap tests** (`tests/golden/`): run each sample through the stack and compare an event
   log (tables, versions, objects with sizes and MD5, flows, counters) with a checked-in
   expectation. Expected values for the current samples are those listed in Section 3 and
   Appendix B.
3. **Round-trip tests**: `alp2bbp|bbp2alp`, `alp2stltp|stltp2alp`, `ip2alp|alp2ip` must reproduce
   the input packet sequence byte-for-byte (modulo timestamps).
4. **Fault injection**: loss, reordering, duplication and truncation applied in the sources;
   assertions on no-crash, correct `ObjectError` reporting and recovery after the fault window.
5. **Fuzzing**: libFuzzer harness per parser, seeded from the samples; run in CI for a fixed budget.
6. **Performance**: benchmark binaries over a synthesized 60 Mbit/s stream (looped samples).
7. **Schema tests**: every example instance in `specs/schemas/` parses into the expected typed
   structure; parsers are run against `xmllint --schema` output as an oracle for well-formedness
   and schema conformance (external schemas supplied by the CI image); ABNF grammars are covered
   by positive/negative string tables (content-advisory strings, User-Agent, capability expressions).
8. **Virtual tuner tests**: format detection on every file in `samples/` (five → ALP mode), on a synthetic
   LAN-chatter capture (→ Unsupported, and `atsc3scan` exit code 3 on it) and on converter outputs (`alp2ip` →
   IP bypass, `alp2stltp` → STLTP, `alp2bbp` → BBP; `BWI_20230307_1742.pcap` → STLTP; both UniSoft
   captures → IP bypass); mixed-interface pcapng; real-time pacing
   accuracy (delivery times vs. record timestamps within ±5 ms p99, clamped gaps, negative gaps,
   `speed` 0.5/2.0); non-real-time mode produces an event log identical to real-time mode except
   for host timestamps; loop/wrap resets; end-of-file flush delivers the last objects.
9. **End-to-end tests** (5.18.3): Playwright against `atsc3d` + virtual tuner over the capture corpus;
   also the web server's REST/SSE contract is covered by HTTP-level tests without a browser.
10. **VLC plugin tests** (5.19.5): `cvlc` log-driven and python-vlc driven playback of the corpus
   through the `atsc3://` demux module; both this suite and the web suite must pass for a release.
11. **Set-top-box player tests** (5.20.3): the `[stb]` unit cases of the UI model and `tests/e2e/run-stb.sh` (ctest
    `e2e.stb`): headless real-time replays with scripted keys, checked for decoded frames on every tuned channel,
    the banner and the present programme from the ESG.

Large-capture golden expectations (first 320 000 tunnel packets of the BWI feed, ~144 s): 73 046 BBPs
(PLP 0: 3424, PLP 1: 17 120, PLP 2: 52 502), 282 642 ALP packets (280 930 IPv4, 1712 LMT), 2333
complete / 26 incomplete ROUTE objects, 33 018 MMTP packets on WNUV, no sequence gaps. UniSoft
3xROUTE (first 120 000 packets): 296 complete / 15 incomplete objects; UniSoft 2xROUTE+1MMT (first
120 000 packets): 142 / 5 objects and 49 108 MMTP packets. These numbers, and the inventory of every small
sample, are asserted by `tests/golden/scan_test.cpp` through `atsc3scan`'s scanner (`--limit N` is the record
window), the regression baseline for the tuner, deframer, decapsulation, demux and the LLS/LCT/MMTP parsers.

Known gaps in the corpus to be filled by new captures (Section 11): BBP from a BBP-output tuner,
ROHC-compressed flows, RaptorQ repair flows, STLTP with ST 2022-1 FEC or signing, DASH with
EventStream/`emsg`, AEAT with rich media delivered over broadcast, a hybrid MMT service carrying an MPD.

---

## 10. Delivery phases

| Phase | Content | Exit criterion |
|-------|---------|----------------|
| 1 | Pcap virtual tuner (format detection, IP bypass, real-time and non-real-time pacing, loop), UdpSource, BBP container, BbpDeframer, StltpDemux (no FEC), AlpDecap (all types, TS pass-through), LMT/RDT parse, IpDemux, LLS (all tables), ROUTE source flows with all completion strategies, ROUTE SLS, Service manager (ROUTE services, ESG/EA/NRT delivery), API, tools P1, golden tests for all five ALP samples; **web server + frontend minimal**: status, services, select, ROUTE playback via dash.js, source panel, signaling view; **VLC plugin minimal**: `atsc3://` pcap MRL, programs, ROUTE playback, pause | All five ALP samples replay with expected event logs; `alp2bbp`/`alp2stltp` round trips pass; fuzz clean; e2e (web and VLC): BWI discovery + WMAR playback + UniSoft playback pass |
| 2 | MMTP (MPU + signaling + mmt_atsc3_message), MDE/fragment delivery, in-house ROHC-U decompressor, security module, tools P2, A/337 events; **web/frontend**: MMT push player, EPG (esg layer), alerts, stats, link views; **VLC plugin**: MMT path, EPG, alerts OSD, UDP/RF MRLs | WNUV MMTP flow in samples decodes to MPU fragments; new MMTP capture with MP table decodes end-to-end; e2e (web and VLC): WNUV and usc03m playback, EPG and alert scenarios pass |
| 3 | RaptorQ repair flows, STLTP ST 2022-1 FEC, MMTP AL-FEC, file repair helper, ts2alp, Android/embedded tuning | Repair-flow capture recovers objects under injected loss |

---

## 11. Open questions and assumptions

- **Q1 Tuner contract (decided, format open).** The first RF target is a **Sony demodulator**; its
  output format (BBP, ALP, or ALP wrapped in a vendor framing such as 188-byte TS-style packets) is
  not yet known. The tuner contract already accepts BBP or ALP (5.1); the Sony tuner is written
  once the driver documentation is available and, if the output is framed, it unwraps the framing
  before calling `onAlp`/`onBbp`. Assumptions until then: BBPs include their header and are
  descrambled. Action: obtain the Sony driver/API description and the first field capture from it.
- **Q2 PLP id for ALP pcaps (closed, unknown).** No marker format is known for the private HEF
  0xF0/0xF1 packets in the small samples. DLT 289 captures therefore carry `kUnknownPlp`; the
  marker packets stay opaque events; a DekTec sidecar (Q8) or the LMT can still supply PLP
  information. `plp_marker_format` is a configuration hook with no built-in formats.
- **Q3 MMTP v1 header (resolved empirically).** Two independent encoders always carry the 16-bit
  QoS/flow word after `packet_counter`; the stack parses it unconditionally for version 01 (5.10.2).
  The remaining uncertainty is the meaning of byte-0 bits 1 and 0, which the stack exposes as raw
  flags and does not depend on. A check against ISO/IEC 23008-1:2017 Table 9-1 is still desirable.
- **Q4 ROHC library (resolved).** In-house ROHC-U decompressor, profiles 1/2/4, U-mode only (5.5.2).
  No external library.
- **Q5 Signature policy (resolved).** Never block; signatures are verified when the security module
  is enabled and the result is reported only (5.14).
- **Q6 ESG parsing depth (resolved).** The core delivers the fragments *and* the OMA BCAST SG data model, as the
  core layer `src/esg` (5.17.5), so that every consumer — the web EPG, the VLC EPG, a middleware — gets the same
  parsed guide from one place. A host with its own EPG database that only wants the raw fragments has them through
  `Engine::esg()`.
- **Q7 Existing API compatibility (resolved).** No earlier API is carried over and no compatibility adapter is
  built; the Application API of 5.12 is the only one.
- **Q9 Browser codec coverage for e2e (resolved).** Both: headless Chromium in CI asserts at the
  MSE-append level, and a self-hosted runner with hardware HEVC decoding runs the playback
  assertions (5.18.3). AC-4 audio stays flagged unsupported in browsers.
- **Q10 VLC target and demux strategy (resolved).** The newest *released* VLC is the 3.0 line; no 4.0 release
  exists, so the plugin targets 3.0 (`access_demux` capability, `vlc_tick_t` as the 3.0 line defines it) and a
  VLC 4 port is a bounded follow-up. The
  demux strategy is the in-plugin CMAF/MPU path on top of `media::decoderConfigs()` / `fragmentSamples()`
  rather than chained `mp4` demuxers: one clock, no `es_out` interposition (5.19.2).
- **Q8 Sidecar formats (resolved).** DekTec `.xa3` profiles are supported as the source of `L1Info`
  and synthesized LMTs for captures without a link layer (5.1.2, rule 8). No other vendor sidecar
  format is known or planned; any future one gets its own reader behind the same interface.
- **Q11 Push transport (decided).** The MMT/MSE push path of 5.17.4 is delivered over plain HTTP
  (`index.json` long-ish polling at 500 ms plus GETs of the reconstructed fragments) instead of the
  binary WebSocket: same origin and port as the ROUTE origin, no IXWebSocket dependency, works through the
  Vite dev proxy, and MSE needs whole fragments anyway. The WebSocket (and the ROUTE `?mode=push` variant)
  stay optional optimisations.
- **A1 (confirmed).** Non-ATSC captures are not inputs for the stack; the negative detection test (virtual tuner
  verdict "Unsupported", `atsc3scan` exit code 3) runs on a synthetic LAN-chatter capture.
- **A2 (confirmed).** IPv6 is not required (A/330 LMT is IPv4-only; A/331 allows IPv6 for ROUTE but
  no US broadcast uses it). The IP layer rejects IPv6 with a counter; adding it later is local to 5.6.

---

## Appendix A: Bit-level quick reference

### A.1 ALP base header (A/330 Table 5.1) — examples from samples

| Bytes | Meaning |
|-------|---------|
| `05 4B` | type 000 (IPv4), PC 0, HM 0, length 0x54B = 1355 → IPv4 packet follows |
| `05 DC` | IPv4, length 1500 |
| `08 00 05 F1 00 00` / `08 00 05 F1 00 04` | IPv4, PC 0, HM 1, length 0; add. header 0x05: length_MSB 0, SIF 0, HEF 1; ext type 0xF1, extension_length_minus1 0, 1 byte `00`/`04` (vendor marker) |
| `08 00 05 F0 07 d8 ed 13 0f dd 4b a6 e4` | as above with ext type 0xF0, 8 bytes |
| `80 xx ..` | type 100 link-layer signaling (LMT when signaling_type 0x01, ext 0xFFFF, format 00) |

### A.2 LLS datagram (A/331 Table 6.1)

`UDP 224.0.23.60:4937` → `id gid cnt ver` + gzip. Example: `04 01 00 26 1F 8B 08 08 …` = AEAT,
group 1, one group, version 38, gzip with embedded filename "AEAT". SignedMultiTable example:
`FE 00 00 00 | 02 | 01 00 01F1 <497 B gzip SLT> | 03 02 00B1 <SystemTime> | 0298 <664 B CMS>`.

### A.3 LCT header (RFC 5651) — examples from samples

| Hex | Decoded |
|-----|---------|
| `12 A0 05 80 00000000 0000001E 007BFDCA C200053A` | V1 C0 PSI 10; S1 O1 H0 A0 B0; HDR_LEN 5; CP 128; CCI 0; TSI 30; TOI 8125898; EXT_TOL(194) = 1338 |
| `12 A1 05 80 …` | same with B=1 (close object) |
| `12 A0 08 08 00000000 0000012C 00000001 4004 000000000255 0000 0000 00000000` | HDR_LEN 8; CP 8; TSI 300; TOI 1; EXT_FTI HEL 4: TL 597, reserved, ESL 0, MSBL 0 |
| `12 A0 04 08 …` | HDR_LEN 4; CP 8; no extensions (length from EFDT or carousel) |
| `12 A0 05 01 …` | CP 1 (SLS / NRT file) |

FEC payload ID (`start_offset`) is the 32-bit word after the header; payload follows.

### A.4 MMTP (see 5.10.2) and STLTP RTP (A/324 Table 6.1)

```
RTP: V(2)=2 P X CC(4)=0 | M PT(7) | sequence(16) | timestamp/timestamp_min(32) |
     [PT 97: protocol_version(2) redundancy(2) number_of_channels(2) reserved(10) packet_offset(16)]
     [PT 81/82: protocol_version(2) reserved(14) packet_offset(16)]
Inner BBP stream: PT 78, M=1 on first packet of a BPPS, SSRC = total BBP length, dst 239.0.51.48:30000+plp
```

### A.5 BBP header (A/322 Figure 5.4)

`MODE(1) ptr_lsb(7) [ptr_msb(6) OFI(2)] [EXT_TYPE(3) EXT_LEN(5) [EXT_LEN_MSB(8)]] [ext…]`;
pointer 8191 = no ALP start; `10000010 00000100` = pointer 130, OFI 00.

---

## Appendix B: Constants observed in the samples

| Item | Values |
|------|--------|
| LLS groups / source IPs / bsids | group 0 ← 10.133.16.20 (bsid 564, services WESH 2.1, WKCF 18.1, WUCF 24.1, SG-18 ESG); group 1 ← 172.16.200.1 (bsid 1409: WBFF 45.1, WBAL 11.1, WMAR 2.1, ESG 5005, WMPT 22.1, WNUV 54.1 **MMTP**); 10.4.49.5 (bsid 668) |
| SLS flows | 239.255.2.1:5021, 239.255.18.1:5181, 239.255.24.1:5241, 239.255.2.255:8000 (ESG), 239.255.45.1:5002, 239.255.11.1:5003, 239.255.2.1:5004, 239.255.54.1:5100 (ESG), 239.255.22.1:5006, 239.255.54.1:5001 (MMTP) |
| LMT content | up to 3 PLPs, 56 multicasts, sources 0.0.0.0 or explicit; SID/compressed flags never set |
| ROUTE codepoints | 128 (media, EXT_TOL), 8 (media, EXT_FTI or none), 1 (SLS, EXT_TOL), 3 (NRT packages on high TSIs 55243/82472/83358/83546) |
| LCT HDR_LEN | 4 (no ext), 5 (EXT_TOL 24-bit), 8 (EXT_FTI HEL 4) |
| TSI values | 0 (SLS/EFDT), 1, 2 (ESG), 10/20/30 (media), 100/300 (media), 5-digit NRT |
| Object sizes | SLS 320–4799 B; init segment 597 B; media segments 1338 B … ~250 kB (span 253384 on TSI 100) |
| MMTP | version 01, packet_counter present, QoS classifier `A4 00`, pids 100 and 200, FT 2 MFUs with timed DU headers, MPU seq 0x639acc63/64 |
| Private flows | RTP PT 33 (MPEG-TS) on 239.255.54.201–215:5000 from 172.18.19.75; RTP PT 96 on 239.255.54.212:5002/5004 from 10.11.82.75; declared in LLS UserDefined `UDST` (digicap namespace) |
| ALP anomalies | zero-length + HEF 0xF1 (`00`/`04`) ×9, HEF 0xF0 (8 B) ×2, one record truncated by 2 bytes (small samples only; none in the STLTP-derived stream) |
| STLTP (BWI) | outer 192.168.59.100:58532 → 239.232.11.51:30000, PT 97, `80 61 38 80 1e f1 07 b2 00 00 00 00` (M=0, seq 0x3880, timestamp, protocol_version 0, packet_offset 0); inner 239.0.51.48:30000–30002 PT 78, :30064 PT 77, :30065 PT 76; BBP 5916 B, OFI 10, EXT_TYPE 111 |
| MMTP (UniSoft) | 239.255.24.3:8000, byte0 0x40, pid 0 signaling / 20 `hev1` / 21 `ac-4`, package `usc03m-1`, MPT 0x11 + subsets 0x12/0x13, HRBM 0x0204, HRBM removal 0x020A, `mmt_atsc3_message` USBD gzip with URI `usbd.xml` |
| MMTP (WNUV) | pid 0 signaling (MPT 0x20, `mmt_atsc3_message` 0x0001/0x0003/0x0005/0x0008/0x0009), 100 `hev1` (timescale 90000), 200 `ac-4` en, 201 `ac-4` es, 300 captions (`stpp`), subset MPTs 0x12–0x15 |
| ROUTE (WBAL/WMAR/WMPT/WBFF) | TSI 100 video `Video1_1` (`video-$TOI$.mp4v`, init TOI 1), 200/201 audio `a02_2`/`a13_3`, 300 subtitles `d4_4`; CP 8 + EXT_FTI; SLS TOI 0x700DA/DB; EFDT `Content-Location="SLS"` |
| ESG (service 5005) | 239.255.54.1:5100 TSI 50 SGDD `sgdd_1919` (6 kB gzip → 61 kB), TSI 60 SGDU `sgdu_content_3378` (44 kB → 450 kB), `sgdu_service_schedule_4153`, TSI 80 PNG icons; CP 1 |
| NRT app packages | 239.255.54.1:15007 TSIs 91437/93782/94193/94863/94940: `single-alert.pkg`, `App.pkg` (3.8 MB), `Alert.pkg`, … `multipart/related`, CP 3, `appContextIdList="tag:sinclairplatform.com,2020:WNUV:1409"` |
| HELD apps | WBFF `tag:sinclairplatform.com,2020:WBFF:1407` → `App.pkg` 3880784 B / `index.html`, bband cdn.sinclairplatform.com, coupled 5002; WNUV `…:WNUV:1409` via MMT 0x0003 + USBD `ROUTEComponent sTSIDUri="stsid.sls"` 239.255.54.1:15007 ← 172.16.200.1 (`App.pkg` 3786958 B); WMAR `tv:ota-wmar.ewscloud.com` broadband only; UniSoft usc01 `uscapp` → `uscapp.zip.multipart` TSI 4 TOI 10000 CP 130 / `uscapp/home.html` |
| UniSoft ROUTE | 239.255.24.1–3:8000 TSI 1 video (`test-0-$TOI$.m4pv`, init TOI 2100000000, `maxTransportSize` 1.9 MB), 2 audio, 3 captions `stpp.ttml.imt1`, 4 app package CP 130 `uscapp.zip.multipart`; ESG 239.255.24.99 TSI 999 `atsc3sgdd`, 1000 `guideFragments`/`serviceFragments`; MPD `isoff-live:2011`, `$Number$`, `startNumber` 1/3 |
| XML variants | CRLF; `tag:atsc.org,2016:` and `http://www.atsc.org/` SystemTime namespaces; `xsi:schemaLocation` present on one SLT; schema examples add a UTF-8 BOM, prefixed `slt:` elements and `?????` placeholder strings |
