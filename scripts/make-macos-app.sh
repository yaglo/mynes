#!/usr/bin/env bash
# make-macos-app.sh -- Package the GPU frontend as MyNES.app and a .dmg.
#
# Usage:   ./scripts/make-macos-app.sh [options]
# Output:  dist/MyNES.app
#          dist/MyNES-<VERSION>-macos-<arch>.dmg
#
# Options:
#   --dry-run          Print every step instead of running it (works on any OS).
#   --no-build         Reuse an existing build directory instead of rebuilding.
#   --build-dir DIR    Build directory (default: build-release, shared with
#                      release-macos.sh).
#   --identity ID      codesign identity. Default "-" (ad-hoc). Pass your
#                      "Developer ID Application: ..." certificate to produce a
#                      bundle that can be notarized; see docs/dev/release.md.
#
# Environment:
#   MYNES_MIN_MACOS    Deployment target and LSMinimumSystemVersion (default 12.0).
#   MYNES_BUNDLE_ID    CFBundleIdentifier (default com.github.yaglo.mynes).
#
# Bundle layout, matched by the resource lookup in frontends/gpu/main.c and
# preset_apply.c (they try ../Resources/<dir> and <dir> relative to
# SDL_GetBasePath, which is Contents/Resources/ for a bundled app):
#   MyNES.app/Contents/Info.plist
#   MyNES.app/Contents/MacOS/mynes_gpu
#   MyNES.app/Contents/Resources/shaders/{compute,render}
#   MyNES.app/Contents/Resources/presets
#   MyNES.app/Contents/Resources/palettes
#
# The version comes from CMake (CMAKE_PROJECT_VERSION in the build cache, or
# the VERSION in CMakeLists.txt before the first configure).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$REPO_ROOT/build-release"
DIST_DIR="$REPO_ROOT/dist"
DRY_RUN=0
DO_BUILD=1
IDENTITY="-"
MIN_MACOS="${MYNES_MIN_MACOS:-12.0}"
BUNDLE_ID="${MYNES_BUNDLE_ID:-com.github.yaglo.mynes}"

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)   DRY_RUN=1 ;;
        --no-build)  DO_BUILD=0 ;;
        --build-dir) shift; BUILD_DIR="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")" ;;
        --identity)  shift; IDENTITY="$1" ;;
        -h|--help)   sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "Unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# Every side effect goes through run/write_file so --dry-run can narrate the
# whole job on a machine without codesign or hdiutil.
run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+ %s\n' "$*"
    else
        "$@"
    fi
}

write_file() {
    local path="$1"
    local content="$2"
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+ write %s:\n' "$path"
        printf '%s\n' "$content" | sed 's/^/    /'
    else
        printf '%s\n' "$content" > "$path"
    fi
}

# Version from CMake: the configured cache if there is one, else CMakeLists.txt.
project_version() {
    local v=""
    if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
        v="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$BUILD_DIR/CMakeCache.txt")"
    fi
    if [ -z "$v" ]; then
        v="$(sed -n 's/^[[:space:]]*VERSION[[:space:]]\{1,\}\([0-9][0-9.]*\).*/\1/p' \
             "$REPO_ROOT/CMakeLists.txt" | head -n 1)"
    fi
    [ -n "$v" ] || { echo "Cannot determine the project version" >&2; exit 1; }
    printf '%s' "$v"
}

if [ "$DRY_RUN" -eq 0 ] && [ "$(uname -s)" != "Darwin" ]; then
    echo "make-macos-app.sh builds a macOS bundle; use --dry-run elsewhere" >&2
    exit 1
fi

ARCH="$(uname -m)"
VERSION="$(project_version)"
APP="$DIST_DIR/MyNES.app"
CONTENTS="$APP/Contents"
STAGE="$DIST_DIR/dmg-root"
DMG="$DIST_DIR/MyNES-$VERSION-macos-$ARCH.dmg"

echo "== Packaging MyNES $VERSION as MyNES.app (macOS $ARCH, min $MIN_MACOS) =="

# 1. Portable release build ----------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
    run rm -rf "$BUILD_DIR"
    run cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNES_BUILD_TESTS=OFF \
        -DNES_BUILD_GPU_FRONTEND=ON \
        -DMYNES_PORTABLE_BINARY=ON \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS"
    run cmake --build "$BUILD_DIR" --parallel --target mynes_gpu
    # The cache exists now; re-read so a VERSION bump in CMakeLists.txt wins.
    VERSION="$(project_version)"
    DMG="$DIST_DIR/MyNES-$VERSION-macos-$ARCH.dmg"
elif [ "$DRY_RUN" -eq 0 ] && [ ! -x "$BUILD_DIR/bin/mynes_gpu" ]; then
    echo "No mynes_gpu in $BUILD_DIR; drop --no-build or pass --build-dir" >&2
    exit 1
fi

# 2. Bundle skeleton ------------------------------------------------------------
run rm -rf "$APP" "$STAGE"
run mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources"
run cp "$BUILD_DIR/bin/mynes_gpu" "$CONTENTS/MacOS/mynes_gpu"
run strip -S "$CONTENTS/MacOS/mynes_gpu"
run cp -R "$BUILD_DIR/shaders" "$CONTENTS/Resources/shaders"
run cp -R "$REPO_ROOT/presets"  "$CONTENTS/Resources/presets"
run cp -R "$REPO_ROOT/palettes" "$CONTENTS/Resources/palettes"
# Compile stamps and .DS_Store files must not end up inside a signed bundle.
run find "$CONTENTS/Resources" -name '.*' -type f -delete

# 3. Info.plist -----------------------------------------------------------------
# The document type lets Finder open .nes files with MyNES; the frontend gets
# them as SDL_EVENT_DROP_FILE. .fds stays out: the ROM browser lists it, but
# the loader reads only iNES images and rejects every .fds file.
write_file "$CONTENTS/Info.plist" "$(cat <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleDevelopmentRegion</key>
    <string>en</string>
    <key>CFBundleExecutable</key>
    <string>mynes_gpu</string>
    <key>CFBundleIdentifier</key>
    <string>$BUNDLE_ID</string>
    <key>CFBundleInfoDictionaryVersion</key>
    <string>6.0</string>
    <key>CFBundleName</key>
    <string>MyNES</string>
    <key>CFBundleDisplayName</key>
    <string>MyNES</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleShortVersionString</key>
    <string>$VERSION</string>
    <key>CFBundleVersion</key>
    <string>$VERSION</string>
    <key>LSMinimumSystemVersion</key>
    <string>$MIN_MACOS</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.games</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key>
    <true/>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key>
            <string>NES ROM</string>
            <key>CFBundleTypeRole</key>
            <string>Viewer</string>
            <key>LSHandlerRank</key>
            <string>Alternate</string>
            <key>LSItemContentTypes</key>
            <array>
                <string>$BUNDLE_ID.rom</string>
            </array>
        </dict>
    </array>
    <key>UTImportedTypeDeclarations</key>
    <array>
        <dict>
            <key>UTTypeIdentifier</key>
            <string>$BUNDLE_ID.rom</string>
            <key>UTTypeDescription</key>
            <string>NES ROM image (iNES)</string>
            <key>UTTypeConformsTo</key>
            <array>
                <string>public.data</string>
            </array>
            <key>UTTypeTagSpecification</key>
            <dict>
                <key>public.filename-extension</key>
                <array>
                    <string>nes</string>
                </array>
            </dict>
        </dict>
    </array>
</dict>
</plist>
EOF
)"
write_file "$CONTENTS/PkgInfo" "APPL????"

# 4. Sign ---------------------------------------------------------------------
# Ad-hoc signing is enough for a bundle to launch on Apple Silicon. A real
# Developer ID needs the hardened runtime and a secure timestamp before
# notarytool accepts it.
if [ "$IDENTITY" = "-" ]; then
    run codesign --force --deep --sign - "$APP"
else
    run codesign --force --deep --options runtime --timestamp --sign "$IDENTITY" "$APP"
fi
run codesign --verify --deep --strict "$APP"

# 5. Disk image ---------------------------------------------------------------
run mkdir -p "$STAGE"
run cp -R "$APP" "$STAGE/MyNES.app"
run ln -s /Applications "$STAGE/Applications"
run rm -f "$DMG"
run hdiutil create -volname "MyNES $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
run rm -rf "$STAGE"
if [ "$IDENTITY" != "-" ]; then
    run codesign --force --timestamp --sign "$IDENTITY" "$DMG"
fi

echo ""
echo "== Done: $DMG =="
if [ "$IDENTITY" = "-" ]; then
    echo "   Ad-hoc signed: users open it once with right-click > Open (Gatekeeper)."
    echo "   For a notarized build pass --identity and follow docs/dev/release.md."
else
    echo "   Notarize with:  xcrun notarytool submit \"$DMG\" --keychain-profile <profile> --wait"
    echo "   Then staple:    xcrun stapler staple \"$DMG\""
fi
