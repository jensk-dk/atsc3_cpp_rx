#!/usr/bin/env bash
# clang-format for C++ (tools, src, include, integrations, tests) and prettier for the frontend.
set -euo pipefail
cd "$(dirname "$0")/.."
find tuners tools src include integrations tests -name '*.[ch]pp' -print0 | xargs -0 clang-format -i
( cd integrations/web/frontend && npx prettier --write src e2e )
