# ATSC 3.0 client stack

A receiver-side ATSC 3.0 protocol stack in C++17. It takes what a demodulator delivers (Baseband
Packets, ALP packets or IP datagrams) and turns it into typed signaling (LLS, ROUTE SLS, MMT
tables), DASH segments, reconstructed MMT MPUs, NRT files, the OMA BCAST service guide, emergency
alerts and A/344 broadcaster applications. A tuner feeds the stack from below; the pcap virtual
tuner replays recorded broadcasts (ALP, STLTP or raw-IP captures) so everything runs on the
capture corpus without hardware. Three integrations embed the stack: a daemon with a REST API and a
Vue 3 web UI (service list, signaling, EPG, DASH/MSE playback, apps), a VLC 3 plugin that plays the
captures as programs, and a GStreamer set-top-box player with a channel list and a channel banner.

`ATSC3_CLIENT_STACK_SPEC.md` is the specification and the source of truth for the architecture,
module behaviour, layout and test strategy. This file is the quick start and is kept in step with
it (see the end).

## Repository layout

| Path | Contents |
|------|----------|
| `ATSC3_CLIENT_STACK_SPEC.md` | The specification (architecture, modules, ingest formats, integrations, tests, open questions). |
| `implementation/` | The C++17 stack: CMake tree with the core library, tuners, tools, integrations and tests. |
| `specs/` | The ATSC standards as PDFs (A/300, A/321, A/322, A/324, A/330, A/331, A/332, A/337, A/344, A/360) and their schema zips; created entirely by `setup.sh`, not versioned. |
| `specs/schemas/` | The A/331 schema release (normative XML schemas, ABNF grammars, example instances) unpacked by `setup.sh` from its zip; read-only test input. |
| `samples/` | The capture corpus the golden and end-to-end tests replay (read-only input); fetched by `setup.sh`, not versioned. |
| `setup.sh` | Fetches the standards and the captures (see Inputs). |

Inside `implementation/`: `src/` and `include/atsc3/` are the core library `atsc3::core`;
`tuners/pcap` is the pcap virtual tuner `atsc3::tuner_pcap` (`tuners/sony` is the placeholder for
the first RF tuner); `tools/` holds the standalone programs `atsc3scan`, `stltp2alp` and
`routedump` plus `tools/lib`, the `atsc3::tools` library they are built from and share with the tests; `integrations/web` is the
web server, the daemon `atsc3d` and the frontend; `integrations/vlc` is the VLC plugin; `integrations/stb` is the
set-top-box player `atsc3stb`; `tests/` has the unit, golden, round-trip, HTTP, benchmark, fuzz and end-to-end suites.

## Inputs

Nothing under `specs/` or `samples/` is in version control (root `.gitignore`); `setup.sh` creates both and skips
files already present. The PDFs and zips come from atsc.org (`specs/schemas/` is unpacked from the A/331 schema
zip), the five small ALP captures from the
Wireshark sample-captures wiki, and the DekTec captures from `ftp.dektec.nl` (user dektec, folder
`pub/persistent`), for which the script asks for the password. Captures and the schema tree are checked against
the SHA-256 the tests expect. Run at least `./setup.sh --schemas` before the unit tests, or the schema cases skip.

```bash
./setup.sh                                    # everything: specs/, samples/, samples/dektec/ (password prompt)
./setup.sh --specs --samples                  # only the public downloads, no prompt
./setup.sh --schemas                          # only the A/331 schema zip and specs/schemas/ (what the unit tests read)
DEKTEC_FTP_PASSWORD=... ./setup.sh --dektec   # non-interactive DekTec fetch; --force refetches present files
```

## Requirements

- CMake 3.21 or newer, a C++17 compiler (GCC or Clang), zlib development headers.
- pugixml and Catch2 are used from the system when installed and otherwise fetched from GitHub by
  CMake (`ATSC3_FETCH_DEPS`, on by default); the web integration adds cpp-httplib and IXWebSocket
  the same way. Offline builds: see `implementation/third_party/README.md`.
- Web frontend and its Playwright tests: Node 20 or newer, then `npx playwright install firefox`.
- VLC plugin: the VLC 3.0 plugin SDK from the distribution (`libvlccore-dev`, found through
  `pkg-config vlc-plugin`); its tests need `cvlc` and `libvlc.so.5` at run time.
- Set-top-box player: GStreamer 1.20 and cairo development packages (`libgstreamer1.0-dev`,
  `libgstreamer-plugins-base1.0-dev`, `libcairo2-dev`) and, at run time, the base, good, bad and libav plugin
  sets (compositor, audiomixer, cairooverlay, decodebin, avdec_h265, avdec_aac, the auto sinks).
- Python 3 (standard library only) for the README check and the VLC test harness; `curl` and `unzip` (or Python 3)
  for `setup.sh`.

## Build

Every configuration is a CMake preset; each builds into `implementation/build/<preset>/`.

```bash
cd implementation
cmake --preset debug && cmake --build --preset debug      # core + tuners + tools + tests
cmake --preset web   && cmake --build --preset web        # + web server, atsc3d daemon, HTTP tests
cmake --preset vlc   && cmake --build --preset vlc        # + VLC plugin and its e2e test
cmake --preset stb   && cmake --build --preset stb        # + the GStreamer set-top-box player atsc3stb and its e2e test
cmake --preset asan  && cmake --build --preset asan       # debug with address + undefined sanitizers
cmake --preset release && cmake --build --preset release
```

Other presets: `ubsan`, `tsan`, `coverage`, `fuzz` (clang, libFuzzer targets `fuzz_<parser>`),
`full` (release with web, VLC, security and the frontend built by CMake) and `embedded-arm64`
(cross build, toolchain file required). The options behind the presets are in
`implementation/cmake/Options.cmake`: `ATSC3_BUILD_TOOLS`, `ATSC3_BUILD_TESTS`,
`ATSC3_TUNER_PCAP`, `ATSC3_TUNER_SONY`, `ATSC3_WEB`, `ATSC3_VLC`, `ATSC3_STB`, `ATSC3_FRONTEND`,
`ATSC3_SECURITY`, `ATSC3_FUZZ`, `ATSC3_COVERAGE`, `ATSC3_SANITIZERS`, `ATSC3_FETCH_DEPS`,
`ATSC3_CORPUS_DIR` (default `../samples`) and `ATSC3_SCHEMA_DIR` (default `../specs/schemas`).

The web UI is an npm project. Build it once and the daemon serves it from `frontend/dist`
(`-DATSC3_FRONTEND=ON` lets CMake run the same build):

```bash
cd implementation/integrations/web/frontend
npm ci && npm run build
```

## Run

### Daemon and web UI

```bash
cd implementation
build/web/integrations/web/atsc3d/atsc3d --pcap ../samples/ch11-25-slt.pcap
```

Open http://127.0.0.1:8080/ for the UI, or query the REST API under `/api/v1`:

```bash
curl -s http://127.0.0.1:8080/api/v1/status | python3 -m json.tool
curl -s http://127.0.0.1:8080/api/v1/services | python3 -m json.tool
```

Without `--pcap` the Source view lists every capture under `--captures DIR` (default
`../samples`). Replay flags: `--nonrealtime` (as fast as possible), `--speed X`, `--loop`,
`--duration SEC`. Server flags: `--web ADDR:PORT` (default 127.0.0.1:8080), `--www DIR`,
`--token T` (bearer token), `--allow-remote`, `--no-web` (headless). Playback in the Player view
needs real-time replay. For frontend development, `npm run dev` in the frontend directory starts
Vite on port 5173 proxying `/api` to the daemon.

### VLC plugin

```bash
cd implementation
VLC_PLUGIN_PATH=build/vlc/integrations/vlc cvlc 'atsc3:///abs/path/capture.pcap?service=2'
```

`vlc` instead of `cvlc` gives the GUI, where the services appear under Playback, Program. MRL
query options (each also exists as `--atsc3-<name>`): `service` (service id or `major.minor`),
`pacing=nonrealtime` (for dumping with `--sout`, not for watching), `loop`, `speed`, `start`,
`sidecar=auto|off|path`, `esg`, `alerts=osd|log|off`, `live-delay`, `verbose-stats`,
`show-hidden`. VLC 3 cannot decode the AC-4 audio of the Sinclair services; verify playback with the
UniSoft captures. `cmake --install build/vlc --component vlc-plugin` installs the module into
VLC's plugin directory.

### Set-top-box player (GStreamer)

```bash
cd implementation
build/stb/integrations/stb/atsc3stb --service 2 ../samples/dektec/ATSC3-PCAP/UniSoft-3xROUTE-24-October-2019.pcap
build/stb/integrations/stb/atsc3stb --service 5007 ../samples/dektec/ATSC3-STLTP/BWI_20230307_1742.pcap   # Sinclair WNUV (MMT): video only, AC-4 audio
cmake --install build/stb --component stb                 # installs atsc3stb into bin/
```

`atsc3stb` plays a capture in a window the way a set-top box does: the stack on the pcap virtual tuner, GStreamer
decoding of the tuned service's video and audio (software decoders unless `--hw-decode`), a vertical channel list on
the left half of the screen and a channel banner at the bottom with the channel number and name, the clock, the
present programme with a progress bar, the following programme and the codecs. The banner appears on every channel
change and stays for `--banner-seconds` (5 s). Keys in the window (also `--keys` below):

| Key | List closed | List open |
|-----|-------------|-----------|
| Up / Down | previous / next channel of the mux, wrapping | move the highlight |
| Page Up / Page Down | same as Up / Down | move the highlight by a page |
| Return (also `l`, Tab, Menu) | open the channel list on the tuned channel | tune the highlighted channel and close the list |
| Escape | hide the banner | close the list |
| `i` | show the banner | |
| `q` | quit | quit |

The list holds the ROUTE and MMT services of the mux that carry video or audio, with "(DRM)" on protected ones;
hidden services appear with `--show-hidden`. Channel changes stay within the mux; there is no track selection and no
broadcaster application. Flags: `--service ID|major.minor` (the channel to start on; the first one otherwise),
`--size WxH` (1280x720), `--loop`, `--start S`, `--duration S` (exit after S seconds; otherwise at the end of the
capture), `--banner-seconds S`, `--live-delay S` (lead of the data over the clock, twice the segment duration by
default), `--sidecar auto|off|PATH` (DekTec `.xa3` profile for raw-IP captures), `--show-hidden`, `--no-esg` (no
present/following), `--hw-decode`, `--video-sink NAME` and `--audio-sink NAME` (GStreamer sink elements;
`fakevideosink` / `fakesink` run headless), `--keys "Down@5,Return@6"` (scripted key presses: GstNavigation key names
at seconds) and `--verbose` (timeline, decoder choice, GStreamer warnings). It prints one line per channel list, tune,
track verdict and present/following change, statistics per tune (decoded video frames, audio buffers) and a final
`summary` line, which is what the end-to-end test reads; the exit status is 1 on a GStreamer error and 2 on bad
arguments. AC-4 audio has no GStreamer decoder, so the Sinclair services play video only; the UniSoft services play
with sound. The UniSoft ESG names channels 3.x while its SLT says 24.x, so that capture shows no programme
information; the Sinclair capture does.

### Tools

```bash
cd implementation
build/debug/tools/atsc3scan/atsc3scan [--limit N] [--start S] [--duration S] [--slt] [--sls] [--objs] [--mmt] [--probe N] [--sidecar FILE] ../samples/ch11-25-slt.pcap
build/debug/tools/stltp2alp/stltp2alp [--bbp out.bbp.pcap] [--plp N] [--start S] [--duration S] [--force] [-q] in.pcap out.pcap
build/debug/tools/routedump/routedump [--service ID] [--start S] [--duration S] [--no-nrt] [--no-unpack] [--no-sls] [--sidecar FILE] [-q] in.pcap out-dir
```

`atsc3scan` is the inventory of a capture of any form: the format verdict, BBP and ALP header facts, the
LMTs, the LLS tables, the UDP flow table, every LCT flow with its object completeness, every MMTP flow with
its signaling, and the flows nothing claimed. It scans the first N records with `--limit`, prints the SLT XML
with `--slt`, the SLS objects with `--sls`, every object with `--objs` and the MP tables and ATSC messages with
`--mmt`; `--probe N` is the format detector's window in records; it exits with status 3 when a capture carries no
ATSC 3.0 content. `stltp2alp` turns an STLTP capture into a DLT 289 ALP capture (`--force` skips the format
detection and treats the input as STLTP). `routedump` extracts every ROUTE service of a capture into
`out-dir/bsid-<bsid>/<serviceId>-<name>/` with a static `manifest.mpd` that VLC plays offline, the segments,
the SLS fragments and the NRT files; `--no-nrt`, `--no-unpack` and `--no-sls` leave out the NRT files, the
unpacking of packages and the SLS fragments. `--start S` and `--duration S` cut a window out of the capture,
`--sidecar auto|off|FILE` names the DekTec `.xa3` profile of a raw-IP capture (default: the file beside it) and
`-q` silences the summary.

## Tests

```bash
cd implementation
ctest --preset debug                      # unit, golden (the corpus inventory numbers), roundtrip, tools, bench, docs
ctest --preset debug -L unit              # one label; others: golden, roundtrip, tools, bench, http, docs, e2e-vlc, e2e-stb
build/debug/tests/unit/atsc3_unit "[lct]"           # one Catch2 tag or test case
build/debug/tests/unit/atsc3_unit "~[corpus]"       # fast loop: skip the tests that read ../samples
build/stb/tests/unit/atsc3_unit "[stb]"             # the set-top-box UI model: channel list, keys, banner, present/following
ctest --preset asan                       # run before declaring a layer done
ctest --preset web -L http                # REST contract tests against an in-process daemon
ctest --preset vlc -R e2e.vlc             # VLC end-to-end suite, real-time replays, about 5 minutes
ctest --preset stb -R e2e.stb             # set-top-box player end to end, headless real-time replays, about 2 minutes
```

Tests tagged `[corpus]` and the golden, tools and end-to-end suites read `../samples` and skip
when it is absent. `[pacing]` tests measure real time.

The web end-to-end suite starts its own daemon on `ATSC3_E2E_PORT` (default 18080, away from a
daemon on 8080) and runs the Playwright specs of the frontend; it is registered as `e2e.web` but
disabled in ctest, so call the script:

```bash
cd implementation
PLAYWRIGHT_PROJECT=firefox tests/e2e/run-web.sh build/web/integrations/web/atsc3d/atsc3d ../samples
```

Against a daemon that is already running:

```bash
cd implementation/integrations/web/frontend
ATSC3_WEB_URL=http://127.0.0.1:18080 npx playwright test --project=firefox
```

The VLC suite can also be run by hand with `tests/e2e/run-vlc.sh <plugin.so> <corpus>`; its parts
are `integrations/vlc/tests/cvlc/run.sh` (headless cvlc scenarios checked against the log) and
`integrations/vlc/tests/pyvlc/libvlc_test.py` (libvlc through ctypes).

The set-top-box suite is `tests/e2e/run-stb.sh <atsc3stb> <corpus>`: headless runs of `atsc3stb` on the UniSoft,
Sinclair and small ALP captures with scripted keys, checked against the log for decoded frames on every tuned
channel, the banner and the present programme. It skips without the corpus or the GStreamer plugins.

Formatting and linting: `scripts/format.sh` (clang-format, prettier) and `scripts/lint.sh`
(clang-tidy over `build/debug`, eslint).

Continuous integration: `.github/workflows/ci.yml` builds the debug, release and asan presets and runs their unit
tests on every push and pull request, after `./setup.sh --schemas`. Its `corpus` job then fetches the captures with
the repository secret `DEKTEC_FTP_PASSWORD` and runs the golden, tools and corpus-tagged tests. Fork pull requests
never receive secrets, so that job skips its steps instead of failing. `.github/workflows/e2e.yml` runs the
end-to-end suites nightly on a self-hosted runner.

## Keeping this file current

This README must change in the same edit as anything it describes: presets, options, targets,
program names and flags, test labels, scripts, paths and behaviour, and every new program or integration
gets a Run section here (what it does, how to start it, its flags or keys, how it is tested). The ctest
entry `docs.readme` (label `docs`, script `implementation/scripts/readme-check.py`) fails when a
preset, option, flag, MRL key, test label, test name, npm script, Playwright project or path named
here no longer exists, and when a program built under `tools/` or `integrations/`, a flag of its
usage text or a VLC plugin option is not named here, so a stale or incomplete README breaks the
default test run. Prose is not checked; keep it short and update it by hand.
