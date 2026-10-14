#!/usr/bin/env bash
# clang-tidy over compile_commands.json and eslint for the frontend.
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD=${1:-build/debug}
run-clang-tidy -p "$BUILD" -quiet tuners tools src integrations
( cd integrations/web/frontend && npx eslint . --ext .ts,.vue )
