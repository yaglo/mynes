# Showcase capture and encoding pipeline

`tools/showcase/showcase.py` records 4K masters of ten games on six
television presets with the GPU frontend's recorder, encodes every derived
output (site clips, lens stills, README animations, reddit and YouTube
variants, three feature clips) and installs the site outputs into the
[mynes-web](https://github.com/yaglo/mynes-web) checkout, merging
`assets/hero/manifest.json`. Everything is driven by `shots.json`, every
command is logged, `--dry-run` prints the commands instead of running them,
and each output is verified (frame count, size, timebase, byte limits) before
the pipeline moves on. No ROMs, states or renders are committed:
`states/` and `out/` are ignored by git.

Requirements on the Mac: the GPU frontend built with the recorder
(`build/bin/mynes_gpu --help` must list `--record`), `ffmpeg`/`ffprobe` 5.1
or newer with libx264, libwebp and libfreetype (`brew install ffmpeg`),
Python 3.10+ and Pillow (`pip3 install Pillow`), and your own ROMs.

## Workflow in five steps

1. **Build with the recorder** and check the tools:

   ```sh
   cmake --build build
   python3 tools/showcase/showcase.py check --roms ~/roms
   ```

   `check` validates `shots.json`, finds each ROM (recursive, case-insensitive
   globs; it prints what matched and refuses an ambiguous match), lists the
   save states that exist, checks ffmpeg's encoders and filters, Pillow, a
   caption font and the recorder flags in `mynes_gpu --help`.

2. **Create one save state per shot.** The pipeline never records a shot whose
   state is missing; `states` tells you exactly what to do:

   ```sh
   python3 tools/showcase/showcase.py states --roms ~/roms
   ```

   For each shot: run `build/bin/mynes_gpu <ROM>`, play to the scene, press
   F5 with slot 1 selected, then copy
   `~/.config/mynes/states/<ROM name>-<CRC-32>.s1` to
   `tools/showcase/states/<shot>.s1` (the command prints the exact paths,
   CRC included). Set `--config-dir` if your config lives elsewhere; the file
   name pattern is `defaults.state_source` in `shots.json` should the saves
   module change it.

   Shots with a `replay` play scripted input from that state. The starter
   replays are simple (hold right, walk up, press Start); to record your own,
   play the scene from the state while the frontend writes the script:

   ```sh
   build/bin/mynes_gpu --load-state tools/showcase/states/<shot>.s1 \
       --input-record tools/showcase/replays/<shot>.replay <ROM>
   ```

3. **Run everything:**

   ```sh
   python3 tools/showcase/showcase.py all --roms ~/roms --site ~/src/mynes-web
   ```

   or stage by stage: `record` (serial, it uses the GPU), `encode --jobs 4`,
   `features --jobs 2`, `install ~/src/mynes-web`. Use `--shots a,b` and
   `--presets x,y` to narrow, `--force` to rebuild outputs that are newer than
   their inputs (otherwise they are skipped), `--dry-run` to see the commands.
   The log is `out/showcase.log`; each master has `record.log` and
   `record.json` (command, frame count, rate, SHA-256 of ROM, state, replay
   and preset) beside it.

4. **Install into the site** (part of `all` when `--site` is given):
   `assets/hero/<shot>/<preset>.{mp4,poster.webp,4k.webp,4k.png}`,
   `assets/hero/features/<id>.{mp4,poster.webp}` and a merged
   `assets/hero/manifest.json`. Entries the run did not produce are kept.

5. **Commit both repositories**: `tools/showcase/shots.json` and any replays
   in `mynes`; `assets/hero/` in `mynes-web`. Masters and derived outputs stay
   in `tools/showcase/out/`; the README animations (`readme.webp`,
   `readme.gif`, `flicker.webp`, `flicker.png`) are copied by hand to
   wherever the README wants them.

## The shot list

| Shot | Game and scene | Length | Replay | Flicker crop (NES px) | README |
|---|---|---|---|---|---|
| `super-mario-bros` | Super Mario Bros., World 1-1 running right | 6 s | right, jump | 48,104 128x96 | PVM, Stas's |
| `legend-of-zelda` | The Legend of Zelda, overworld start, walk up | 6 s | up | 64,96 128x96 | |
| `punch-out` | Punch-Out!!, first fight, crowd visible | 15 s | | 64,8 128x96 | PVM |
| `journey-to-silius` | Journey to Silius, stage 1 with the dithered sky | 15 s | right | 64,16 128x96 | |
| `castlevania-3` | Castlevania III, clock tower or the first stage | 6 s | right | 64,64 128x96 | |
| `blaster-master` | Blaster Master, area 1 driving right | 6 s | right | 64,96 128x96 | |
| `ninja-gaiden` | Ninja Gaiden, opening cutscene panel then gameplay | 15 s | | 64,64 128x96 | |
| `mega-man-2` | Mega Man 2, stage select flashing, then a boss intro | 15 s | Start at frame 120 | 64,64 128x96 | |
| `metroid` | Metroid, Brinstar start, bright shots on black | 6 s | right, fire | 64,96 128x96 | |
| `batman` | Batman, stage 1 | 6 s | right | 64,96 128x96 | |

Every shot is recorded on `sony_pvm_14l2`, `jvc_d_series_2000`,
`toshiba_14af43`, `stass_favourite`, `vhs_sp_consumer` and
`reference_composite`. Hero loops are 6 s (361 frames), feature clips 15 s
(901 frames): frames = round(seconds x 60.0988) for NTSC, x 50.007 for PAL.

Feature clips, built from the masters rather than recorded:

| Feature | Built from | What it is |
|---|---|---|
| `five-televisions` | `mega-man-2` on five presets | 3 s per television with the preset name as caption, no crossfade. Segment *i* shows seconds 3*i*..3*i*+3 of master *i*, so the game keeps running while the set changes; audio is continuous from the first master. |
| `raw-vs-pvm-vs-rf` | `journey-to-silius` on `reference_composite`, `sony_pvm_14l2`, `stass_favourite` | Three vertical thirds of the same picture, each from a different television, labelled. |
| `push-in` | `punch-out` on `sony_pvm_14l2` | 12 s eased zoom from the whole 4K frame into a 4:3 window around the flicker crop, ending at one master pixel per pixel (1920x1440). |

### shots.json schema

```
defaults      seconds {hero, feature}, presets [], flicker_crop [x,y,w,h], record_after,
              offscreen "WxH", thumbnail_frame, region, readme_seconds, record_args [],
              state_source (template), state_slot, font
presets       {id: {name, blurb}} for the manifest (falls back to the preset file's name)
shots[]       id, title, scene, rom (glob), rom_exclude [globs], state (file in states/),
              replay (file in replays/), seconds, kind hero|feature, presets [],
              flicker_crop, caption, thumbnail_frame, flicker_frame, default_preset,
              readme [presets that get README animations], readme_seconds, region, record_after
features[]    id, type five-televisions|side-by-side|push-in, shot, presets [] (push-in: preset),
              labels [], caption, seconds_per_preset (five-televisions), seconds and
              start_frame (push-in)
```

`flicker_crop` is in NES pixel coordinates (256x240). On the 3840x2880 master
one NES pixel is 15x12 master pixels, so the default 128x96 region is
1920x1152 master pixels; `--flicker-scale 15` or `15x12` overrides the scale.
`thumbnail_frame` picks the poster and lens still; the site freezes clips at
frame 0, so leave it at 0 for shots that go on the site.

Replays are the frontend's review-input format: rows of `frame hexmask`
(A=01 B=02 Select=04 Start=08 Up=10 Down=20 Left=40 Right=80), frames
ascending from 1 and relative to the start of the recording, at most 128
rows, no comments.

## Outputs

Per (shot, preset) in `out/<shot>/<preset>/`:

| File | Recipe |
|---|---|
| `master.mov` | The recorder's 3840x2880 output, every emulated frame, AAC audio. Kept here only. |
| `hero.mp4` | 1440x1080 H.264 for the site: `-vf scale=1440:1080:flags=lanczos,format=yuv420p -c:v libx264 -crf 20 -preset slow -pix_fmt yuv420p -fps_mode passthrough -c:a aac -b:a 128k -movflags +faststart` |
| `hero.poster.webp` | Frame `thumbnail_frame` of `hero.mp4`: `-vf select='eq(n\,F)' -frames:v 1 -c:v libwebp -quality 85` |
| `still.4k.png`, `still.4k.webp` | Frame `thumbnail_frame` of the master, lossless PNG and `libwebp -quality 90` |
| `reddit.mp4` | As `hero.mp4` with `-crf 18` and `-b:a 192k` |
| `youtube.mp4` | `-c:v copy -c:a copy -movflags +faststart` when the master is H.264/HEVC with AAC; otherwise `libx264 -crf 14 -preset slow` |
| `readme.webp` | Presets listed in `readme` only. Every second frame of the first `readme_seconds` at 30 fps, 960x720: `-vf trim=end_frame=N,select='not(mod(n\,2))',setpts=N/(30*TB),scale=960:720:flags=lanczos -c:v libwebp_anim -quality Q -compression_level 6 -loop 0`, Q lowered 90,85,80,...,30 until under 5 MB |
| `readme.gif` | Same frames at 640x480 with a per-clip palette: `palettegen=max_colors=C:stats_mode=diff`, then `paletteuse=dither=bayer:bayer_scale=5:diff_mode=rectangle`, C lowered 256,192,128,96,64 until under 8 MB |
| `flicker.webp` | Eight consecutive frames from `flicker_frame` (default `thumbnail_frame`), cropped 1:1 at master pixels, 8 fps: `-vf trim=start_frame=F:end_frame=F+8,setpts=N/(8*TB),crop=W:H:X:Y -c:v libwebp_anim -lossless 1 -loop 0`, lossy 95,90,85,80 only if lossless exceeds 3 MB |
| `flicker.png` | The same crop of frame F, lossless |
| `readme.json` | The chosen WebP quality, GIF colours, flicker setting and sizes |

Per feature in `out/features/<id>/`: `youtube.mp4` (native size, `-crf 14`),
`reddit.mp4` (1440x1080, `-crf 18`), `site.mp4` (1440x1080, `-crf 20`) and
`poster.webp`. The graphs:

- five-televisions: `[i:v]trim=start_frame=i*F:end_frame=(i+1)*F,setpts=PTS-STARTPTS,drawtext=textfile=caption-i.txt:...` per input, `concat=n=5:v=1:a=0,setpts=N/FRAME_RATE/TB`, `[0:a]atrim=end=T`.
- side-by-side: `[i:v]crop=1280:2880:1280*i:0,drawtext=...`, `hstack=inputs=3`.
- push-in: `zoompan=z='1+(Z-1)*ease(on/(N-1))':x='(iw/2+(cx-iw/2)*ease)-iw/zoom/2':y=...:d=1:s=1920x1440:fps=<master rate>` with a smoothstep ease and Z = 3840 / window width.

Every video output is encoded with `-fps_mode passthrough` and no `-r`, so
ffmpeg copies the master's timestamps and neither drops nor duplicates a
frame; the master's own rational rate (`39375000/655171` as reduced by the
mov muxer) is what the outputs carry. After each encode `ffprobe` must report
exactly the expected `nb_frames`, size and `r_frame_rate`, and Pillow must
report the expected frame count for WebP/GIF/PNG, or the job fails loudly.

## The recorder

The pipeline runs, per (shot, preset):

```
build/bin/mynes_gpu --offscreen 3840x2880 --sdr --mask-alignment pixels \
    --preset presets/<preset>.json --load-state states/<shot>.s1 \
    [--input-replay replays/<shot>.replay] \
    --record out/<shot>/<preset>/master.mov --record-seconds <s> --record-after 2 <ROM>
```

with `XDG_CONFIG_HOME` pointing at a private directory and
`MYNES_REVIEW_NO_INPUT=1`, from the repository root. It relies on:
`--record` writing every emulated frame as one video frame (no drops) with
the APU audio muxed as AAC; `--record-seconds N` producing exactly
round(N x 60.0988) frames (PAL: 50.007); `--record-after F` skipping F frames
after the state load; `--load-state` taking a file written by F5; and
`--input-replay` frame numbers counted from the start of recording.
`defaults.record_args` in `shots.json` appends extra flags (for example
`--room-reflections`); `MYNES_RECORD_CODEC_ARGS` is the recorder's own
override for its encoder.

## Adding a shot

1. Append an object to `shots` in `shots.json`: `id`, `title`, `scene`, a
   `rom` glob (add `rom_exclude` if the glob also matches sequels), and
   optionally `seconds` or `kind`, `flicker_crop`, `caption`, `readme`.
2. `python3 tools/showcase/showcase.py check --roms ~/roms` - it validates the
   entry and tells you which ROM matched.
3. `python3 tools/showcase/showcase.py states --shots <id>` and create the
   state as printed; optionally record a replay with `--input-record`.
4. `python3 tools/showcase/showcase.py all --roms ~/roms --site ~/src/mynes-web --shots <id>`.

To feature a shot, add an entry to `features` that names it; `check` verifies
the shot is long enough for the feature.

## Tests

```sh
python3 -m unittest discover -s tools/showcase/tests -t tools/showcase -v
```

`test_recipes`, `test_shots`, `test_manifest` and `test_runner` are pure and
fast. `test_encode` draws a 3840x2880, 60-frame pattern with Pillow, pipes it
into ffmpeg at the NES frame rate with a sine track (the shape of a master),
runs the whole encode and feature job set on it and checks every output,
including that the push-in starts on the whole frame and ends 1:1 on the
window; it takes a few minutes and is skipped without ffmpeg and Pillow.
