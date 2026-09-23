#!/usr/bin/env bash
# release-linux.sh -- Build a portable Linux distribution of MyNES.
#
# Usage:   ./scripts/release-linux.sh [VERSION]
# Output:  dist/mynes-<VERSION>-linux-<arch>.tar.gz
#
# The tarball contains:
#   bin/mynes_gpu      (GPU frontend)
#   bin/mynes          (SDL2 frontend, when SDL2 was found)
#   shaders/           (compute + render shaders)
#   presets/           (physical-preset JSON files)
#   palettes/          (RGB palettes)
#   LICENSE
#
# The binaries are built with MYNES_PORTABLE_BINARY=ON (x86-64-v3 on x86-64,
# no -march=native) so they run on other machines. SDL3 is built in and
# linked statically by default (MYNES_BUNDLED_SDL3=ON) so the tarball does
# not depend on the target distribution's SDL3; that needs the X11/Wayland
# development packages (on Debian/Ubuntu the apt list in README.md, from
# build-essential and pkg-config to libpulse-dev and libasound2-dev). Set
# MYNES_BUNDLED_SDL3=OFF to link the system
# SDL3 instead, and MYNES_CMAKE_ARGS for anything else (e.g. -DSDL3_DIR=...).
#
# Requirements: cmake, a C compiler, tar; glslc + spirv-cross only if you
# changed a .glsl (the committed SPIR-V/MSL shaders are used otherwise).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${MYNES_BUILD_DIR:-$REPO_ROOT/build-release}"
DIST_DIR="$REPO_ROOT/dist"
VERSION="${1:-$(git -C "$REPO_ROOT" describe --tags --always --dirty 2>/dev/null || date +%Y%m%d)}"
ARCH="$(uname -m)"
STAGE_DIR="$DIST_DIR/mynes-$VERSION"
TARBALL="$DIST_DIR/mynes-$VERSION-linux-$ARCH.tar.gz"

echo "== Building MyNES $VERSION (Linux $ARCH, portable) =="

# 1. Clean portable release build ---------------------------------------------
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
# shellcheck disable=SC2086  # MYNES_CMAKE_ARGS is a list of extra arguments
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNES_BUILD_TESTS=OFF \
    -DNES_BUILD_GPU_FRONTEND=ON \
    -DMYNES_PORTABLE_BINARY=ON \
    -DMYNES_BUNDLED_SDL3="${MYNES_BUNDLED_SDL3:-ON}" \
    ${MYNES_CMAKE_ARGS:-}

cmake --build "$BUILD_DIR" --parallel

[ -x "$BUILD_DIR/bin/mynes_gpu" ] || { echo "mynes_gpu was not built (SDL3 missing?)" >&2; exit 1; }

# 2. Stage release tree --------------------------------------------------------
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/bin"

cp "$BUILD_DIR/bin/mynes_gpu" "$STAGE_DIR/bin/"
if [ -x "$BUILD_DIR/bin/mynes" ]; then
    cp "$BUILD_DIR/bin/mynes" "$STAGE_DIR/bin/"
else
    echo "   (SDL2 frontend not built -- shipping mynes_gpu only)"
fi
strip "$STAGE_DIR"/bin/*

cp -R "$BUILD_DIR/shaders"   "$STAGE_DIR/shaders"
cp -R "$REPO_ROOT/presets"   "$STAGE_DIR/presets"
cp -R "$REPO_ROOT/palettes"  "$STAGE_DIR/palettes"
cp "$REPO_ROOT/LICENSE"      "$STAGE_DIR/"
# Build stamps are not part of the release.
find "$STAGE_DIR/shaders" -name '.*' -type f -delete

# 3. Verify the GPU frontend starts far enough to print --help -----------------
"$STAGE_DIR/bin/mynes_gpu" --help >/dev/null

# 4. Create the tarball --------------------------------------------------------
mkdir -p "$DIST_DIR"
rm -f "$TARBALL"
tar -czf "$TARBALL" -C "$DIST_DIR" "mynes-$VERSION"

du -sh "$TARBALL" | awk '{printf "\n== Done: %s (%s) ==\n", $2, $1}'
echo "   Unpack with:  tar -xzf $(basename "$TARBALL")"
echo "   Run with:     ./mynes-$VERSION/bin/mynes_gpu <rom.nes>"
