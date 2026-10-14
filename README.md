# ATSC 3.0 client stack

A receiver-side ATSC 3.0 protocol stack in C++17. It takes what a demodulator delivers (Baseband
Packets, ALP packets or IP datagrams) and turns it into typed signaling (LLS, ROUTE SLS, MMT
tables), DASH segments, reconstructed MMT MPUs, NRT files, the OMA BCAST service guide, emergency
alerts and A/344 broadcaster applications. A tuner feeds the stack from below; the pcap virtual
tuner replays recorded broadcasts (ALP, STLTP or raw-IP captures) so everything runs on the
capture corpus without hardware. Two integrations embed the stack: a daemon with a REST API and a
Vue 3 web UI (service list, signaling, EPG, DASH/MSE playback, apps), and a VLC 3 plugin that plays
the captures as programs.

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
web server, the daemon `atsc3d` and the frontend; `integrations/vlc` is the VLC plugin; `tests/`
has the unit, golden, round-trip, HTTP, benchmark, fuzz and end-to-end suites.

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
- Python 3 (standard library only) for the README check and the VLC test harness; `curl` and `unzip` (or Python 3)
  for `setup.sh`.

## Build

Every configuration is a CMake preset; each builds into `implementation/build/<preset>/`.

```bash
cd implementation
cmake --preset debug && cmake --build --preset debug      # core + tuners + tools + tests
cmake --preset web   && cmake --build --preset web        # + web server, atsc3d daemon, HTTP tests
cmake --preset vlc   && cmake --build --preset vlc        # + VLC plugin and its e2e test
cmake --preset asan  && cmake --build --preset asan       # debug with address + undefined sanitizers
cmake --preset release && cmake --build --preset release
```

Other presets: `ubsan`, `tsan`, `coverage`, `fuzz` (clang, libFuzzer targets `fuzz_<parser>`),
`full` (release with web, VLC, security and the frontend built by CMake) and `embedded-arm64`
(cross build, toolchain file required). The options behind the presets are in
`implementation/cmake/Options.cmake`: `ATSC3_BUILD_TOOLS`, `ATSC3_BUILD_TESTS`,
`ATSC3_TUNER_PCAP`, `ATSC3_TUNER_SONY`, `ATSC3_WEB`, `ATSC3_VLC`, `ATSC3_FRONTEND`,
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

### Tools

```bash
cd implementation
build/debug/tools/atsc3scan/atsc3scan [--limit N] [--slt] [--sls] [--objs] [--mmt] ../samples/ch11-25-slt.pcap
build/debug/tools/stltp2alp/stltp2alp [--bbp out.bbp.pcap] [--plp N] [--start S] [--duration S] in.pcap out.pcap
build/debug/tools/routedump/routedump [--service ID] [--start S] [--duration S] [--no-nrt] in.pcap out-dir
```

`atsc3scan` is the inventory of a capture of any form: the format verdict, BBP and ALP header facts, the
LMTs, the LLS tables, the UDP flow table, every LCT flow with its object completeness, every MMTP flow with
its signaling, and the flows nothing claimed. It scans the first N records with `--limit`, prints the SLT XML
with `--slt`, the SLS objects with `--sls`, every object with `--objs` and the MP tables and ATSC messages with
`--mmt`; it exits with status 3 when a capture carries no ATSC 3.0 content. `stltp2alp` turns an STLTP capture
into a DLT 289 ALP capture. `routedump` extracts every ROUTE service of a capture into
`out-dir/bsid-<bsid>/<serviceId>-<name>/` with a static `manifest.mpd` that VLC plays offline, the segments,
the SLS fragments and the NRT files.

## Tests

```bash
cd implementation
ctest --preset debug                      # unit, golden (the corpus inventory numbers), roundtrip, tools, bench, docs
ctest --preset debug -L unit              # one label; others: golden, roundtrip, tools, bench, http, docs, e2e-vlc
build/debug/tests/unit/atsc3_unit "[lct]"           # one Catch2 tag or test case
build/debug/tests/unit/atsc3_unit "~[corpus]"       # fast loop: skip the tests that read ../samples
ctest --preset asan                       # run before declaring a layer done
ctest --preset web -L http                # REST contract tests against an in-process daemon
ctest --preset vlc -R e2e.vlc             # VLC end-to-end suite, real-time replays, about 5 minutes
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

Formatting and linting: `scripts/format.sh` (clang-format, prettier) and `scripts/lint.sh`
(clang-tidy over `build/debug`, eslint).

## Keeping this file current

This README must change in the same edit as anything it describes: presets, options, targets,
program names and flags, test labels, scripts and paths. The ctest entry `docs.readme` (label
`docs`, script `implementation/scripts/readme-check.py`) fails when a preset, option, flag, MRL
key, test label, test name, npm script, Playwright project or path named in a code block here no
longer exists, so a stale README breaks the default test run. Prose is not checked; keep it short
and update it by hand.
