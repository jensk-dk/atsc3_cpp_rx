#!/usr/bin/env bash
# CI: web and VLC end-to-end suites (needs corpus, node, and for vlc: cvlc + python-vlc).
set -euo pipefail
cd "$(dirname "$0")/../.."
scripts/corpus-check.sh
ctest --preset full -L 'e2e-web|e2e-vlc'
