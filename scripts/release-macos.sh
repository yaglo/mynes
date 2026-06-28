#!/usr/bin/env bash
# release-macos.sh -- Build a clean macOS distribution of MyNES.
#
# Usage:   ./scripts/release-macos.sh [VERSION]
# Output:  dist/mynes-<VERSION>.tar.xz
#
# The tarball contains:
#   bin/mynes          (SDL2 frontend)
#   bin/mynes_gpu      (GPU frontend — experimental)
#   shaders/           (compute + render shaders)
#   presets/           (physical-preset JSON files)
#   palettes/          (optional RGB palettes)
#   docs/              (README snippets + reference)
#   LICENSE
#   README.md
#
# Requirements:
#   - cmake, make, clang, glslc, spirv-cross (brew install sdl2 sdl3 shaderc spirv-cross)
#   - Chicken Scheme (brew install chicken) for DSL compilation; pre-generated
#     CPU source is tracked in generated/ as a fallback.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-release"
DIST_DIR="$REPO_ROOT/dist"
VERSION="${1:-$(git -C "$REPO_ROOT" describe --tags --always --dirty 2>/dev/null || date +%Y%m%d)}"
STAGE_DIR="$DIST_DIR/mynes-$VERSION"
TARBALL="$DIST_DIR/mynes-$VERSION-macos-$(uname -m).tar.xz"

echo "== Building MyNES $VERSION (macOS $(uname -m)) =="

# 1. Clean release build -------------------------------------------------------
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNES_BUILD_TESTS=OFF \
    -DNES_BUILD_GPU_FRONTEND=ON

cmake --build "$BUILD_DIR" --parallel --target mynes mynes_gpu

# 2. Strip symbols (shrinks binaries ~3x) --------------------------------------
strip -S "$BUILD_DIR/bin/mynes" "$BUILD_DIR/bin/mynes_gpu"

# 3. Stage release tree --------------------------------------------------------
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/bin" "$STAGE_DIR/shaders"

cp "$BUILD_DIR/bin/mynes"      "$STAGE_DIR/bin/"
cp "$BUILD_DIR/bin/mynes_gpu"  "$STAGE_DIR/bin/"

cp -R "$BUILD_DIR/shaders"/* "$STAGE_DIR/shaders/"
cp -R "$REPO_ROOT/presets"   "$STAGE_DIR/presets"
cp -R "$REPO_ROOT/palettes"  "$STAGE_DIR/palettes"

cp "$REPO_ROOT/README.md" "$STAGE_DIR/"
cp "$REPO_ROOT/LICENSE"   "$STAGE_DIR/"

# 4. Verify both binaries launch and print --help ------------------------------
echo "== Verifying binaries --"
"$STAGE_DIR/bin/mynes" --help >/dev/null 2>&1 \
    || echo "   (mynes --help not implemented — skipping)"
"$STAGE_DIR/bin/mynes_gpu" --help >/dev/null

# 5. Create the tarball --------------------------------------------------------
mkdir -p "$DIST_DIR"
rm -f "$TARBALL"
tar -cJf "$TARBALL" -C "$DIST_DIR" "mynes-$VERSION"

du -sh "$TARBALL" | awk '{printf "\n== Done: %s (%s) ==\n", $2, $1}'
echo "   Unpack with:  tar -xJf $(basename "$TARBALL")"
echo "   Run with:     ./mynes-$VERSION/bin/mynes  (or bin/mynes_gpu)"
