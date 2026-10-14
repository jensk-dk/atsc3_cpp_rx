#!/usr/bin/env bash
set -euo pipefail
PRESET=${1:-debug}
cd "$(dirname "$0")/../.."
scripts/corpus-check.sh
ctest --preset "$PRESET" -L golden
