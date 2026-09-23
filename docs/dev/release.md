# Releasing MyNES

How to turn a checkout into something other people can run: portable
binaries, the macOS app bundle and disk image, the Linux tarball, and the
Apple code-signing and notarization steps.

## Portable binaries

Release builds use `-march=native` by default, which is right for a build
you run on the machine that made it and wrong for anything you hand to
someone else: a binary tuned for one CPU dies with `Illegal instruction`
on an older one. Every release script therefore configures with

```bash
-DMYNES_PORTABLE_BINARY=ON
```

which replaces `-march=native` with a fixed baseline:

| Target            | Flag                 | Runs on                                   |
|-------------------|----------------------|-------------------------------------------|
| x86-64            | `-march=x86-64-v3`   | Haswell (2013) and later: AVX2 + FMA, which the NTSC FIR loops use |
| Apple Silicon     | `-mcpu=apple-m1`     | Every Apple Silicon Mac                   |
| anything else     | compiler default     | Universal or unusual builds get no flag   |

The CMake summary prints which one was chosen:

```
--   Portable binary: ON (-march=x86-64-v3: x86-64-v3: AVX2/FMA, Haswell 2013 and later)
```

Portable Linux and macOS x86-64 builds do not run on CPUs without AVX2.
That is the same requirement the composite pipeline's SIMD loops already
have; there is no scalar fallback to ship.

## Version number

The version lives in one place, `project(MyNES VERSION x.y.z)` in
`CMakeLists.txt`. `make-macos-app.sh` reads it from the configured build's
`CMakeCache.txt` (`CMAKE_PROJECT_VERSION`) for `CFBundleShortVersionString`
and the `.dmg` file name; the tarball scripts take an explicit argument and
fall back to `git describe`. Bump it before tagging:

```bash
git tag -a v0.3.0 -m "MyNES 0.3.0"
```

## macOS

### Tarball

```bash
./scripts/release-macos.sh [VERSION]
# -> dist/mynes-<VERSION>-macos-<arch>.tar.xz
```

Builds `mynes` (SDL2) and `mynes_gpu` with `MYNES_PORTABLE_BINARY=ON` and
`CMAKE_OSX_DEPLOYMENT_TARGET` set to `MYNES_MIN_MACOS` (default `12.0`),
strips them, and stages `bin/`, `shaders/`, `presets/`, `palettes/`,
`README.md` and `LICENSE`. The frontends find their resources relative to
the executable, so the tree can be unpacked anywhere. `mynes_gpu` links its
SDL3 statically; `mynes` is linked against Homebrew's SDL2 by absolute path,
so the script copies that dylib into `lib/`, points the load command at
`@executable_path/../lib/`, re-signs the binary ad hoc, and then fails if
either binary still loads anything from `/opt/homebrew` or `/usr/local`.
Homebrew builds SDL2 for the host's macOS release, so `mynes` may need a
newer macOS than `MYNES_MIN_MACOS`; `mynes_gpu` is the one held to it.

### App bundle and disk image

```bash
./scripts/make-macos-app.sh                # build, bundle, ad-hoc sign, .dmg
./scripts/make-macos-app.sh --dry-run      # print every step, runs on any OS
./scripts/make-macos-app.sh --no-build --build-dir build-release
./scripts/make-macos-app.sh --identity "Developer ID Application: Your Name (TEAMID)"
```

Produces `dist/MyNES.app` and `dist/MyNES-<VERSION>-macos-<arch>.dmg`. The
bundle layout is

```
MyNES.app/Contents/Info.plist        CFBundleIdentifier, version from CMake,
                                     LSMinimumSystemVersion, NSHighResolutionCapable,
                                     a document type for .nes
MyNES.app/Contents/MacOS/mynes_gpu
MyNES.app/Contents/Resources/shaders/{compute,render}
MyNES.app/Contents/Resources/presets
MyNES.app/Contents/Resources/palettes
```

`frontends/gpu/main.c` and `preset_apply.c` look for `../Resources/<dir>`
and `<dir>` next to `SDL_GetBasePath()` (which is `Contents/Resources/` for
a bundled app) in addition to the `../<dir>` used by build trees and
tarballs, so the same binary works in all three layouts.

`MYNES_BUNDLE_ID` overrides the bundle identifier (default
`com.github.yaglo.mynes`); `MYNES_MIN_MACOS` overrides the deployment
target and `LSMinimumSystemVersion` (default `12.0`).

The `.dmg` holds the app and an `Applications` symlink for drag-install.
Only `mynes_gpu` is bundled; the SDL2 frontend stays a command-line tool in
the tarball.

### Code signing

Without `--identity`, the script signs ad hoc:

```bash
codesign --force --deep --sign - dist/MyNES.app
```

Ad-hoc signatures satisfy the Apple Silicon requirement that every binary
be signed, but Gatekeeper still treats the app as unidentified. That is
fine for builds you run yourself or hand to people who trust you; see
"Opening an unsigned build" below for what they have to do.

For a distributable build you need a paid Apple Developer account and a
**Developer ID Application** certificate in your login keychain. Find its
name with

```bash
security find-identity -v -p codesigning
```

and pass it to the script. It then signs with the hardened runtime and a
secure timestamp, both of which notarization requires:

```bash
codesign --force --deep --options runtime --timestamp \
    --sign "Developer ID Application: Your Name (TEAMID)" dist/MyNES.app
codesign --verify --deep --strict dist/MyNES.app
```

The GPU frontend needs no entitlements: it uses SDL and Metal only. If you
ever add JIT or dynamic loading of unsigned code, the hardened runtime will
need the matching `com.apple.security.cs.*` entitlements.

### Notarization

Notarization uploads the disk image to Apple, which scans it and records
the result so Gatekeeper lets it through on first launch. It is done with
`notarytool`, which ships with Xcode 13 and later.

1. Store your credentials once. Create an app-specific password at
   <https://account.apple.com> (Sign-In and Security > App-Specific
   Passwords), then

   ```bash
   xcrun notarytool store-credentials mynes-notary \
       --apple-id you@example.com --team-id TEAMID --password <app-specific>
   ```

2. Submit the signed disk image and wait for the verdict:

   ```bash
   xcrun notarytool submit dist/MyNES-0.3.0-macos-arm64.dmg \
       --keychain-profile mynes-notary --wait
   ```

   `--wait` blocks until Apple answers, usually within a few minutes. On
   `Invalid`, fetch the reasons with
   `xcrun notarytool log <submission-id> --keychain-profile mynes-notary`.
   The usual causes are a binary signed without `--options runtime`, a
   missing `--timestamp`, or an unsigned file inside the bundle (the script
   deletes dotfiles from `Resources/` for that reason).

3. Staple the ticket so the app opens without a network round-trip:

   ```bash
   xcrun stapler staple dist/MyNES-0.3.0-macos-arm64.dmg
   xcrun stapler validate dist/MyNES-0.3.0-macos-arm64.dmg
   ```

   Stapling the `.dmg` covers the app inside it. If you distribute the bare
   `.app` (in a zip, say), staple the `.app` instead and zip it with
   `ditto -c -k --keepParent` so the resource fork survives.

4. Check the result the way Gatekeeper will:

   ```bash
   spctl --assess --type open --context context:primary-signature -v dist/MyNES-0.3.0-macos-arm64.dmg
   ```

### Opening an unsigned build

If you skipped notarization (ad-hoc signature, or a Developer ID
signature that was never submitted), macOS refuses the first launch with
"MyNES cannot be opened because the developer cannot be verified" or, on
macOS 15 and later, "Apple could not verify MyNES is free of malware".
Tell users to do one of the following, once:

- **Right-click (or Control-click) MyNES.app in Finder and choose Open**,
  then click **Open** in the dialog. This works up to macOS 14.
- On macOS 15 (Sequoia) and later the context-menu route is gone: open the
  app once (it will be blocked), then go to **System Settings > Privacy &
  Security**, scroll to the Security section, and click **Open Anyway**
  next to the MyNES message. Authenticate when asked.
- From a terminal, remove the quarantine attribute that Safari or Finder
  attached on download:

  ```bash
  xattr -dr com.apple.quarantine /Applications/MyNES.app
  ```

The tarball's command-line binaries are not quarantined when extracted
with `tar` in a terminal, so they run as is; if a browser unpacked the
archive, the same `xattr -dr` command on the extracted directory fixes it.

## Linux

```bash
./scripts/release-linux.sh [VERSION]
# -> dist/mynes-<VERSION>-linux-<arch>.tar.gz
```

Builds with `MYNES_PORTABLE_BINARY=ON` and, by default,
`MYNES_BUNDLED_SDL3=ON` so SDL3 is compiled in and linked statically: the
tarball then only needs the system's Vulkan loader, X11/Wayland client
libraries and audio server, not a particular SDL3 package. Building SDL
from source needs the usual development packages (on Debian/Ubuntu:
`build-essential git cmake pkg-config libvulkan-dev libx11-dev libxext-dev
libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev
libwayland-dev libxkbcommon-dev libdecor-0-dev libegl-dev libgl-dev
libpulse-dev libasound2-dev`, the list the README gives). Set
`MYNES_BUNDLED_SDL3=OFF` to link the distribution's SDL3 instead and
`MYNES_CMAKE_ARGS` to pass anything else through to CMake.

The tarball contains `bin/mynes_gpu` (and `bin/mynes` when SDL2 was
available), `shaders/`, `presets/`, `palettes/` and `LICENSE`. Run it from
anywhere: the binaries locate their resources relative to `bin/`.

There is no AppImage or Flatpak yet. A distribution package would install
`shaders/`, `presets/` and `palettes/` next to a `bin/` directory; the
lookup does not consult `/usr/share`.

## Hardware checks

The tests cannot press a gamepad button, feel input latency or look at a
panel. Run these on a Mac with one or two controllers before tagging.

Input:

- Plug a gamepad in while a game runs: "GAMEPAD CONNECTED" appears and the
  pad drives player 1. A second pad drives player 2. Unplug and plug in
  again: the notice repeats and the pad works.
- Guide opens the menu. The d-pad and left stick move through the menu and
  the ROM browser (South is Enter, East is back); nothing reaches the game
  while either is open. The stick registers at about half its travel.
- Player 2 plays on W/A/S/D with J, H, U and Y. Ctrl+D, Ctrl+T, Ctrl+B,
  Ctrl+A and Ctrl+L never press a player 2 button.
- Escape and M open the menu and never quit. Ctrl+Q and Game > Quit quit.

Play:

- Space pauses: the picture stays and audio stops; it resumes without a
  click.
- Holding ` or the right shoulder runs up to 8x with audio muted; audio
  comes back clean on release.
- F, Globe+F, F11 and Alt+Return toggle fullscreen at the panel's native
  mode; the performance overlay (V) reports NATIVE PANEL PIXELS. Globe+F in
  the ROM browser does not type an F.
- Cover the window with another app for 30 s, then minimize it: the music
  keeps its tempo.
- The pointer stays hidden over the picture, windowed and fullscreen.

Saves:

- The Legend of Zelda: register a name, save, quit, relaunch; the file is
  still there.
- F5 saves, F6 changes slot, F7 loads; states survive a restart.

Display:

- Render scale Auto on a heavy preset (vhs_sp_consumer) in fullscreen
  steps down only under sustained load, never after a resize or a
  fullscreen switch.
- Low latency on and off: no stutter on a 60 Hz panel.

SDL2 frontend (`bin/mynes`): gamepad hot-plug as above, F toggles
fullscreen, the pointer stays hidden.

Packages: `dist/MyNES.app` opens from Finder (right-click Open for an
ad-hoc build) and plays a ROM chosen in the browser.

## Checklist

1. `ctest` passes on macOS and in the Linux CI workflow.
2. The hardware checks above pass.
3. Bump `VERSION` in `CMakeLists.txt`; commit.
4. Regenerate shaders if any `.glsl` changed:
   `cmake --build build --target shaders_regenerate`, commit the `.spv`/`.msl`.
5. Tag, then run the release scripts on each platform.
6. macOS: sign with Developer ID, notarize, staple; or document the
   right-click workaround in the release notes.
7. Attach `dist/*.tar.*` and `dist/*.dmg` to the GitHub release.
