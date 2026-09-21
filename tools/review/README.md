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

## Beam measurements

Create a 256×240 byte array filled with PPU code $0F. At x=32,96,160 place
48×48 patches from y=64 through 111, and 48×1 strokes at y=144, using codes
$00,$10,$20 respectively. Render with `--simulate-frame pattern.bin` for each of
`sony_pvm_14l2`, `jvc_d_series_2000`, `toshiba_14af43`, `stass_favourite`, at 4K,
frames 60 and 61, with screenshot path `<preset>.ppm` in one directory.

Run `python3 tools/review/measure_beam.py CAPTURE_DIRECTORY` with NumPy installed.
This averages the paired final linear PFM captures, takes luminance across the
central 96 columns of each isolated stroke, and linearly interpolates its
half-maximum crossings. The result describes final rendered light, not raw gun
spot parameters or a hardware calibration. PNG presentation uses exposure 0.7
followed by sRGB encoding. No resizing is applied to the published detail crops.
