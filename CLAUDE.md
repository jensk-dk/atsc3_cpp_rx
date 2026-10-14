# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

An ATSC 3.0 client-side (receiver) protocol stack in C++17: the specification and its implementation.
The repository holds:

- `ATSC3_CLIENT_STACK_SPEC.md` — the specification (currently v0.27). It is the source of truth for
  architecture, module behaviour, ingest formats, the pcap virtual tuner, the optional web server /
  Vue 3 frontend and VLC plugin integrations, test strategy and open questions (Section 11). Bump its
  version line at the top when editing it (git history is the change log); new findings from captures go
  into Section 3 ("Observed" notes) and Appendix B.
- `README.md` — the quick start: what the project is, how to build and run every target and how to run the
  tests, with a Run section for every program and integration (what it does, how to start it, its flags or keys,
  how it is tested). The user wants it kept current as the project grows: it must change in the same edit as
  anything it names or describes — presets, options, targets, program names and flags, MRL keys, test labels and
  names, scripts, paths, behaviour. The ctest entry `docs.readme` (`implementation/scripts/readme-check.py`, label
  `docs`, part of the default `ctest --preset debug` run) fails when such a reference goes stale and when a program
  built under `tools/` or `integrations/`, a flag of its usage text or a VLC plugin option is not named in the
  README; prose is not checked, so update it by hand when behaviour it describes changes, and keep it short.
- `specs/` — ATSC standards as PDFs (A/300, A/321, A/322, A/324, A/330, A/331, A/332, A/337, A/344,
  A/360, …) and schema zips. Created entirely by the root `setup.sh` (README "Inputs"), not versioned.
- `specs/schemas/` — the normative A/331 XML schemas, ABNF grammars and example instances, unpacked by
  `setup.sh --schemas` from `A331-2022-09-Schema.zip` into `lls/` and `sls/{application,emergency,mmtp,route}/`
  (the unit tests read it and skip when it is absent). Per the release notes the schemas take precedence over
  the A/331 text; typed structures in the spec are derived from them.
- `samples/` — capture corpus (see below), also created by `setup.sh` and not versioned.

- `implementation/` — the C++17 implementation (CMake). Implemented so far: the `io` layer (the delivery
  units — BBP, ALP packet, IP datagram with `UnitTime` and `L1Info` — and the IPv4/UDP/RTP header helpers; the capture
  code lives in `tuners/pcap`, below), the STLTP demux
  (`stltp`), the BBP deframer (`phy`), ALP decapsulation with segmentation/concatenation/SID/TS
  (`alp`), LMT/RDT parsing with the PLP map (`link`; the ROHC decompressor is an interface only),
  the IPv4/UDP demux with flow routing and fragment reassembly (`ip`; handlers may subscribe or
  unsubscribe from inside a delivery callback), the LLS processor with gzip, SignedMultiTable,
  registry and typed SLT/SystemTime/AEAT/RRT/OMN parsers (`lls`), the ROUTE layer (`route`: LCT
  header + EXT_FTI/EXT_TOL/EXT_TIME/EXT_ROUTE_PRESENTATION_TIME, object recovery `Session` with
  transfer-length / EFDT-length / close-flag / carousel / timeout completion, in-band and S-TSID
  EFDTs with `fileTemplate`, entity and package modes, gzip Content-Encoding, `multipart` parser),
  the ROUTE SLS layer (`sls`: envelope, USBD, S-TSID, HELD, light MPD with template resolution and
  the static-MPD rewrite, `SlsBundle` with Annex C TOI bits and multipart/signed, `RouteReceiver`
  that opens the SLS session from an SLT entry and the media/NRT sessions from the S-TSID), plus the
  MMTP layer (`mmtp`: packet header with the version-01 QoS word, MPU/signaling payload headers,
  signaling reassembly, MP table complete/subset, `mmt_atsc3_message` with gzip, MMT USBD, and
  `MpuAssembler`: MPU reconstruction into MSE init segments and `styp moof mdat` fragments with the
  keep/rewrite tfdt timeline, enabled per session by `setMpuAssembly`), the `media` layer
  (`media/isobmff.hpp`: box walking, init/moof parsing, RFC 6381 codecs strings from sample entries,
  the MSE-oriented init/fragment rewrites; on-air fixtures in `tests/vectors/mmt`), the service manager (`service::Engine`: `open()` takes a
  `service::ITuner` (`include/atsc3/service/tuner.hpp`), couples it at the layer it delivers — BBP, ALP or IP — and runs the
  chain from there; `tuner()` hands it back for implementation controls; service table keyed by (bsid, serviceId) from every SLT, acquires the SLS of
  every ROUTE and MMTP service, builds the component/track model with codecs, selection opens media and
  NRT sessions, thread-safe snapshots + a state version for long-polling), plus the private
  namespace-tolerant XML helpers (`src/xml/xml.hpp`) and `util/gzip`.
  `tuners/` holds the tuner implementations: `tuners/pcap` is `atsc3::tuner_pcap` (namespace `pcap`: `VirtualTuner` with
  real-time/non-real-time pacing, pause, loop and time window, the pcap/pcapng reader and writer, format detector, DekTec
  `.xa3` sidecar, BBP container; option `ATSC3_TUNER_PCAP`, on — every tool, test and integration opens the engine with
  `std::make_shared<pcap::VirtualTuner>(cfg)`; tests use `pcapTuner(path, cfg)` from `tests/support/tuner.hpp`) and
  `tuners/sony` the vendor placeholder (`ATSC3_TUNER_SONY`).
  `implementation/tools/` has one directory per standalone program — `atsc3scan` (capture inventory: format verdict,
  BBP/ALP/LMT facts, LLS tables, UDP flows, LCT flows with object completeness, MMTP flows and signaling; `--limit N`,
  `--slt`, `--sls`, `--objs`, `--mmt`; its corpus numbers are the golden baselines in `tests/golden/scan_test.cpp`),
  `stltp2alp` and `routedump` — plus `tools/lib`, the `atsc3::tools` library
  (namespace `atsc3::tools`, headers `atsc3/tools/`) the thin mains are built from and the tests link: the scan,
  routedump and STLTP→ALP bodies, the packet builders and the ALP→BBP and BBP→STLTP packers; built when tools or
  tests are on. Integrations — programs that embed the stack and present it through their own
  interface (spec 4.1) — live under `implementation/integrations/`. `integrations/web` (preset `web`, option `ATSC3_WEB`) is the cpp-httplib server
  behind the daemon `integrations/web/atsc3d`: REST API under `/api/v1` (status with `?since=&wait=` long-poll, source list /
  open / pause / close, services, service detail with raw fragments, select, lls, link), bearer token,
  static serving of `integrations/web/frontend/dist` with SPA fallback. No SSE/WebSocket yet (the frontend long-polls
  `/status`). POST/DELETE requests must carry a body (even empty): cpp-httplib waits for one. The
  Vue 3 + Vite + Pinia frontend in `integrations/web/frontend/` has Source, Services, service detail (components, codecs,
  SLS XML), Signaling, Link, Stats, EPG and Player views; Alerts is a placeholder. Playback: the engine keeps a
  bounded segment store per selected ROUTE service, the server is a local DASH origin under `/media/{bsid}/{sid}/`
  (`mpd.mpd` origin rewrite whose UTCTiming is `time.xsdate` = the availability time of the newest cached segment,
  because broadcasters send segments seconds ahead of availability and a fresh receiver has nothing older;
  `offline.mpd`, `player.json`, `index.json`, segments with Range) and `PlayerPanel.vue` plays it with dash.js 5
  (startup gate on `minSegments`, EME via `setProtectionData` with a user-entered license URL). Replay must be
  real-time for playback. MMT services play through the push path: the engine keeps the reconstructed MPU
  fragments per selected MMTP service, the server serves `/media/{bsid}/{sid}/mmt/index.json|{pid}/init.mp4|{pid}/{mpu}.m4s`
  (player.json `mode: push`), and `integrations/web/frontend/src/player/MsePushPlayer.ts` appends them with MSE (spec Q11: HTTP
  polling, not the WebSocket). Broadcaster applications (A/344, spec 5.18.4): the engine keeps an Application
  Context Cache per selected service (HELD apps from the ROUTE SLS or the MMT `mmt_atsc3_message` 0x0003, NRT files
  and unpacked `multipart/related` packages, plus the ROUTE session named by the MMT USBD `ROUTEComponent`), the
  server serves it under `/apps/{bsid}/{sid}/` and injects `a344-shim.js` into the entry page, and the service detail
  view (only there, not the Player view) runs the app in an iframe over the video with an in-page JSON-RPC receiver
  host, key routing and a virtual remote (`integrations/web/frontend/src/a344/`, `AppOverlay.vue`, `RemoteControl.vue`;
  `e2e/apps.spec.ts`). Playwright e2e (`integrations/web/frontend/e2e`, Firefox project) needs
  Node 20+ (`~/.nvm/versions/node/v22.22.0`) and `npx playwright install firefox`; run against a daemon with
  `ATSC3_WEB_URL=http://127.0.0.1:PORT npx playwright test --project=firefox`, or `tests/e2e/run-web.sh <atsc3d> <corpus>`
  with `PLAYWRIGHT_PROJECT=firefox` (it starts its own daemon on `ATSC3_E2E_PORT`, default 18080).
  ESG: the service layer splits SGDUs and parses SGDDs (`include/atsc3/service/esg.hpp`), the engine opens the
  sessions of every category-4 service by policy and keeps versioned fragments plus icon files, the core's `esg`
  layer (`src/esg`, `include/atsc3/esg/esg.hpp`, namespace `esg`) holds the OMA data
  model with A/332 extensions and the programme index, and the web server serves `/api/v1/time` and `/api/v1/esg[/services|/schedule|/content/{id}|/icons/{name}|/fragments]`.
  The VLC plugin (`integrations/vlc`, preset `vlc`, spec 5.19) is an `access_demux` module for the VLC 3.0 line built against
  the distribution's SDK (`libvlccore-dev` 3.0.16, `pkg-config vlc-plugin`; `cmake --preset vlc`; a VLC source tree via
  `VLC_SOURCE_DIR` or `../vlc/vlc-*` plus `VLC_BUILD_DIR` is the fallback; `demux.hpp` defines the `vlc_tick_t`/`VLC_TICK_*`
  aliases that 3.0.16's headers lack): it runs `service::Engine` on the capture, lists services
  as VLC programs, feeds the selected service's ROUTE segments / MMT fragments as elementary streams through
  `media::decoderConfigs()` and `media::fragmentSamples()` (no chained mp4 demuxer), pushes the ESG as EPG and logs AEAT
  alerts. Run it with `VLC_PLUGIN_PATH=build/vlc/integrations/vlc cvlc 'atsc3:///abs/capture.pcap?service=5004'`
  (`pacing=nonrealtime` with `--sout` dumps faster than real time); tests: `integrations/vlc/tests/cvlc/run.sh`,
  `integrations/vlc/tests/pyvlc/libvlc_test.py` (ctypes on libvlc.so.5) and `tests/e2e/run-vlc.sh` (ctest `e2e.vlc`, ~5 min).
  The set-top-box player (`integrations/stb`, preset `stb`, option `ATSC3_STB`, spec 5.20) is `atsc3stb`: the engine on
  the pcap virtual tuner, GStreamer decoding in a window and a cairo-drawn STB UI (channel list on the left half,
  channel banner with present/following at the bottom on every channel change; Return opens the list, Return in the list
  tunes, Up/Down change channel otherwise, `i` shows the banner; no track selection, no apps). One
  persistent pipeline (black `videotestsrc` + `compositor` + `cairooverlay` + video sink, silent `audiotestsrc` +
  `audiomixer` + audio sink) keeps the window open; every decodable track of the tuned service is a branch
  `appsrc → decodebin → convert → mixer pad` fed with `media::fragmentSamples()` on one timeline anchored to the
  pipeline clock plus a live delay (the VLC plugin's scheme). `src/model.*` is the GStreamer-free UI model (unit tests
  `[stb]`), `src/player.*` the pipeline, `src/overlay.*` the cairo drawing. Keys arrive as GstNavigation events from
  the sink window; `--keys "Down@5,Return@6"` scripts them and `--video-sink fakevideosink --audio-sink fakesink`
  runs headless, which is how `tests/e2e/run-stb.sh` (ctest `e2e.stb`, label `e2e-stb`, about 2 min) checks decoded
  frames per tuned channel, the banner and the ESG. GStreamer facts learned: a branch added while another branch's
  `decodebin` is still async must be set PLAYING explicitly (the pipeline reports PAUSED, `sync_state_with_parent`
  leaves it dead); lock a branch's state before tearing it down or the pipeline's state continuation restarts it;
  `appsrc` needs a `max-latency` or the mixers' latency query fails; hardware decoders are skipped unless
  `--hw-decode` (the VA-API path produced garbage here); 10-bit HEVC decodes in software fine.
  Security is a placeholder.
  Layout and build rules are in spec Section 8.
  All implementation work goes in this directory, nothing at the repository root:
  `cmake --preset debug && cmake --build --preset debug && ctest --preset debug` (labels `unit`,
  `golden`, `tools`, `http` (web preset), `e2e-web`, `e2e-vlc`, `e2e-stb` (stb preset)); single Catch2 case: `build/debug/tests/unit/atsc3_unit "[lct]"`;
  optional parts via `-DATSC3_WEB=ON -DATSC3_VLC=ON -DATSC3_STB=ON -DATSC3_FRONTEND=ON`; frontend:
  `cd implementation/integrations/web/frontend && npm ci && npm run build`. Dependencies (pugixml, Catch2, and
  cpp-httplib/IXWebSocket for `web`) are fetched from GitHub by CMake unless installed system-wide.
  Tests tagged `[corpus]` read `../samples` and skip when it is absent; `[pacing]` tests measure real
  time (a few hundred ms each) — run `atsc3_unit "~[corpus]"` for a fast loop. Test helpers (packet
  builders, `lct.hpp` LCT builder, `RecordingSink`) live in `implementation/tests/support/`. Run the
  `asan` preset before declaring a layer done.

The directory is not a git repository.

## Commands

Every tool is built by the debug preset (`cmake --preset debug && cmake --build --preset debug`):

```bash
# Inventory of any capture (DLT 289 ALP, STLTP, BBP container, Ethernet or raw IP): format verdict, BBP/ALP/LMT facts,
# LLS tables, UDP flows, LCT flows with object completeness, MMTP flows and signaling, unclassified flows
implementation/build/debug/tools/atsc3scan/atsc3scan samples/ch11-25-slt.pcap
implementation/build/debug/tools/atsc3scan/atsc3scan --slt --sls --objs samples/ch11-25-slt.pcap   # + SLT XML, SLS objects, every object
implementation/build/debug/tools/atsc3scan/atsc3scan --limit 120000 --mmt <big.pcap>             # first N records; + MP tables, mmt_atsc3_messages

# STLTP capture -> DLT 289 ALP capture
implementation/build/debug/tools/stltp2alp/stltp2alp [--bbp out.bbp.pcap] [--plp N] [--start S] [--duration S] in.pcap out.pcap

# Daemon + web UI (build with the web preset once: cmake --preset web && cmake --build --preset web;
# frontend once: cd implementation/integrations/web/frontend && npm ci && npm run build). Captures default to ../samples.
implementation/build/web/integrations/web/atsc3d/atsc3d [--pcap FILE] [--captures DIR] [--web 127.0.0.1:8080] [--nonrealtime] [--loop]
curl -s http://127.0.0.1:8080/api/v1/services | python3 -m json.tool      # UI at http://127.0.0.1:8080/
cd implementation/integrations/web/frontend && npm run dev                                  # Vite dev server on :5173 proxying /api to :8080

# Extract every ROUTE service (STLTP, ALP, BBP or raw-IP capture) into <out>/bsid-<bsid>/<serviceId>-<name>/:
# manifest.mpd (static, plays offline in VLC), init + media segments named by Content-Location, sls/ fragments,
# nrt/ files with multipart packages unpacked into <name>.d/; summary.txt lists what each service yielded.
implementation/build/debug/tools/routedump/routedump [--service ID] [--start S] [--duration S] [--no-nrt] in.pcap out-dir
vlc out-dir/bsid-1409/5004-WMAR/manifest.mpd        # ffprobe 4.4 refuses the .mp4v/.m4pv segment extensions by policy
```

Reading the standards: `pdftotext -layout specs/<spec>.pdf <out>.txt` into the session scratchpad,
then grep/sed the text. `tcpdump -nn -r` and `capinfos` are available for quick capture summaries.

## Capture corpus

- `samples/alp-sample1.pcap`, `alp-sample2.pcap`, `ch11-25-slt.pcap`, `lls-tables-alp.pcap`,
  `mp4-ftyp-styp-sidx.pcap`: the ATSC 3.0 sample captures of the Wireshark wiki
  (https://wiki.wireshark.org/samplecaptures#atsc3-protocols; SiliconDust receiver, several stations including
  Baltimore bsid 1409, December 2022 / January 2023); spec 3.1 lists their contents.
- `samples/dektec/` comes from DekTec's FTP (`ftp.dektec.nl`, user dektec, folder `pub/persistent`).
  `ATSC3-STLTP/BWI_20230307_1742.pcap` (943 MB): STLTP from a broadcast gateway,
  PLPs 0–2; decodes end to end (`atsc3scan --limit 320000`: 73 046 BBPs, 282 642 ALP packets, 2333/26 objects).
- `samples/dektec/ATSC3-PCAP/UniSoft-*.pcap` (644/765 MB): raw IP multicast (no ALP, no LMT); the
  second one contains an MMT service. The `.xa3` files beside them are DekTec modulator profiles;

Never cat or hexdump the large captures wholesale; use the analyzers with `--limit` or `tcpdump`
with a packet count. Treat the corpus and `specs/` as read-only inputs.

## Facts worth knowing before touching the spec

- Tuners deliver either Baseband Packets or ALP packets; captures come as DLT 289, STLTP, or raw IP.
  The pcap virtual tuner auto-detects the form and, for plain IP, bypasses the PHY/ALP/link layers.
- MMTP version-01 packets carry a 16-bit QoS/flow word right after `packet_counter` regardless of
  the flag bits (seen on two encoders); the spec treats it as a fixed header field.
- Real streams deviate from the schemas (SLT without namespace, `Name lang=` instead of `xml:lang`,
  `MediaInfo contentType="text"`, old `application/route-usd+xml` media types, three EFDT
  packagings); parsers must be lenient, and such deviations are catalogued in spec Section 3.
- Settled decisions (spec Section 11): first RF target is a Sony demodulator whose output format is
  unknown; ROHC-U is implemented in-house; signature failures are reported, never block; the
  VLC plugin targets the newest *released* VLC, the 3.0 line (a VLC 4 port is a follow-up);
  DekTec `.xa3` is the only sidecar format; IPv6 is out of scope.
- VLC facts learned on 3.0.x: the plugin must call `ES_OUT_SET_GROUP` for the program it feeds or no decoder is
  created; decoders refuse blocks more than pts-delay + 3 s ahead of the last PCR ("Timestamp conversion failed"), so
  the plugin keeps a 1.5–2.5 s lead and reports a 1.5 s pts delay; the engine's delivery thread must yield to waiting
  API callers (`Engine::ApiLock`) or non-real-time replay starves every call; VLC headers are not to be wrapped in
  `extern "C"`; the in-tree `bin/vlc` needs a flat module directory in `VLC_PLUGIN_PATH`.
- A/344 apps in the corpus: Sinclair WBFF (HELD in the ROUTE SLS, `App.pkg` 3.9 MB React app on a continuous
  ≈245 kbit/s carousel: a full copy lands ≈126 s after selection, the service page shows the progress), WNUV (same app, HELD via MMT 0x0003, packages on the ROUTE session the USBD
  `ROUTEComponent` names), WMAR (broadband-only HELD) and UniSoft usc01 (`uscapp`, one canvas page carouselled ≈19×/s).
  The Sinclair app calls twelve `org.atsc.*` methods including the non-standard `org.atsc.query.MPDUrl`, keys on
  `KeyboardEvent.code`, and draws its menu from a broadband feed (empty screen offline); verify app hosting with usc01
  first, it is ready within a second of selection.
- `samples/lls-tables-alp.pcap` carries two broadcasters' SLTs (bsid 564 and bsid 668) in the same LLS
  group 0 from different source addresses; anything keyed by LLS group alone sees them overwrite each other.
- On-air codecs: Sinclair services are HEVC + Dolby AC-4 (`ac-4.02.01.x`) + IMSC1 (`stpp.ttml.im1t`), UniSoft
  HEVC (service 1 is 4K) + AAC-LC. VLC 3 and Chromium MSE cannot decode AC-4; verify playback with the
  UniSoft AAC services, not the Sinclair ones.
- ROUTE on air: `EXT_TOL` HET 194 is a 24-bit length, HET 67 an 8-byte extension with a 48-bit length;
  `EXT_FTI` puts the 48-bit transfer length first; Sinclair MPDs use `$Number$` with epoch-second
  numbers and `startNumber=1`, UniSoft MPDs `$Number$` with small numbers, the Triveni sample `$Time$`
  with a SegmentTimeline. The offline MPD rewrite keeps `$Number$` templates (longest contiguous run,
  `presentationTimeOffset` from the first number) and rebuilds the SegmentTimeline for `$Time$`.
  Segments are matched to Representations by name against the resolved `media`/`initialization`
  templates, then by `MediaInfo@repId` with TOI as the number. The live origin calibrates `$Number$`
  `startNumber` against the cached segments' tfdt (spec 5.17.3): UniSoft usc01 numbers its segments from 0 but
  signals startNumber 67/65 with no presentationTimeOffset, which left dash.js buffering 134 s ahead of its playhead.
