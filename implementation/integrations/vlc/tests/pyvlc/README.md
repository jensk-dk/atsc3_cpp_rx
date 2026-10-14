# libvlc-driven tests (spec 5.19.5, harness 2)

`libvlc_test.py <plugin-dir> <capture> [service]` binds `libvlc.so.5` through ctypes (no python-vlc
needed): plays the MRL, waits for Playing, checks that the time advances, lists the video/audio/subtitle
tracks and the media statistics (decoded and displayed pictures), pauses and resumes, and with
`ALT_PROGRAM=<id>` opens a second player with `:program=<id>` to switch service. Exit 0 pass, 1 fail,
77 skipped. VLC 3.0 line; a VLC 4 libvlc would expose programs and EPG directly.
