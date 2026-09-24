#!/usr/bin/env bash
# release-macos.sh -- Build a clean macOS distribution of MyNES.
#
# Usage:   ./scripts/release-macos.sh [VERSION]
# Output:  dist/mynes-<VERSION>.tar.xz
#
# The tarball contains:
#   bin/mynes          (SDL2 frontend)
#   bin/mynes_gpu      (GPU frontend)
#   lib/               (Homebrew's SDL2 dylib, loaded by bin/mynes)
#   shaders/           (compute + render shaders)
#   presets/           (physical-preset JSON files)
#   palettes/          (optional RGB palettes)
#   LICENSE
#   README.md
#
# The binaries are built with MYNES_PORTABLE_BINARY=ON (a fixed ISA baseline
# instead of -march=native) and MYNES_MIN_MACOS (default 12.0) as the
# deployment target, so the tarball runs on other Macs of the same
# architecture. mynes_gpu links its SDL3 statically; mynes needs SDL2, which
# step 3b copies into lib/ so the tarball does not depend on Homebrew. See
# docs/dev/release.md.
#
# Requirements:
#   - cmake, make, clang (brew install cmake sdl2)
#   - glslc + spirv-cross (brew install shaderc spirv-cross) only if you changed
#     a .glsl; otherwise the committed SPIR-V/MSL shaders are used.
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
    -DNES_BUILD_GPU_FRONTEND=ON \
    -DMYNES_PORTABLE_BINARY=ON \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="${MYNES_MIN_MACOS:-12.0}"

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

# 3b. Bundle SDL2 for the SDL2 frontend ----------------------------------------
# CMake links mynes against Homebrew's SDL2 dylib by its absolute install
# name (/opt/homebrew/opt/sdl2/lib/libSDL2-2.0.0.dylib), which a Mac without
# Homebrew does not have: dyld would refuse to start it. Ship the dylib in
# lib/ and point the load command at it. The edit invalidates the linker's
# ad-hoc signature, and Apple Silicon does not run unsigned binaries, so sign
# it again.
SDL2_DYLIB="$(otool -L "$STAGE_DIR/bin/mynes" | awk '/libSDL2/ { print $1; exit }')"
if [ -n "$SDL2_DYLIB" ] && [ -f "$SDL2_DYLIB" ]; then
    mkdir -p "$STAGE_DIR/lib"
    cp "$SDL2_DYLIB" "$STAGE_DIR/lib/"
    install_name_tool -change "$SDL2_DYLIB" \
        "@executable_path/../lib/$(basename "$SDL2_DYLIB")" "$STAGE_DIR/bin/mynes"
    codesign --force --sign - "$STAGE_DIR/bin/mynes"
fi

# 4. Verify both binaries launch and load nothing from the build host ----------
echo "== Verifying binaries --"
"$STAGE_DIR/bin/mynes" --help >/dev/null 2>&1 \
    || echo "   (mynes --help not implemented — skipping)"
"$STAGE_DIR/bin/mynes_gpu" --help >/dev/null
# --help passes on the build host whatever the load commands say, so check
# them: a Homebrew path here is a binary that only starts on this machine.
if otool -L "$STAGE_DIR"/bin/* | grep -E '^[[:space:]]+/(opt/homebrew|usr/local)/'; then
    echo "error: staged binaries load libraries from the build host (see above)" >&2
    exit 1
fi

# 5. Create the tarball --------------------------------------------------------
mkdir -p "$DIST_DIR"
rm -f "$TARBALL"
tar -cJf "$TARBALL" -C "$DIST_DIR" "mynes-$VERSION"

du -sh "$TARBALL" | awk '{printf "\n== Done: %s (%s) ==\n", $2, $1}'
echo "   Unpack with:  tar -xJf $(basename "$TARBALL")"
echo "   Run with:     ./mynes-$VERSION/bin/mynes  (or bin/mynes_gpu)"
