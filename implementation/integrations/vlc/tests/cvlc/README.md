# cvlc log-driven tests (spec 5.19.5, harness 1)

`run.sh <plugin-dir> <capture> <mrl-options> <seconds> <expected.json> [vlc-binary]` plays
`atsc3://<capture>?<mrl-options>` headless (`-I dummy --vout dummy --aout dummy -vv --run-time`) with the
plugin directory in `VLC_PLUGIN_PATH` and runs `check_log.py` over the log. Expectations live in
`../expected/*.json` (programs, selected service, elementary streams, decoder lines, fragment counts, EPG
events, alerts, EOF, forbidden lines). Exit 0 pass, 1 fail, 77 skipped. Any VLC 3.0.x binary works, the
installed `cvlc` or the `bin/vlc` of a VLC built from source (then put a flat directory of its
`*_plugin.so` files into `VLC_PLUGIN_PATH` as well).
