#!/bin/bash
# Run the registered core tests, including the complete AccuracyCoin suite.
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"
exec ctest --test-dir "${NES_BUILD_DIR:-build}" --output-on-failure \
    -R '^(cpu_tests|cpu_cycle_tests|ppu_tests|nes_tests|rom_tests|accuracy_coin_tests)$'
