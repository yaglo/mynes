# Showcase capture and encoding pipeline

`tools/showcase/showcase.py` records ten games on six television presets with
the GPU frontend's recorder, at every size the picture is shown at and in both
SDR and HDR, then encodes the site's stage and lens clips, stills and detail
crops, the README media and two feature clips, and installs the site files
into the [mynes-web](https://github.com/yaglo/mynes-web) checkout with a
merged `assets/hero/manifest.json`. Everything is driven by `shots.json`,
every command is logged, `--dry-run` prints the commands instead of running
them, and each output is checked (frame count, size, timebase, codec, colour
tags, byte limits) before the pipeline moves on. No ROMs, states or renders
are committed: `states/` and `out/` are ignored by git.

Requirements on the Mac:

- the GPU frontend built with the HDR recorder: `build/bin/mynes_gpu --help`
  must list `--record`, `--record-hdr`, `--record-headroom` and
  `--record-hdr-white`;
- ffmpeg 5.1 or newer with libx264, libx265, libsvtav1, libwebp and
  drawtext: `brew install ffmpeg-full` (the plain `ffmpeg` formula has no
  libwebp or drawtext);
- `avifenc` (`brew install libavif`);
- Python 3.10 or newer with Pillow and numpy (`pip3 install Pillow numpy`);
- your own ROMs.

ffmpeg and ffprobe are taken from `--ffmpeg`/`--ffprobe`, then
`MYNES_FFMPEG`/`MYNES_FFPROBE`, then `/opt/homebrew/opt/ffmpeg-full/bin`,
then `PATH`; an explicit ffmpeg brings the ffprobe beside it. Each run logs
the pick, and the recorder gets the same ffmpeg through `MYNES_FFMPEG`.

## What the outputs keep

- No resampling. The emulator aligns the aperture grille and the mask to
  output pixels, so each size is recorded by the emulator at that size.
  Filters convert pixel format and colour only, and crops cut whole pixels.
  The runner refuses any ffmpeg command with a filter that could change the
  picture size, in any graph option (`-vf`, `-filter`, `-lavfi`,
  `-filter_complex`, with or without a stream specifier), an output `-s` or
  `-video_size`, or a graph read from a file (`resampling_problem` in
  `pipeline/recipes.py`). No file is reduced either: a 1x display shows a
  detail crop 1:1 too, twice as large on the page as a 2x display does.
- The SDR and HDR renders of a clip come from the same state and replay, so
  their frames match one for one.
- Every video keeps the render's frames and timebase: `-fps_mode passthrough`
  and no `-r`.

## Workflow

Global options go before the subcommand; `install` and `all` take their own
options after it. Relative paths (`--roms`, `--states`, `--out`, `--binary`,
the site) are taken from the directory the command runs in, and the recorder
is given them as absolute paths.

1. Build the frontend with the HDR recorder and check the tools, ROMs and
   states:

   ```sh
   cmake --build build
   python3 tools/showcase/showcase.py --roms ~/roms check
   ```

   `check` validates `shots.json`, finds each ROM (recursive,
   case-insensitive globs; it prints what matched and refuses an ambiguous
   match), reads each ROM's TV system the way `src/nes/rom.h` does, lists
   the save states, and checks ffmpeg's encoders and filters, avifenc,
   Pillow, numpy, a caption font and the recorder's flags.

2. Create one save state per shot. The pipeline never records a shot whose
   state is missing; `states` prints what to do:

   ```sh
   python3 tools/showcase/showcase.py --roms ~/roms states
   ```

   For each shot: run `build/bin/mynes_gpu <ROM>`, play to the scene, press
   F5 with slot 1 selected, then copy
   `~/.config/mynes/states/<ROM name>-<CRC-32>.s1` to
   `tools/showcase/states/<shot>.s1` (the command prints the exact paths,
   CRC included). Pass `--config-dir` if your config lives elsewhere.

   Shots with a `replay` play scripted input from that state. To record your
   own, play the scene from the state while the frontend writes the script:

   ```sh
   build/bin/mynes_gpu --load-state tools/showcase/states/<shot>.s1 \
       --input-record tools/showcase/replays/<shot>.replay <ROM>
   ```

   and set `"record_after": 0` so the rows land on the frames they were
   recorded on.

3. Run one clip end to end first, then everything:

   ```sh
   python3 tools/showcase/showcase.py --roms ~/roms --shots super-mario-bros \
       --presets sony_pvm_14l2 all --site ~/src/mynes-web
   python3 tools/showcase/showcase.py --roms ~/roms all --site ~/src/mynes-web
   ```

   or stage by stage: `record` (serial, it uses the GPU), `--jobs 4 encode`,
   `features`, `install ~/src/mynes-web`. `--shots a,b` and `--presets x,y`
   narrow any stage, `--force` rebuilds outputs that are newer than their
   inputs (otherwise they are skipped), `--fast` trades quality for speed
   (see Outputs), `--dry-run` prints the commands. The log is
   `out/showcase.log`; each render has `sdr.record.log`/`hdr.record.log` and
   `sdr.record.json`/`hdr.record.json` beside it (command, frames, rate, the
   recorder's sidecar, SHA-256 of ROM, state, replay and preset). `record`
   runs a pass again when its command changes (size, length, start frame,
   HDR headroom and white, `record_args`), when the ROM, state, replay,
   preset or the recorder binary is newer, or when its last run failed: the `.record.json` is
   removed before the recorder starts and written only after the checks
   pass.

   `all` checks the site directory and its manifest before it records
   anything, and builds only the features whose shot and presets are all in
   the selection; the others need renders it does not record.

4. Install into the site (part of `all` when `--site` is given). Each complete
   clip is copied to `assets/hero/<shot>/<preset>/<WxH>/` and merged into
   `assets/hero/manifest.json`; entries the run did not produce are kept. A
   file is copied when the site's copy differs from the build in size or
   modification time (the copy keeps the build's time), so a file restored by
   `git checkout` is replaced too. `install` reads and merges the manifest
   before it copies anything, and stops without copying when the manifest does
   not parse, when an HDR render's `hdr.json` is missing or lacks `max_cll`
   and `max_fall`, or when no selected clip is complete (the manifest is then
   left as it was). It refuses, listing the 20 largest files, when the site's
   `assets/` would exceed `--budget-mb` (default 900; GitHub Pages sites must
   stay under 1 GB). `--with-crops` also copies the detail crops and adds
   `crop` to each clip's entry; a crop preset (see the shot list) installs
   its crop and nothing else.

5. Commit both repositories: `tools/showcase/shots.json` and any replays in
   mynes, `assets/hero/` in mynes-web (check `du -sh assets` first). The
   README media stay in `out/` and are copied by hand.

## Render sizes

Each clip (shot and preset) is recorded twice at each size, SDR and HDR,
and each pass runs until the last frame read from it. The README size is
recorded in SDR only: the README's WebPs are SDR, so nothing reads an HDR
render there.

| Size | SDR frames | HDR frames | Built from it |
|---|---|---|---|
| 1920x1440 | the whole shot | the whole shot | stage clips and poster for 2x displays |
| 960x720 | the whole shot | the whole shot | stage clips and poster for 1x displays |
| 3840x2880 | the whole shot for lens clips and features; otherwise up to the still frame, or the eight flicker frames for README presets | the whole shot for lens clips; otherwise up to the still frame | lens clips, stills, detail crops, flicker crop, feature clips |
| 1600x1200 | the first `readme_seconds` | none | `readme.webp`, presets in `readme` only |

A crop preset is recorded at 3840x2880 only, up to the still frame in both
passes.

Emulation from a state and a replay is deterministic, so a short render
holds the same first frames as the stage renders. Frames =
round(seconds x 60.0988) for NTSC, x 50.007 for PAL: 361 for a 6 s hero shot,
901 for a 15 s feature shot. The sizes, the HDR headroom (4.0) and the SDR
white inside HDR (203 nits) are `defaults.sizes` and `defaults.hdr` in
`shots.json`.

## Outputs

In `out/<shot>/<preset>/<WxH>/`:

| File | Size | What it is |
|---|---|---|
| `sdr.mov`, `sdr.json` | all | The recorder's SDR render (H.264 4:4:4, BT.601, untagged) and sidecar |
| `hdr.mov`, `hdr.json` | all | The HDR render (ProRes 4444, BT.2020 PQ) and sidecar with `max_cll` and `max_fall` |
| `stage-hdr-hevc.mp4` | stage | libx265 Main10, `-tag:v hvc1`, crf 18, preset slow, `hdr10=1:repeat-headers=1:colorprim=bt2020:transfer=smpte2084:colormatrix=bt2020nc`, `max-cll` from `hdr.json`, `master-display` P3-D65 1000 nits; AAC 128k |
| `stage-hdr-av1.mp4` | stage | libsvtav1 10-bit, crf 24, preset 6, `enable-hdr=1` with the same mastering display and content light; AAC 128k |
| `stage-sdr.mp4` | stage | libx264 High, crf 18, preset slow, 4:2:0, BT.709; AAC 128k |
| `poster.webp` | stage | Frame `thumbnail_frame` of the SDR render of this size, WebP quality 85 |
| `lens-hdr-hevc.mp4` | 3840x2880 | As `stage-hdr-hevc.mp4` at crf 14, no audio; lens shots only |
| `lens-hdr-av1.mp4` | 3840x2880 | As `stage-hdr-av1.mp4` at crf 20, no audio; lens shots only |
| `lens-sdr-hevc.mp4` | 3840x2880 | libx265 Main, crf 14, BT.709, no audio; lens shots only |
| `still-sdr.png` | 3840x2880 | Frame `thumbnail_frame` of the SDR render, lossless, with an sRGB chunk and no ICC profile (as every SDR PNG here) |
| `still-hdr.avif` | 3840x2880 | The same frame of the HDR render: `avifenc --cicp 9/16/9 --depth 10 --yuv 444 --range full -q 90 --clli MaxCLL,MaxFALL` from a 16-bit PQ PNG (`still-hdr.png`, with a cICP chunk), whose light levels give the `--clli` values. The PNG is deleted once the AVIF's size, format, colour tags and light levels check out |
| `crop-sdr.png` | crop | 1:1 detail crop of `still-sdr.png` |
| `crop-hdr.avif` | crop | The same crop of the HDR frame, made as `still-hdr.avif` (from `crop-hdr.png`, deleted afterwards) |
| `flicker.webp` | crop | README presets: eight consecutive frames from `flicker_frame` at 1:1, 125 ms each (8 fps), always lossless: lossy WebP is 4:2:0 and would halve the colour resolution the crop shows. Over 24 MB the encode fails; choose a smaller `flicker_crop` |
| `readme.webp` | 1600x1200 | README presets: every second frame of the first `readme_seconds` at 30 fps (33 and 34 ms frames), the highest quality from 90 to 30 that is under 10 MB, all candidates encoded at once |

`out/<shot>/<preset>/readme.json` records the WebP qualities and sizes and
the crop in render and NES pixels. In the README, embed `readme.webp` with
`width="800"` and `flicker.webp` at half its width (`embed_width` in
`readme.json`, 679 for the 1358-pixel crop), so a 2x display shows render
pixels 1:1. Nothing else is written for the README: the PNG copies and
gain-map JPEGs the pipeline used to make were never embedded. libwebp stores a run of identical
frames as one longer frame, so a still stretch of picture gives an animation
with fewer frames and the same length.

`--fast` encodes the HEVC files with `hevc_videotoolbox` (`-q:v` 70 for the
stage, 80 for lens clips; it writes no HDR10 mastering or light-level SEI),
SVT-AV1 at preset 10 and x264 at `veryfast`. Use it for previews. Each stage
and lens file has `<name>.encode.json` beside it with the command that made
it, and `encode` makes the file again when that command differs from the one
it would run, so a plain `encode` after `--fast` replaces the previews;
`install` warns about any file still made with `--fast`. Posters, stills,
README media and flicker crops keep `poster.encode.json`, `still.encode.json`,
`readme.encode.json` and `flicker.encode.json` with the frame, crop and
quality settings they were made with, and `encode` makes them again when a
setting in `shots.json` or `--flicker-scale` changes. Outputs made before
these records existed have none, so the first `encode` after updating makes
every poster, still, README WebP and flicker crop once more. A job whose output
fails its checks deletes what it wrote, so the next run builds it again and
`install` cannot copy it.

The encodes convert only pixel format and colour. HDR files go from the
render's 4:4:4 to 10-bit 4:2:0 (`format=yuv420p10le`) and carry their colour
on the frames (`setparams`, which libsvtav1 reads) and in the container. The
recorder's SDR files are BT.601 without tags (swscale's default when it
converts rgb24), so SDR outputs go through 16-bit RGB to BT.709 and are
tagged BT.709.

The HDR stills and crops take the render's frame as ffmpeg decodes it
(ProRes 4444 is `yuv444p12le`) and convert it to 16-bit R'G'B' in numpy
(`images.yuv_to_rgb48`): BT.2020 matrix, limited range expanded so that
Y' 940 of 10 bits is 65535. swscale's `rgb48le` output maps that white to
65280, which made every HDR still 256/257 darker in PQ code than the stage
video of the same frame, about 2.5% in nits.

Feature clips, `out/features/<id>/youtube.mp4`, are built at 3840x2880 from
the full-size SDR renders (libx264 High, crf 14, BT.709, AAC 192k) and are
not installed:

| Feature | Built from | What it is |
|---|---|---|
| `five-televisions` | `mega-man-2` on five presets | 3 s per television with the preset name as caption, no crossfade. Segment *i* shows seconds 3*i*..3*i*+3 of render *i*, so the game keeps running while the set changes; audio is continuous from the first render. |
| `raw-vs-pvm-vs-rf` | `journey-to-silius` on `reference_composite`, `sony_pvm_14l2`, `stass_favourite` | Three vertical thirds of the same picture, each from a different television, labelled. |

## manifest.json, version 2

```json
{"version": 2, "fps": 60.0988, "aspect": [4, 3],
 "presets": [{"id": "sony_pvm_14l2", "name": "Sony PVM-14L2", "blurb": "Focused beam, fine aperture grille, D65, composite"}],
 "games": [{"id": "super-mario-bros", "title": "Super Mario Bros.", "scene": "World 1-1, running right past the first blocks",
            "default_preset": "sony_pvm_14l2"}],
 "clips": {"super-mario-bros": {"sony_pvm_14l2": {
   "poster": [{"src": "assets/hero/super-mario-bros/sony_pvm_14l2/1920x1440/poster.webp", "width": 1920, "height": 1440},
              {"src": "assets/hero/super-mario-bros/sony_pvm_14l2/960x720/poster.webp", "width": 960, "height": 720}],
   "stage": [{"src": "assets/hero/super-mario-bros/sony_pvm_14l2/1920x1440/stage-hdr-hevc.mp4",
              "type": "video/mp4; codecs=\"hvc1.2.4.L153.B0\"", "hdr": true,
              "width": 1920, "height": 1440, "bytes": 18300000}],
   "lens": [{"src": "assets/hero/super-mario-bros/sony_pvm_14l2/3840x2880/lens-hdr-hevc.mp4",
             "type": "video/mp4; codecs=\"hvc1.2.4.L183.B0\"", "hdr": true,
             "width": 3840, "height": 2880, "bytes": 90100000}],
   "still": {"hdr": "assets/hero/super-mario-bros/sony_pvm_14l2/3840x2880/still-hdr.avif",
             "sdr": "assets/hero/super-mario-bros/sony_pvm_14l2/3840x2880/still-sdr.png",
             "width": 3840, "height": 2880, "frame": 0},
   "crop": {"sdr": "assets/hero/super-mario-bros/sony_pvm_14l2/3840x2880/crop-sdr.png",
            "hdr": "assets/hero/super-mario-bros/sony_pvm_14l2/3840x2880/crop-hdr.avif",
            "x": 930, "y": 1260, "width": 1500, "height": 1122},
   "hdr": {"white_nits": 203, "headroom": 4.0, "max_cll": 812, "max_fall": 50}}}}}
```

(The numbers are placeholders.) `stage` lists `stage-hdr-hevc`,
`stage-hdr-av1` and `stage-sdr` for each stage size, largest size first;
`lens` appears for lens shots only. `type` carries the video stream's codecs
string, built from the decoder configuration record that
`ffprobe -show_data` prints (hvcC, av1C, avcC) and the stream's colour tags,
so the same string works for `canPlayType` and
`mediaCapabilities.decodingInfo`; stage files also carry AAC-LC audio.
`poster` has one entry per stage size because the switcher shows a poster
only at its own pixel size. `hdr` holds the largest `max_cll` and `max_fall`
over the clip's HDR renders. `crop` is the 1:1 detail crop of the still,
SDR and HDR, with its position and size in the still; a clip installed
without `--with-crops` has none, and a crop preset's clip has nothing else. Merging keeps presets, games, clips and
top-level keys the run did not produce, replaces a produced clip's version 1
keys (`video`, `still_size`, `full`), merges `stage` and `lens` by `src`, and
drops the `sdr_1x` and `hdr_1x` keys of earlier crops, whose 2x2-averaged
files the site no longer shows.

The site reads `version` once for the whole file, so a version 1 clip that
stays in a version 2 manifest is rewritten in version 2 form: its `video`
becomes a one-entry SDR `stage` list, its `full` PNG (or else its `still`)
with `still_size` becomes `still` with frame 0 and no HDR file, and a `poster`
path becomes a one-entry list. Sizes, byte counts and the codecs string are
read from the files on the site. The manifest warnings list any clip still
holding version 1 keys.

## The shot list

| Shot | Game and scene | Length | Replay | Crop (NES px) | Lens | README |
|---|---|---|---|---|---|---|
| `super-mario-bros` | Super Mario Bros., World 1-1 running right | 6 s | right, jump | 8,144 100x93.75 | | PVM, Stas's |
| `legend-of-zelda` | The Legend of Zelda, overworld start, walk up | 6 s | up | 78,97 100x93.75 | | |
| `punch-out` | Punch-Out!!, first fight, crowd visible | 15 s | | 78,9 100x93.75 | every other preset as a crop | PVM |
| `journey-to-silius` | Journey to Silius, stage 1 with the dithered sky | 15 s | right | 78,17 100x93.75 | | |
| `castlevania-3` | Castlevania III, clock tower or the first stage | 6 s | right | 78,65 100x93.75 | | |
| `blaster-master` | Blaster Master, area 1 driving right | 6 s | right | 78,97 100x93.75 | | |
| `ninja-gaiden` | Ninja Gaiden, opening cutscene panel then gameplay | 15 s | | 78,65 100x93.75 | | |
| `mega-man-2` | Mega Man 2, stage select flashing, then a boss intro | 15 s | Start at frame 120 | 78,65 100x93.75 | | |
| `metroid` | Metroid, Brinstar start, bright shots on black | 6 s | right, fire | 78,97 100x93.75 | | |
| `batman` | Batman, stage 1 | 6 s | right | 78,97 100x93.75 | | |

Every shot is recorded on `sony_pvm_14l2`, `jvc_d_series_2000`,
`toshiba_14af43`, `stass_favourite`, `vhs_sp_consumer`, `bedroom_rf_1990`
and `famicom_kitchen`. `"lens": true` gives lens clips on all of a shot's
presets; a list of presets limits them. No shot has lens clips at present:
a 6 s lens clip adds about 70 MB to the site, which is near the GitHub Pages
budget with one game on every preset. Inspect then shows the still.

`crops` lists presets that get the still frame and its detail crop but no
clip. They are recorded for that one frame at full size, and the site shows
the crop on the game's page and on the television's page. Punch-Out!!
lists every preset outside the seven above in `crops`, so each television
is on the site once (about 4 MB per crop), on a frame with large sprites,
skin tones and a dithered crowd.

`detail_crop`, in the same units, is the region of the site's detail crops
when it differs from `flicker_crop` (the README's crop keeps its width): a
larger region shows more of a scene than the mouth of a boss.

`flicker_crop` is in NES pixel coordinates (256x240) and may be fractional.
The 4:3 render is the receiver's active raster, and the 256x240 picture is
a window in it: 282.75 NES dots across and 241 lines down, with the
picture starting 14.53 dots in from the left and its first line one line
above the field, so NES pixels are 8:7 and a set without overscan shows
black at both sides. On 3840x2880 one NES pixel is 13.58x11.95 render
pixels and the picture starts 197 pixels in: 100 pixels by 93.75 lines is
the 1358x1120 flicker crop, where a 100x75 region would be 1358x896. The
detail crops use the same region with a size that is a multiple of 6
(1356x1116). The site shows a crop 1:1 at any pixel ratio, at its pixel size
divided by the ratio, and a multiple of 6 makes that a whole number of CSS
px at ratios 1, 1.5, 2 and 3: even for the half-size width and height the
markup gives 2x displays, and a multiple of 3 at 1.5 and 3, where browsers
lay out in steps of 1/64 CSS px and a fractional size would stretch the
image by a fraction of a pixel.
`--flicker-scale 15` or `15x12` overrides the scale. `thumbnail_frame` picks
the poster and still frame; the site freezes clips at frame 0, so leave it
at 0 for shots on the site.

### shots.json schema

```
defaults      seconds {hero, feature}, presets [], flicker_crop [x,y,w,h], record_after,
              sizes {lens: WxH, stage: [WxH, ...], readme: WxH}, hdr {headroom, white_nits},
              thumbnail_frame, region, readme_seconds, record_args [], state_source (template),
              state_slot, font
presets       {id: {name, blurb}} for the manifest (falls back to the preset file's name)
shots[]       id, title, scene, rom (glob), rom_exclude [globs], state (file in states/),
              replay (file in replays/), seconds, kind hero|feature, presets [],
              lens (true, or a list of presets), flicker_crop, caption, thumbnail_frame,
              flicker_frame, default_preset, readme [presets that get README media],
              readme_seconds, region, record_after, crops [presets with a detail crop only],
              detail_crop [x,y,w,h] (the site's crop when it differs from flicker_crop)
features[]    id, type five-televisions|side-by-side, shot, presets [], labels [], caption,
              seconds_per_preset (five-televisions)
```

Replays are the frontend's review-input format: rows of `frame hexmask`
(A=01 B=02 Select=04 Start=08 Up=10 Down=20 Left=40 Right=80), frames
ascending from 1 and relative to the start of the recording, at most 128
rows, no comments.

## The recorder

Per clip and size the pipeline runs these two commands from the repository
root, with `XDG_CONFIG_HOME` pointing at a private directory,
`MYNES_REVIEW_NO_INPUT=1`, and without `MYNES_RECORD_CODEC_ARGS`,
`MYNES_RECORD_HDR_CODEC_ARGS`, `MYNES_OFFSCREEN_HEADROOM` or the
`MYNES_REVIEW_*` variables from your shell, so the recorder writes its
default masters:

```
build/bin/mynes_gpu --offscreen WxH --sdr --mask-alignment <A> \
    --preset presets/<preset>.json --load-state states/<shot>.s1 \
    [--input-replay replays/<shot>.replay] \
    --record out/<shot>/<preset>/WxH/sdr.mov --record-seconds <s> --record-after 2 <ROM>
build/bin/mynes_gpu --offscreen WxH --mask-alignment <A> \
    --preset presets/<preset>.json --load-state states/<shot>.s1 \
    [--input-replay replays/<shot>.replay] \
    --record out/<shot>/<preset>/WxH/hdr.mov --record-hdr --record-headroom 4 \
    --record-hdr-white 203 --record-seconds <s> --record-after 2 <ROM>
```

`<A>` is `pixels` at the full size, where a 3840-pixel frame resolves a
triad in 3 to 9 whole pixels, and `physical` at the stage and README
sizes: there the mask is drawn at its own pitch and band-limited, because
at 1920 or 960 pixels an integer-period mask would be 2 to 4 times too
coarse, and the 4:2:0 video could not carry it anyway. Panel subpixel
output is off for every render: a viewer's panel is unknown.

It relies on: `--record` writing every emulated frame as one video frame
with the APU audio as AAC; `--record-seconds N` producing exactly
round(N x 60.0988) frames (a shorter render passes the nine-decimal value
that gives its frame count); `--record-after F` skipping F frames after the
state load; `--input-replay` frame numbers counted from the first recorded
frame; `--record-hdr` writing BT.2020 PQ, tagged in the file; and `OUT.json`
beside `OUT.mov` with `frames`, `rate`, `width`, `height`, `hdr`,
`white_nits`, `headroom`, plus `max_cll` and `max_fall` for HDR. After each
run the pipeline checks the frame count, size, HDR tags, sidecar and pixel
format: 4:4:4 (`yuv444p` for SDR; ProRes 4444, which ffmpeg decodes as
`yuv444p12le`, for HDR). A 4:2:0 render would subsample the chroma behind
the stills and crops, so `record` and `encode` refuse one.
`defaults.record_args` appends extra flags (for example
`--room-reflections`).

## Adding a shot

1. Append an object to `shots` in `shots.json`: `id`, `title`, `scene`, a
   `rom` glob (add `rom_exclude` if the glob also matches sequels), and
   optionally `seconds` or `kind`, `flicker_crop`, `lens`, `caption`,
   `readme`.
2. `python3 tools/showcase/showcase.py --roms ~/roms check` validates the
   entry and shows which ROM matched.
3. `python3 tools/showcase/showcase.py --roms ~/roms --shots <id> states`,
   then create the state as printed; optionally record a replay with
   `--input-record`.
4. `python3 tools/showcase/showcase.py --roms ~/roms --shots <id> all --site ~/src/mynes-web`.

To feature a shot, add an entry to `features` that names it; `check`
verifies the shot is long enough for the feature.

A scene that play cannot reach in a scripted boot can be reached by poking
RAM: `boot/contra-boss.c` boots Contra, sets the level and routine bytes
the disassembly names, forces the player through the level until the
scroll stops for the waterfall boss, then writes `states/contra.s1` with
the core's own save-state call. Build it against the core the way
`run_rom` is built (`build/CMakeFiles/run_rom.dir/link.txt` has the flags)
and run it with the ROM and the output path.

The boss palette fades in over the first second after the state loads,
so the shot's `record_after` is 180: the clip, and the still and poster
taken from its first frame, start on the lit boss. The player in that
state falls as soon as the forced walk ends, respawns about 135 frames
after the load and, standing still, dies again at 290.
`boot/contra-replay.c`, built the same way, plays the state headless and
tries random schedules of firing, aiming, running and jumping until one
keeps the player alive to 10 frames past the end of the clip; it wrote
`replays/contra.replay`
(`contra-replay ROM states/contra.s1 replays/contra.replay 180 360`). The
search is deterministic, so a state built the same way gives the same
replay.

## Tests

```sh
python3 -m unittest discover -s tools/showcase/tests -t tools/showcase -v
```

`test_recipes`, `test_codecs`, `test_shots`, `test_manifest`, `test_runner`,
`test_images` and `test_cli` need no encoders; one PNG check in
`test_images` uses ffmpeg when it is there. `test_record` runs the record
stage and the command line against `tests/fake_recorder.py`, which takes
the recorder's arguments and writes ffmpeg test patterns with sidecars; it
needs ffmpeg. `test_encode` draws small SDR and HDR renders the way the
recorder writes them (512x384, 256x192, 128x96 and 320x240, 60 frames, with
sidecars; the HDR frames are limited-range BT.2020 Y'CbCr computed in numpy
and piped as `yuv444p16le`), runs every encode, feature and install job on
them and checks the outputs: codecs strings, HDR10 metadata, one-pixel
columns surviving, colours after the BT.601 to BT.709 change, an 800-nit
highlight within 1% in the stage video and the still, a PQ-peak patch that
must read 65535 in `still-hdr.avif` and 10000 nits in its content light
level, 1:1 crops, the manifest (a version 1
clip rewritten in version 2 form), the budget refusal and the cases where
install stops before copying. It takes under a minute and is skipped when
ffmpeg lacks one of the encoders or avifenc, numpy or Pillow is missing; the
`--fast` check is skipped without `hevc_videotoolbox`.
