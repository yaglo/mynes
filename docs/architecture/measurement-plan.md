# Hardware measurement plan

What to measure on the real hardware to replace the model's remaining
assumptions, with the equipment on hand: an NES-001 with its RF/AV module,
a Sony PVM-14L2, a colorimeter, a Mac. Each entry says what the number
settles, what it needs, the procedure, and where the result goes. Entries
that need a scope or an SDR are marked; everything else can be done now.
The model-limits review's table (section 3 of the plan) is the source for
the receiver items; the audio and video-output items come from the
schematic work in `tools/circuits`.

## Ground rules

- Warm both units for 30 minutes. Note mains voltage if a meter is handy;
  the NES's reservoir ripple and the 7805's headroom follow it.
- Photograph every setup and every menu screen. A phone photo of the PVM's
  service menu values is a measurement.
- Use the emulator's own patterns where one exists (`--test-signal`, the
  calibration bars in `mynes_retro --calibrate`) and the 240p Test Suite
  ROM on the console, so the console picture and the emulator picture come
  from the same pixels.
- Record raw files (WAV, RAW photos, `.pfm` captures), not processed ones.

## A. NES-001 audio (audio interface only)

**A1. Frequency response at the jack.** Settles the gate's output
resistance (the 8–53 kHz output-pin pole), confirms the 16.7 Hz corner and
the flat middle, and calibrates the jack level (`AUDIO_JACK_VOLTS_PER_UNIT`,
an estimate today).

- Needs: a line input on an audio interface with a known input impedance
  (read it from its manual; 10 kΩ and 47 kΩ give different low corners, and
  the chain has a field for it), a sweep or noise ROM (rainwarrior's sweep
  ROM from nesdev thread 17745, or any ROM that plays a long triangle
  note), 48 kHz or 96 kHz recording.
- Procedure: record 60 s of the sweep; also record 10 s of a single
  full-volume pulse at 1 kHz (any tracker ROM) and note the interface's
  input sensitivity in dBu or volts. Analyse with the script in
  `tools/circuits/sweep_nes001_audio.py --measured file.wav` (to be added:
  FFT of the sweep against the deck's response).
- Goes to: `AUDIO_CORNERS_NES_FRONT` (`audio_format.h`), the golden file,
  `AUDIO_JACK_VOLTS_PER_UNIT`.

**A2. Hum, noise floor and the picture in the sound.** Settles the PSU
ripple at the jack (the deck says 0.4 to 1.5 mV peak at 120 Hz, 62 to
74 dB under a unit) and the board's video-to-audio coupling: nesdev
recordings (tepples, thread 156) show the line rate and its divisions
(7.9 kHz from alternate black and white lines, 983 Hz from a half-black
picture) and a 60 Hz component from the PPU's blanking in the NES-001's
audio, quietest on a black screen; the deck rules out the regulator as the
path (84 dB under a unit), leaving the audio's run past the PPU and video
nets on the board, whose coupling has no published value. A picofarad
into the gate's summing node would put the line rate 60 dB under a unit.

- Needs: the same interface, a ROM with a silent screen and one with a
  bright full screen, cable shielding as used at the TV.
- Procedure: record 30 s of silence on a black screen and 30 s on a white
  screen at the highest clean gain; FFT, read the 60, 120, 180, 240 Hz
  lines and the 60 Hz field component; repeat with the AV lead lifted from
  the ground of the interface (ground-loop check). Then the two nesdev
  patterns (alternate black and white lines; top half black, bottom half
  white) with the music muted: the 7.9 kHz and 983 Hz lines against the
  15.7 kHz line give the coupling's frequency law, and their level against
  a known pulse tone its magnitude, which would drive the same per-line
  track the RF buzz already uses.
- Goes to: the `psu` block's values (adaptor voltage under load, the
  reservoir's real capacitance, the console's draw) and `audio_pickup_mv`
  on the NES-001 presets; the hum itself is derived from them. The deck
  puts the video stage's draw on the rail at 50 to 125 µV of 15.7 kHz and
  10 µV of 60 Hz at the jack, 80 to 95 dB under a unit, so a measured
  field-rate buzz on composite would point at another path (ground return,
  the modulator's rail) rather than the regulator.

**A3. Level and clipping.** Settles the gate's rail window (3 V peak to
peak from the data sheet) and whether real games reach it.

- Procedure: record a game with all channels at full volume (Battletoads
  pause music, or a DPCM-heavy title); look for flattened peaks; measure
  the peak voltage from the interface's calibration.
- Goes to: `AUDIO_NES001_GATE_WINDOW_V` (`audio_format.h`) and the jack
  level scale.

**A4. RF sound buzz.** Settles the two numbers the RF buzz is derived
from: the set's sound-detector AM rejection (`rf.sound_am_rejection_db`,
45 dB assumed) and the NES modulator's incidental phase modulation
(`rf.icpm_deg`, 0 assumed).

- Needs: a set with RF input and a line or headphone output (the PVM has
  no tuner; a consumer set or a VCR's tuner into the interface), the
  console on channel 3, a ROM that can show a black field, a white field
  and a 50% field with the music muted.
- Procedure: record 20 s of each field; the 60 Hz buzz harmonics against
  the field's average level give the AM rejection (the buzz should scale
  with the level step between the picture and blanking); a tick at the
  field edges that does not scale that way is the modulator's phase
  modulation. Read the 15.7 kHz line whistle as well.
- Goes to: the two `rf` values on the RF presets; a set's own value in
  its preset.

**A5. The set's acoustic whine.** The flyback and yoke of a CRT radiate
the line rate, 15.7 kHz, and nothing in the electrical chain carries it,
so it cannot be derived; a phone's spectrum analyser at the viewing
position, with the set on a black field and the sound muted, gives its
level in dB SPL against the game audio at normal volume. Without that
measurement it stays out of the model.

## B. NES-001 video output (needs a scope)

**B1. Edge shape at the jack, terminated.** Settles the console output
follower's rise (the deck says 105–118 ns 10–90%) and fall (11 ns), and
therefore whether `console_follower_tau_ns` should be the presets' default
in place of the 30 ns estimate.

- Needs: a scope of at least 100 MS/s, a 75 Ω terminator, a ROM with a
  one-pixel white grid (240p Test Suite "Grid" or "Sharpness").
- Procedure: probe the RCA jack across the terminator; capture a line
  with the grid; measure 10–90% rise and 90–10% fall on a black-to-white
  and a white-to-black edge; capture a flat colour of each palette row
  ($0x, $1x, $2x, $3x at hue 8) and read the chroma square wave's shape.
- Goes to: the console follower default, the `sweep_nes001_video.py`
  golden, the PPU pin swing.

**B2. Pin 21 versus jack.** Settles the PPU's own source impedance and the
brightness-dependent phase (the 2C02G estimate) separately from the board.

- Procedure: same captures at PPU pin 21 (motherboard, before Q1) and at
  the jack; compare per row.
- Goes to: `console_phase_distortion_ns`, `nes001_video_chain.cir`
  (`rppu`).

**B3. RF.** Needs an RTL-SDR or HackRF behind a 20 dB attenuator on the RF
out: an IQ capture of channel 3 with a colour-bar screen and a 1 kHz tone
gives the vision/sound carrier ratio (assumed 13 dB), sound deviation,
AM/AM and AM/PM, and buzz. Goes to WP-B and the RF sound noise formula in
`preset_apply.c`.

## C. PVM-14L2 with the colorimeter (no other instruments)

**C1. Primaries and white.** Settles the PVM preset's phosphor
chromaticities (nominal SMPTE-C today) and its white points.

- Needs: the colorimeter in CRT mode (in ArgyllCMS, `spotread -y c` or the
  instrument's CRT setting), a dark room, the PVM warmed up, the emulator
  or a test ROM showing full-screen red, green, blue and white.
- Procedure: on the PVM, set the menu white to the D65 option and read xyY
  of the four fields at the screen centre with CONTRAST at the detent and
  BRIGHTNESS at the centre; repeat at the 9300 K option; repeat R, G, B at
  three levels (25, 50, 100 IRE) to see whether the primaries drift with
  level.
- Goes to: `crt_color.h` phosphor primaries for the PVM preset,
  `white_temperature_k`; the same fields for the FW900 if it is measured.

**C2. Grey scale and gamma.** Settles the per-gun EOTF and black level.

- Procedure: a 17-step grey ramp (240p Test Suite "Grey ramp" or the
  emulator's pattern), read Y at each step at the centre; read Y of a 10%
  window and a full field of white (the ABL: full-field white should be
  dimmer per unit area). Note the PVM's contrast and brightness positions.
- Goes to: `gamma`, `black_floor`, the ABL item in WP-E.

**C3. Luminance.** Settles absolute anchoring: read Y of a 100% window at
the PVM's standard settings (Sony specifies 120 cd/m²) and compare with the
Mac panel's SDR white; this is the number `hdr_gain` should be derived
from rather than fitted.

**C4. Composite versus S-Video versus RGB colour.** Settles the decoder
axes and gains (the receiver is nominal today).

- Procedure: feed the PVM the NES on composite; show the 12 hues at
  level 2 ($2x) one at a time as full fields; read xyY of each; do the same
  from the emulator's output of the same codes (the colorimeter on the Mac
  panel, in its LCD mode), and, if an RGB source is available (a Mega
  Drive on SCART), full-code bars on RGB for the tube's own primaries.
- Goes to: `crt_color.h` decoder matrix and hue offset for the PVM,
  `chroma_gain`.

## D. PVM-14L2 with a phone camera (macro)

**D1. Comb filter law and store type.** Settles whether the PVM's adaptive
comb falls back to band-pass on NES detail (the biggest open item for the
PVM preset's look).

- Procedure: 240p Test Suite or a custom ROM with one-dot and two-dot
  vertical stripes, a white bar on black, white text on $02, and hue
  boundaries; photograph each at 1:1 on the grille with the phone at its
  closest focus; compare with the same screens through the emulator's
  three comb settings. Static dots on a flat $16 field mean an H-reset
  store.
- Goes to: `comb_type` and the comb parameters of the PVM preset.

**D2. Spot and grille.** Settles beam width against current and the stripe
fill.

- Procedure: isolated one-pixel lines at three levels, at the centre and a
  corner, and a crosshatch; the 0.25 mm grille pitch is the scale.
- Goes to: `beam_fwhm_min`/`beam_fwhm_max`, mask parameters.

**D3. Persistence.** Needs a photodiode rig (BPW34 and an integrator; the
deck for it is in the plan) or, less well, a phone at 240 fps: a
single-frame flash of each primary once a second.

## E. Mac panel

Read the panel's SDR white in cd/m² with the colorimeter in LCD mode, so
the PVM's 120 cd/m² can be mapped; note the value in the HDR notes.

## What to do first

1. A1 and A2 (an afternoon; only the audio interface).
2. C1, C2, C3 (an evening; colorimeter and the ROM). C1 alone replaces the
   PVM preset's nominal primaries with measured ones.
3. D1 (an hour; phone). Decides the comb.
4. B1 when a scope is available; it decides the follower default.
