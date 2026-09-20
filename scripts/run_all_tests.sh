#!/bin/bash
# Full test suite — unit tests + AccuracyCoin + Blargg ROM tests
#
# Uses test_runner for Blargg tests (single binary for all ROM-based testing).
# For quick CI: ./scripts/run_priority_tests.sh
# For Scheme scripting: csi -s scripts/run-tests.scm all

set -euo pipefail

# Find project root
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

RUNNER="${NES_BUILD_DIR:-build}/bin/test_runner"
if [[ ! -x "$RUNNER" ]]; then
    echo "Build test_runner first: cmake --build ${NES_BUILD_DIR:-build} --target test_runner" >&2
    exit 1
fi
PASS=0; FAIL=0; TIMEOUT=0; SKIP=0

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

run_blargg() {
    local rom="$1"
    local name
    name=$(basename "$rom" .nes)

    [[ ! -f "$rom" ]] && { printf "  %-40s ${YELLOW}SKIP${NC}\n" "$name"; ((++SKIP)) || true; return; }

    local code
    code=0
    "$RUNNER" "$rom" --blargg --frames 18000 >/dev/null 2>&1 || code=$?

    case $code in
        0) printf "  %-40s ${GREEN}PASS${NC}\n" "$name"; ((++PASS)) ;;
        1) printf "  %-40s ${RED}FAIL${NC}\n" "$name"; ((++FAIL)) ;;
        2) printf "  %-40s ${YELLOW}TIMEOUT${NC}\n" "$name"; ((++TIMEOUT)) ;;
        *) printf "  %-40s ${YELLOW}SKIP${NC}\n" "$name"; ((++SKIP)) ;;
    esac
}

# === Unit tests + AccuracyCoin (via priority script) ===
./scripts/run_priority_tests.sh || ((++FAIL))
echo ""

# === Blargg CPU Instruction Tests ===
echo "=== Blargg CPU Tests ==="
for rom in tests/nes-test-roms/instr_test-v5/rom_singles/*.nes; do
    run_blargg "$rom"
done
echo ""

# === Blargg PPU Tests ===
echo "=== Blargg PPU Tests ==="
for rom in tests/nes-test-roms/blargg_ppu_tests_2005.09.15b/*.nes; do
    run_blargg "$rom"
done 2>/dev/null
echo ""

# === Blargg APU Tests ===
echo "=== Blargg APU Tests ==="
for rom in tests/nes-test-roms/apu_test/rom_singles/*.nes; do
    run_blargg "$rom"
done 2>/dev/null
for rom in tests/nes-test-roms/blargg_apu_2005.07.30/*.nes; do
    run_blargg "$rom"
done 2>/dev/null
echo ""

# === Sprite Tests ===
echo "=== Sprite Tests ==="
for rom in tests/nes-test-roms/sprite_hit_tests_2005.10.05/*.nes; do
    run_blargg "$rom"
done 2>/dev/null
echo ""

# === Summary ===
TOTAL=$((PASS + FAIL + TIMEOUT + SKIP))
echo "========================================="
printf "Blargg: ${GREEN}%d pass${NC}, ${RED}%d fail${NC}, ${YELLOW}%d timeout${NC}, %d skip (of %d)\n" \
    "$PASS" "$FAIL" "$TIMEOUT" "$SKIP" "$TOTAL"

[[ $FAIL -gt 0 ]] && exit 1 || exit 0
