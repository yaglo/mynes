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
PASS=0; FAIL=0; TIMEOUT=0; NOREPORT=0; ERROR=0; SKIP=0

# ROM sets that predate blargg's $6000 status protocol: they print their
# result on screen only, so test_runner never sees one and they always run
# to the frame limit. Their timeout is expected and does not fail the run;
# a timeout anywhere else is a hung test and does.
NON_REPORTING_DIRS=(
    tests/nes-test-roms/blargg_ppu_tests_2005.09.15b
    tests/nes-test-roms/blargg_apu_2005.07.30
    tests/nes-test-roms/sprite_hit_tests_2005.10.05
)

reports_via_6000() {
    local dir
    dir=$(dirname "$1")
    local d
    for d in "${NON_REPORTING_DIRS[@]}"; do
        [[ "$dir" == "$d" ]] && return 1
    done
    return 0
}

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

run_blargg() {
    local rom="$1"
    local name
    name=$(basename "$rom" .nes)

    [[ ! -f "$rom" ]] && { printf "  %-40s ${YELLOW}SKIP${NC} (ROM missing)\n" "$name"; ((++SKIP)) || true; return; }

    local code
    code=0
    "$RUNNER" "$rom" --blargg --frames 18000 >/dev/null 2>&1 || code=$?

    # Anything but 0/1/2 is a runner problem, not a result: 3 is a ROM
    # load error, 127 a missing binary, 128+N a crash on signal N.
    case $code in
        0) printf "  %-40s ${GREEN}PASS${NC}\n" "$name"; ((++PASS)) ;;
        1) printf "  %-40s ${RED}FAIL${NC}\n" "$name"; ((++FAIL)) ;;
        2)
            if reports_via_6000 "$rom"; then
                printf "  %-40s ${RED}TIMEOUT${NC}\n" "$name"; ((++TIMEOUT))
            else
                printf "  %-40s ${YELLOW}NO REPORT${NC} (no \$6000 status)\n" "$name"; ((++NOREPORT))
            fi ;;
        *) printf "  %-40s ${RED}ERROR${NC} (exit %d)\n" "$name" "$code"; ((++ERROR)) ;;
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

# === Blargg Reset Tests (test_runner presses reset on request) ===
echo "=== Blargg Reset Tests ==="
for rom in tests/nes-test-roms/apu_reset/*.nes \
           tests/nes-test-roms/cpu_reset/ram_after_reset.nes; do
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
TOTAL=$((PASS + FAIL + TIMEOUT + ERROR + NOREPORT + SKIP))
echo "========================================="
printf "Blargg: ${GREEN}%d pass${NC}, ${RED}%d fail${NC}, ${RED}%d timeout${NC}, ${RED}%d error${NC}, ${YELLOW}%d no report${NC}, %d skip (of %d)\n" \
    "$PASS" "$FAIL" "$TIMEOUT" "$ERROR" "$NOREPORT" "$SKIP" "$TOTAL"

[[ $((FAIL + TIMEOUT + ERROR)) -gt 0 ]] && exit 1 || exit 0
