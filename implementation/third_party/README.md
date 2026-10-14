# Third-party dependencies

Preferred source is the system package; `cmake/Dependencies.cmake` falls back to pinned
FetchContent downloads when `ATSC3_FETCH_DEPS=ON`. For offline builds, place source checkouts of
the same versions here and point `FETCHCONTENT_SOURCE_DIR_<NAME>` at them.

| Dependency | Version | Licence | Used by |
|------------|---------|---------|---------|
| zlib | system | zlib | core (gzip) |
| pugixml | 1.14 | MIT | core (XML) |
| Catch2 | 3.5.2 | BSL-1.0 | tests |
| cpp-httplib | 0.15.3 | MIT | integrations/web |
| IXWebSocket | 11.4.5 | BSD-3 | integrations/web |
| OpenSSL | system ≥ 1.1 | Apache-2.0 | src/security (optional) |
| libpcap | system | BSD | live capture (optional) |
| VLC plugin SDK | 3.0 line (distribution's `libvlccore-dev`, `pkg-config vlc-plugin`) | LGPL-2.1 | integrations/vlc |
