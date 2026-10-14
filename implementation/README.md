# atsc3 — implementation

C++17 implementation of the ATSC 3.0 client-side stack specified in `../ATSC3_CLIENT_STACK_SPEC.md`.
The quick start (build, run, tests for every target) is `../README.md`; layout and build rules are spec Section 8.

```bash
cmake --preset debug            # core + tuners + tools + tests (Catch2 and pugixml fetched if not installed)
cmake --build --preset debug -j
ctest --preset debug            # labels: unit, golden + tools (need ../samples), roundtrip, bench, docs
build/debug/tests/unit/atsc3_unit "[core]"      # one tag / one test case
cmake --preset web | vlc | full # integrations/: web server + atsc3d (+ frontend), VLC plugin (libvlccore-dev, see spec 5.19.4), everything
```

Corpus: `-DATSC3_CORPUS_DIR=…` (default `../samples`).
