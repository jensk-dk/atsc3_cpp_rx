#!/usr/bin/env bash
# CI: configure + build + unit tests for the given preset (default debug).
set -euo pipefail
PRESET=${1:-debug}
cd "$(dirname "$0")/../.."
cmake --preset "$PRESET"
cmake --build --preset "$PRESET" -j"$(nproc)"
ctest --preset "$PRESET" -L 'unit|roundtrip|bench'
