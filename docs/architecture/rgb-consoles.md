# RGB consoles through the signal chain

Research note and prototype, September 2026. The question was whether the
GPU signal, receiver and CRT chain built for the 2C02 can take a Mega Drive
or a Super Famicom: convert the emulator's RGB back into the waveform the
console's video encoder puts on the cable, and process it from there.

The answer is yes, and the prototype does it: `mynes_retro` loads a libretro
core (Genesis Plus GX, Snes9x), recovers the video chip's colour codes from
the core's frame, and runs them through a new encoder source stage in place
of the 2C02 DAC. Everything after the raster is the existing chain. Colour
bars decode to the sent values to three decimals on the composite, S-Video
and RGB connections.

## What differs from the NES

The 2C02 draws its composite waveform itself: each palette code is a voltage
square wave, and the chain's source stage is a table of measured voltages.
The Mega Drive VDP and the S-PPU2 output RGB through DACs, and a separate
encoder IC (Sony CXA1145 or CXA1645, Fujitsu MB3514 and Samsung KA2195D
clones on the Mega Drive; Nintendo's S-ENC and, on 1CHIP boards, S-RGB on
the Super Famicom) forms the composite signal from that RGB plus the chip's
CSYNC. So the source stage becomes RGB codes → measured DAC ramp → an
encoder model: Y and B-Y/R-Y matrix, chroma band-pass, balanced modulators,
standard sync and a sine burst.

That makes the encoder the interesting part, and the emulator's RGB an
exact input rather than an approximation:

- The Mega Drive DAC has 3 bits per gun (15 levels with shadow/highlight);
  the measured ramp is 0, 29, 52, 70, 87, 101, 116, 130, 144, 158, 172, 187,
  206, 228, 255 of 1 V, normal colours on every other step, shadow on the
  first eight, highlight on the last eight ([plutiedev, VDP colour
  ramp](https://plutiedev.com/vdp-color-ramp)). Genesis Plus GX writes the
  step index into each channel of its RGB565 frame with injective shifts,
  so the index comes back exactly and the ramp is applied here, not the
  core's linear expansion.
- The S-PPU2 has 5 bits per gun; Snes9x's RGB565 carries them exactly
  (master brightness already applied). The prototype uses a linear ramp;
  the chip's actual DAC curve, and the 2-chip consoles' slow falling edges
  that blur their picture ([lidnariq, TmEE, ccovell on
  nesdev](https://forums.nesdev.org/viewtopic.php?t=16052)), are not
  modelled yet.

## Geometry on the 12-samples-per-cycle grid

The chain samples at twelve times the subcarrier. Both consoles fit it
without resampling the receiver:

| | Master clock | Line | Cycles/line | Line phase | Pixels | Samples/pixel |
|---|---|---|---|---|---|---|
| 2C02 | 6 fsc (21.48 MHz) | 341 dots, 1364 clocks | 227.33 | 4 slots | 256 | 8 |
| S-PPU | 6 fsc | 1364 clocks, one 1360-clock line every other frame | 227.33 | 4 slots | 256 or 512 | 8 or 4 |
| VDP | 15 fsc (53.69 MHz) | 3420 clocks | 228 exactly | 0 | 320 (H40) or 256 (H32) | 6.4 or 8 |

Sources: [SNESdev timing](https://snes.nesdev.org/wiki/Timing), [Mesen SNES
timing](https://snesdev.mesen.ca/wiki/index.php?title=SNES_Timing), [kabuto's
Mega Drive notes](https://plutiedev.com/mirror/kabuto-hardware-notes),
[tepples and lidnariq on the Genesis line
length](https://forums.nesdev.org/viewtopic.php?t=24447).

Consequences:

- The Super Famicom is the NES geometry with an RGB source: the same 341
  dots, 8 samples per pixel, 4 slots of phase per line. Its short line
  makes the frame phase alternate between two values instead of the NES's
  three-frame cycle with a skipped dot.
- The Mega Drive's active picture is 2560 clocks = 2048 samples in both
  widths, so H40's 320 pixels are 6.4 samples each. The encoder stage maps
  each sample to the pixel under it (sample-and-hold), so no resampling of
  the picture is involved.
- A 228-cycle line means the chroma phase is the same on every line and
  every frame. Dot crawl is stationary, and the well-known "rainbow" on
  vertical dither comes from a 2-pixel pattern at 7.5 cycles per 8 landing
  next to the subcarrier. The chain's 1H comb keeps a receiver's 2730-sample
  (227.5-cycle) delay line, so against a 2736-sample line the delayed line's
  chroma is inverted as the comb needs, and luma from the previous line
  arrives six samples (half a cycle) displaced. That is what a comb-filter TV
  does with a Mega Drive, and it came out of the existing stage unchanged.
- The complete line length is now a field of `SignalFormat`
  (`dots_per_line`, 341 unless a frontend sets it); the raster and the
  console-output stage read it.

## Encoder model in the prototype

`encoder_rgb.comp.glsl` per output sample: pixel code → ramp → gun
voltages; Y = 0.299 R + 0.587 G + 0.114 B; U = 0.492 (B-Y), V = 0.877 (R-Y);
U and V through a Hamming-windowed sinc low-pass at the encoder's chroma
band (1.3 MHz default; the CXA1145's external LC and the CXA1645's built-in
filter are not measured); composite = Y + U cos(a - 138°) + V cos(a - 48°)
on the raster's slot phase. The raster stage gained a standard -40 IRE sync
and a 40 IRE peak-to-peak sine burst (`sync_level`, `burst_amp`,
`burst_sine`), the 2C02 values remaining the default.

The chroma axes were measured, not derived: full-code colour bars through
the reference composite preset, decoded RGB read back before the tube, and
a least-squares fit of the decoded colour-difference vector against the sent
one. With the 2C02 output-impedance model on, the bars decoded 9 degrees
rotated, 6 per cent low in luma and 10 per cent low in chroma; that stage is
a brightness-dependent estimate for the 2C02's pin, so the prototype turns
it off for encoder sources, after which the fit is a pure rotation and the
axes above give an identity to floating-point precision on all three
connections. `mynes_retro --calibrate` repeats the measurement.

Whether NTSC setup (7.5 IRE) is added by the Mega Drive's encoders is not
verified; `setup` is a parameter, 0 for now.

### Cross-colour on white detail

Single-pixel white strokes (text) rainbow heavily on composite. The
encoder stage has a luma low-pass (`luma_bw_hz`, 5 MHz by default from the
CXA1645's -3 dB figure) and an optional trap at the subcarrier
(`luma_trap`, the YTRAP pin's function); the M menu's Encoder submenu sets
both. `mynes_retro --cross-colour` measures them: white one-pixel strokes
every fourth pixel (vertical) and every fourth line (horizontal), decoded
RGB read back before the tube, mean chroma magnitude in R-Y/B-Y units over
each half. Super Famicom geometry, 8 samples per pixel:

| Encoder | Reference composite (notch) | PVM-14L2 (adaptive comb) |
|---|---:|---:|
| Unfiltered | 0.515 | 0.490 |
| Luma 5 MHz | 0.517 | 0.489 |
| Luma 3.5 MHz | 0.326 | 0.300 |
| Luma 5 MHz, trap depth 1 | 0.122 | 0.015 |
| Chroma band 0.6 MHz | 0.515 | 0.490 |

Horizontal strokes give 0.000 in every case. The vertical strokes' third
harmonic (a 4-pixel period is 1.34 MHz) lands at 4.03 MHz, inside the
receiver's chroma band and below a 5 MHz luma cut, so the encoder's rated
bandwidth does not remove it; the comb does not either, because its
227.5-cycle delay line meets a 227.33-cycle line a quarter pixel displaced.
Only a trap, or a luma path much slower than the encoder's rating, does.
The 2-chip Super Famicom's slow DAC edges are the latter and are the likely
reason real consoles show blur where the model shows rainbow; the 1CHIP
consoles and the Mega Drive have sharper paths. Which of these to make the
default is a measurement (scope captures of each console's composite on a
one-pixel pattern), not a choice.

### Tests

`frontends/gpu/tests/test_encoder.c`, part of `gpu_fidelity_tests`, holds
both measurements on the default chain (no preset): full-code bars through
the encoder come back within 0.0003 of the sent RGB on composite, S-Video
and RGB for both console line lengths, which is the receiver's first check
against a standard-level NTSC source; and one-pixel strokes give 0.33 of
cross-colour vertically, 0.000 horizontally, and 0.12 with the trap, held to
"at least half removed" since the ratio depends on the receiver's chroma
band (0.24 on the reference preset, 0.03 with an adaptive comb).

The same bars also measured the 2C02 output-impedance stage
(`console_phase_distortion_ns`, 30 ns in the presets): on a standard-level
source it rotates hue by 9 degrees and takes 10 per cent of the chroma and 6
per cent of the luma of fully saturated bars. That follows from the model's
form, a one-pole whose time constant grows with instantaneous voltage: the
carrier's peaks are followed more slowly than its troughs, so the cycle
mean drops with chroma amplitude. It is the intended brightness-dependent
behaviour, not a defect, but the luma loss is a consequence the NESdev
estimate it comes from does not state, and NES colours, being less
saturated than full-code bars, see less of it.

## Prototype

The frontend (`frontends/retro/`, target `mynes_retro`) is a separate commit
on the `worktree-research-rgb-consoles` branch; the chain changes, the
encoder stage and the test are in master.

```
cmake --build build --target mynes_retro
build/bin/mynes_retro --core genesis_plus_gx_libretro.dylib --console md \
    --preset sony_pvm_14l2 game.md
build/bin/mynes_retro --core snes9x_libretro.dylib --console snes \
    --preset living_room_1988 --offscreen 2560x1920 --screenshot-after 900 \
    --screenshot out.ppm game.sfc
build/bin/mynes_retro --core ... --console md --preset reference_composite \
    --offscreen 640x480 --calibrate game.md
```

Cores are built from their libretro Makefiles (on this machine both need
`SDKROOT` pointed at the Xcode SDK and a macOS deployment target of 11.0).
Keyboard: arrows, Z/X/C for the three face buttons, S and A/D for the
others, Enter for Start, right Shift for Select, Escape quits. Audio goes
straight to the device; the console audio chain is not applied.

Verified on Castlevania Bloodlines and Contra Hard Corps (Mega Drive) and
Blackthorne (Super Famicom) through the reference composite, PVM-14L2,
living room 1988, bedroom RF, S-Video and RGB arcade presets, in HDR with
the Auto white fit and in SDR.

## Not done, and what a real integration needs

- No OSD, no presets of its own: the console profile is a struct in
  `frontends/retro/main.c`. A shipped version needs console profiles as
  preset data with menu items for the encoder band, setup and DAC ramp.
- Genesis Plus GX and Snes9x are loaded as libretro cores, so per-line
  width changes, the backdrop colour outside the picture, the Mega Drive's
  border and the Super Famicom's overscan line placement are approximate:
  the picture sits at raster line 11 (Mega Drive) or 1 (Super Famicom) on a
  black border. Interlaced Super Famicom output is taken as its first field.
- PAL is not wired (PAL Mega Drive lines are 285 cycles, PAL S-PPU 284.17;
  both need their own raster timing).
- The encoder's own defects are not modelled: chroma/luma delay mismatch,
  the CXA1145's external filter values on each board revision, the Mega
  Drive's jailbars, the 2-chip Super Famicom's DAC settling. These are the
  next fidelity items and each is a measurement, not a guess.
- The frame's carrier phase is derived from line counts, not from a core
  clock. Genesis Plus GX and Snes9x do not expose the master clock phase;
  a native core would.
