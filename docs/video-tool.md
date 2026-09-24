# Pictures and videos through the chain

`mynes_video` takes a still picture or a video and runs it through a
television preset's whole signal chain: an NTSC composite encoder, the cable
or RF modulator, the VHS deck when the preset has one, the television's
receiver, Y/C separation and decoder, and the tube. It writes a new video of
the result, SDR or HDR, with the input's sound.

```sh
cmake --build build --target mynes_video
./build/bin/mynes_video --preset vhs_sp_consumer holiday.mp4 holiday-vhs.mov
./build/bin/mynes_video --preset sony_pvm_14l2 --preset bedroom_rf_1990 \
    --preset vhs_sp_consumer --seconds 8 photo.jpg photo.mov
```

With several `--preset` flags it writes one file per preset, named
`OUTPUT-<preset>.mov`. `--list-presets` prints the bundled ones; a preset can
also be given as a JSON file.

## What happens to each frame

1. ffmpeg decodes the input at the chain's field rate, 60.0988 per second
   (a still is decoded once and repeated for `--seconds`, 5 by default). The
   picture is cropped to the shape of the area a console picture fills on
   the tube (`--fit` pads instead) and scaled to 1024 pixels by 240 lines.
2. The RGB encoder source turns the gamma-encoded R'G'B', 10 bits per gun,
   into Rec. 601 luma and B-Y/R-Y on the subcarrier, with a -40 IRE sync and
   a 40 IRE sine burst, as an NTSC encoder IC does. Its luma band (5 MHz),
   chroma band (1.3 MHz), optional subcarrier trap and black pedestal are
   `--luma-mhz`, `--chroma-mhz`, `--trap` and `--setup` (0.075 for US 7.5 IRE
   setup; the presets were set up for signals without it).
3. The preset's chain processes the composite signal exactly as it does the
   NES's, including dot crawl, cross-colour, RF snow and ghosts, the VHS
   deck's FM luma, colour-under chroma, timing error and dropouts, and the
   receiver's line and colour loops. The deck's record AGC holds the
   encoder's sync at -40 IRE, as it does the NES's shallower one.
4. The tube and the glass are drawn at `--size` (1920x1440 by default) with
   the mask at its physical pitch (`--mask-alignment pixels` fits it to
   whole output pixels), and the recorder writes every field.

Before recording, 60 fields of the first picture let the receiver's AGC,
line and colour loops and the tube's persistence settle (`--settle`).

## Output

| Output | Format |
|---|---|
| `OUTPUT.mov` | ProRes 4444, 4:4:4, BT.709 primaries with the sRGB transfer the display pass writes; plays in QuickTime and editors |
| `OUTPUT.mp4` | H.264 4:2:0 at CRF 16 for playback anywhere; 4:2:0 halves the colour resolution, which softens the mask |
| `--hdr`, `OUTPUT.mov` | ProRes 4444 in BT.2020 PQ, 203 nits for SDR white, with MaxCLL and MaxFALL in `OUTPUT.json` |

The sound is copied from the same stretch of the input as AAC. A still gets
a silent track. `OUTPUT.json` records the frame count, rate, size and light
levels, as for `mynes_gpu --record`.

## Limits

- The chain runs at the NES's timing: 262 progressive lines per field and
  227 1/3 subcarrier cycles per line, with the 2C02's alternating frame
  phase. Dot crawl and comb artefacts have a console's pattern, not
  broadcast NTSC's (227.5 cycles per line, 525 interlaced lines), and a
  video is shown at 240 lines per field. The VHS deck model is built for
  that line length.
- The picture fills the 256 dots by 240 lines a console picture uses, 47.7 of
  the 52.7 us active line, so the tube shows the same narrow side borders,
  black at the encoder's pedestal (`--setup`).
- NTSC presets only; PAL presets are refused.
- The input's primaries are taken as the encoder's; an HD source's BT.709
  primaries and SMPTE-C differ by less than the presets' phosphor gamuts.
