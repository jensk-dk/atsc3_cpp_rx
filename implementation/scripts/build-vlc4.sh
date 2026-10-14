#!/usr/bin/env bash
# Builds the VLC 4 line from source to obtain libvlccore + vlc-plugin.pc for integrations/vlc (spec 5.19.4).
# Usage: scripts/build-vlc4.sh [prefix] ; then: PKG_CONFIG_PATH=<prefix>/lib/pkgconfig cmake --preset vlc
set -euo pipefail
PREFIX=${1:-$HOME/.local/vlc4}
WORK=${WORK:-$(mktemp -d)}
git clone --depth 1 https://code.videolan.org/videolan/vlc.git "$WORK/vlc"
cd "$WORK/vlc"
./bootstrap
./configure --prefix="$PREFIX" --disable-lua --disable-a52 --disable-qt --disable-skins2 --enable-debug
make -j"$(nproc)"
make install
echo "VLC 4 installed in $PREFIX; export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig"
