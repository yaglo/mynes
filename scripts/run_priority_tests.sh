#!/bin/bash
# Quick CI test — runs unit tests + AccuracyCoin
# Exit 0 = all pass, 1 = something failed
#
# For full test suite: ./scripts/run_all_tests.sh
# For Scheme scripting: csi -s scripts/run-tests.scm all

set -euo pipefail

# Find project root (directory containing CMakeLists.txt)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'
FAIL=0

echo "=== Priority Tests ==="

for test in test_cpu test_ppu test_nes; do
    name=$(echo "$test" | sed 's/test_//' | tr '[:lower:]' '[:upper:]')
    echo -n "$name... "
    if ./build/bin/$test 2>&1 | tail -1 | grep -q "passed"; then
        printf "${GREEN}PASS${NC}\n"
    else
        printf "${RED}FAIL${NC}\n"; ((FAIL++)) || true
    fi
done

echo -n "AccuracyCoin... "
result=$(./build/bin/accuracy_coin 2>&1 | grep "Summary")
if echo "$result" | grep -q "0 FAIL"; then
    printf "${GREEN}${result}${NC}\n"
else
    printf "${RED}${result}${NC}\n"; ((FAIL++)) || true
fi

echo ""
if [[ $FAIL -gt 0 ]]; then
    printf "${RED}$FAIL suite(s) failed${NC}\n"
    exit 1
else
    printf "${GREEN}All priority tests passed${NC}\n"
fi
