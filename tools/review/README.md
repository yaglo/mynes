# Reproducible visual captures

`MYNES_REVIEW_INPUT_SCRIPT` names a text controller replay. Each row contains an
emulated frame number and hexadecimal button mask, held until the next row.
Frames must be strictly increasing and nonzero; at most 128 rows are accepted.
A=01, B=02, Select=04, Start=08, Up=10, Down=20, Left=40, Right=80. For example:

```
1 00
3000 08
3060 00
```

Use `MYNES_REVIEW_NO_INPUT=1` and an isolated `XDG_CONFIG_HOME` when capturing.
The replay is parsed once before the worker starts. Ordinary playback reads no
replay file. `--screenshot-after N --screenshot-pair` records consecutive actual
source frames; `--screenshot-frames 240` records a four-second motion segment.
Use `--offscreen 3840x2880 --mask-alignment pixels` for a 4:3 game image
3840 pixels wide. A 3840×2160 canvas contains a narrower 2880×2160 viewport.
`darkwing-bridge.input` reproduces the bridge gameplay at frame 8000.
`castlevania-hall.input` reproduces the castle hall at frame 2500.

## Beam measurements

Create a 256×240 byte array filled with PPU code $0F. At x=32,96,160 place
48×48 patches from y=64 through 111, and 48×1 strokes at y=144, using codes
$00,$10,$20 respectively. Render with `--simulate-frame pattern.bin` for each of
`sony_pvm_14l2`, `jvc_d_series_2000`, `toshiba_14af43`, `stass_favourite`, at 4K,
frames 60 and 61, with screenshot path `<preset>.ppm` in one directory.

Run `python3 tools/review/measure_beam.py CAPTURE_DIRECTORY` with NumPy installed.
This measures each phase separately, taking luminance across the
central 96 columns of each isolated stroke, and linearly interpolates its
half-maximum crossings. The result describes final rendered light, not raw gun
spot parameters or a hardware calibration. PNG presentation uses exposure 0.7
followed by sRGB encoding. No resizing is applied to the published detail crops.


For synthetic PPU-code calibration patterns through the complete signal path:

```sh
python3 frontends/gpu/tests/capture_patterns.py build/bin/mynes_gpu /tmp/beam sony_pvm_14l2 --pattern beam --size 3840x2880
```

Patterns: `chart` (codes and detail), `beam` (isolated lines at several drive
levels), `recovery` (white/black patches in identical grey surrounds), `chroma`
(color boundaries and neutral detail), and `pluge` (NES black/grey codes, not a
standardized PLUGE voltage generator). The older measurement script uses the
specific 3840×2160 isolated-stroke fixture described above; it is not an
arbitrary screenshot analyser. Inspect native unaveraged crops for beam width
and consecutive phases for crawl. Averaging moving or displaced frames can
broaden a beam that was never actually broad in either frame.

## Refresh the published showcase

`refresh_showcase.py` captures the gallery, gameplay stills, supporting checks,
phase videos and isolated beam fixture from the current executable. It needs
NumPy, Pillow and `imageio-ffmpeg`, plus your local game files (filenames are
listed in the script). No ROMs or PPU fixtures are added to the repository.

```sh
python3 tools/review/refresh_showcase.py \
  --roms /path/to/roms --mario /path/to/mario.nes \
  --contra /path/to/contra-boss.bin --logs /tmp/mynes-showcase
```

Use `--sections gallery gameplay supporting motion vhs_motion beam` to select work.
Captures run sequentially with isolated settings. Full game stills have a
3840×2880 drawable; the beam measurement retains its 3840×2160 fixture. Stills
and native crops show one phase. Videos preserve consecutive source frames at
60.0988 fps; GIF previews use 20 ms frames for player compatibility. Linear
exposure is fixed at 0.6 with 1.6× simulated offscreen headroom. These SDR
exports do not reproduce the panel's live HDR luminance or BFI.

The logs directory records capture frame numbers and source/shader hashes.
Temporary raw frames are deleted after conversion; one 960×720 motion segment
needs about 2.5 GB before conversion. Do not benchmark alongside capture runs.
