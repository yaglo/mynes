# GPU signal and CRT controls

**M** opens the translucent TV menu. Use Up/Down to choose a row and Enter to
open a submenu or enter adjustment mode. On a parameter, Left/Right also starts
adjustment immediately. The menu then collapses to a bottom strip with only the
parameter, value and range bar; the rest of the game stays visible. Enter or
Escape returns to the same row. M closes either view.

Audio starts in **GPU + fallback** mode. Use **M → Audio → Processing** or **A**
to switch to CPU processing for the session. `MYNES_GPU_AUDIO=0` selects CPU
at startup. A late GPU block falls back to CPU without delaying playback;
unavailable GPU audio also falls back automatically.

![TV-style parameter adjustment](images/gpu-osd-adjustment.png)

The menu and preset notice are mixed as RGB after the receiver, before the gun
amplifiers and beam. They retain the tube's focus, grille, geometry and light
response without acquiring NES composite rainbowing. Black text outlines keep
the translucent display readable over bright game content. The overlay pass is
skipped when closed; unchanged overlays reuse their uploaded pixels.


[Hardware evidence](gpu-hardware-research.md) · [Preset audit](gpu-preset-audit.md)

Controls describe several different things: source electronics, receiver response,
CRT behavior, and adaptation to the host display. A working control is not by
itself evidence of a calibrated hardware model. The settings below have been
traced through the preset/OSD update paths to their consumers; this is not a
claim that every combination has been visually calibrated.

| Group | Controls and effect | Conditions and limitations |
|---|---|---|
| Source timing | Region, source phase, line phase, demodulation rotation | Gameplay frame phase comes from the PPU clock. Synthetic field advance/count are no longer offered in the OSD. Phase controls are diagnostics, not TV service adjustments. |
| Console | Output resistance, video bandwidth, differential-phase RC, PSU hum | Nonlinear phase response applies to NTSC composite/RF. Output resistance also affects the cable pole. |
| Cable | Length, resistance, capacitance, termination, shielding, reflection level/delay | A lumped pole and explicit delayed reflection, not a distributed transmission line. Reflection delay needs nonzero reflection level. Shield pickup is a generic noise approximation. |
| RF | Carrier level, noise floor, complex IF bandwidth/asymmetry/detuning, AGC attack/release | Applies to the RF route. Carrier *frequency* is legacy channel metadata, not a simulated tunable RF oscillator. |
| Y/C separation | Horizontal trap, line-comb topology, comb extraction fraction | Trap reduces cross-luma; it cannot remove luma leaking into C. Comb fraction zero selects a legacy default, not bypass. NTSC line combs do not operate on PAL or separated-input routes. |
| Decoder filters | Y/I/Q bandwidth, FIR lengths/window, sharpness | Q bandwidth zero follows I. Short luma FIRs below 23 taps disable the trap. FIR lengths are session diagnostics rather than saved tube characteristics. |
| Color | Hue, saturation, decoder R-Y/B-Y gain, white point, RGB drive/cutoff, phosphor primaries | Decoder gain differs from white balance. Primaries are nominal colorimetric matrices, not measured spectra of each preset's tube. |
| Video amplifier | RGB bandwidth, rise/fall asymmetry, vertical smear, velocity modulation | Equivalent filtering and behavioral effects; not circuit models of the named monitors. |
| Gun and spot | Gamma and per-gun offsets, dark/white FWHM, bloom exponent, horizontal spot size/growth, edge focus, convergence | Positive FWHM settings override the old sharpness/height fields. RGB landing offsets precede the mask. |
| Loading | Video rail sag, black-level recovery, recovery time, size sag, focus change | Strength zero disables the corresponding effect. Recovery time matters only with recovery enabled. Supply models are generic. |
| Geometry | Position, size, curvature, skew, rotation, keystone, jitter, interference jitter, wobble, top-band faults | Top-band bounds need shift/skew enabled. “Interference jitter” is a geometry disturbance, not an RF carrier simulation. |
| Mask | Type, triads across, pixel pitch, strength, RGB/BGR order | Positive triad count overrides pixel pitch. Pitch is derived from the actual drawable, filtered when unresolved, and optionally aligned to pixels. RGB/BGR denotes modeled phosphor order, not proof of physical panel-subpixel alignment. |
| Time | Persistence and RGB lifetime scales, optional frame blend and motion threshold | Motion threshold only affects frame blending. This post-render blend is not a 3D composite comb filter. Zero persistence disables phosphor history. |
| Tube variation | Cathode gains/nonuniformity, purity tint, grain, thermal doming, chromaticity shift, astigmatism, microphonics | Behavioral approximations, not individually measured tube defects. Avoid increasing every imperfection merely to make a picture “more CRT.” |
| Glass/room | Halation and tint, glass transmission/reflection, scatter, antiglare, glare position/size/color, ambient light, vignette | Halo tint sets per-primary scatter fractions and needs halation. Internal reflection uses spatial scatter, not a local gray pedestal. Glare geometry needs glare enabled. Reflected room light does not scale with emission gain. |
| Output | Emission gain, available HDR headroom, SDR white level | Gain controls emitted light; headroom and white level come from the host. The output shoulder preserves RGB ratios while fitting peaks. |
| Audio | Amplifier drive, hum strength/frequency/harmonics, noise | Shared CPU/GPU audio model controls; independent of picture gain. |

Legacy `num_sections` and RF `carrier_freq` values still round-trip when importing
old presets, but are not operative GPU controls. They are retained for file
compatibility, not exposed as adjustable hardware capabilities. Other conditional
legacy fields remain readable so older custom presets retain their interpretation.

## Grille and HDR

The mask is normalized in linear light: its dark gaps concentrate the average
emission into brighter phosphor regions. HDR provides room for those peaks;
it does not create extra physical subpixels or recover detail below the panel's
resolution. At insufficient headroom, the output shoulder reduces resolved
phosphor peaks, lowering mean brightness. Increasing gain indefinitely then
compresses highlights instead of restoring an unbounded glow.

The decoder preserves superwhite and undershoot as voltage through the video
amplifier; gun cutoff and supply loading precede emission. Nominal white is not
treated as an amplifier rail. Exact amplifier saturation remains uncalibrated.
The final shoulder scales the entire linear RGB vector instead of clipping
channels independently. Extended-linear HDR also retains negative sRGB
coordinates introduced by the phosphor-primary conversion, allowing the host
colour manager to reproduce colours outside sRGB on capable panels. SDR reduces
out-of-gamut chroma towards an equal-luminance neutral before output encoding.
Available headroom and SDR white are queried from the window each frame.

An actual GPU render-target test uses a uniform 0.25 linear input and emission
gain 2. For the aperture grille, measured red-channel means and maximum RGB
peaks are:

| RGB period in drawable pixels | SDR mean / peak | Headroom 4 mean / peak |
|---|---:|---:|
| 3 | 0.4824 / 0.8271 | 0.5000 / 0.8618 |
| 6 | 0.4249 / 0.9072 | 0.4998 / 1.1748 |
| 12 | 0.3580 / 0.9434 | 0.4998 / 1.6035 |

These are linear framebuffer measurements, not nits measured on a MacBook.
Regression tests also check RGB balance, unresolved-mask averaging, ambient
independence, highlight slopes and output limits. Real panel peak brightness,
local dimming, viewing distance and ambient reflections remain external factors.

An additional before/after Contra check used unaveraged 3840×2880 phase pairs,
unchanged presets and a simulated headroom of 4. Removing the decoder's early
0–1 clamp produced these maximum linear RGB components:

| Preset | Before | After | Mean scene luminance change |
|---|---:|---:|---:|
| Sony PVM-14L2 | 1.720 | 2.920 | +0.31% |
| JVC D-Series | 3.338 | 3.742 | +0.51% |
| Toshiba 14AF43 | 3.236 | 3.631 | +0.26% |
| Stas's Favourite | 3.313 | 3.502 | +0.31% |

These are isolated highlight changes, not an increase to preset gain. All four
also stayed within a simulated 1.6 headroom. Fixed-exposure SDR previews were
inspected at native crop resolution; they cannot demonstrate actual HDR glow.
The companion PFM capture preserves signed HDR values; PPM clips to SDR white.

## High-refresh presentation

**Host display → Presentation → 60 Hz hold** targets picture presentation at
60 per second, without dark-frame insertion. The command-line equivalent is
`--presentation 60hz`. Each picture remains visible until its replacement,
including across additional panel refreshes. This is a session preference,
independent of CRT presets; Hold remains the default.

This mode preserves emulated PPU/APU cycles and genuine NTSC phase alternation.
It does not freeze or average the composite pattern. The frontend paces NTSC
frames at 60 instead of about 60.0988 frames/s (about 0.16% slower wall-clock
playback), with matching audio resampling. This avoids periodic frame drops
from running a faster source against a 60 Hz presentation clock. Absolute
presentation deadlines prevent timer overshoot from accumulating. PAL remains
about 50 frames/s. Audio continues independently if rendering stalls.
The bundled macOS Metal backend submits work early and uses timed drawable
presentation. Its initial deadline is two source intervals ahead (about 33 ms
for NTSC), with two frames allowed in flight. Hold uses the native source
interval; 60 Hz hold uses 16.667 ms for NTSC. Skipped source frames retain their
place in the schedule, and long stalls reset the deadline. This trades video
latency for consistent exposure of the alternating composite phases. Other
backends and offscreen tests retain CPU submission pacing in 60 Hz mode.
Neither path can hide a sustained rendering overload or OS scheduling stall.
The option controls application timing; it does not change the monitor's
refresh rate. A fixed 144 Hz panel still cannot show 60 evenly spaced updates
without a matching display mode or variable refresh.

The serrated colored edges in composite video are consistent with cross-luma
(dot crawl): see the [AD723 encoder datasheet](https://www.analog.com/media/en/technical-documentation/data-sheets/AD723.pdf).
Irregularly holding or skipping phases can turn that regular pattern into
intermittent shimmer. `MYNES_PRESENT_TRACE=/tmp/presentation.csv` records each
submission's source frame, carrier phase, presentation mode and refresh slot.
This helps distinguish missed pictures from normal phase changes; submission
timestamps alone cannot establish what the panel actually displayed.
The bundled Metal backend also supports `MYNES_METAL_PRESENT_TRACE=1`, which
logs actual drawable presentation timestamps, deadlines and source frames.
`frontends/gpu/tests/test_metal_presentation.py` runs a windowed composite/GPU
audio review and reports visible intervals. Keep its window visible; a zero
Metal presentation timestamp is not a successful displayed frame.

Host display → Presentation → BFI enables optional dark-frame insertion.
Dark refresh defaults to 0.15, so switching to BFI gives each dark refresh 15%
of its paired bright refresh's phosphor light. Override it with
`--dark-frame-level F` (0–1); 0 gives fully black refreshes.
This is linear-light dimming, not
window transparency. The preference is session-only
and deliberately absent from television presets.

The current display must report approximately 2–8 refreshes per source frame:
120/240 Hz for NTSC, 100/200 Hz for PAL, for example. 60 Hz, unknown refresh and
144/60 combinations use Hold. The OSD shows when BFI is inactive. If submission
cadence falls below 85% of the reported rate over 60 intervals, BFI suspends;
toggle it off/on after resolving the cause. Moving to another display or
changing its reported mode restarts the check. Submission times can detect
slow pacing but cannot certify physical scanout, especially under variable
refresh/compositing.

Only the first refresh runs the NES signal/beam pipeline. Additional refreshes
reuse its output and glass-scatter textures; the final phosphor/glass pass runs
again. Audio, PPU timing, carrier phase, phosphor history and supply state advance
once per emulated frame. Blank/dim refreshes affect emission (including its
pedestal), not room reflections. Bright-refresh gain compensates the duty cycle
before the HDR shoulder; insufficient peak headroom still reduces average
brightness. BFI and a strong grille compete for the same available headroom.

This reduces display hold time when the host presents the requested cadence;
it is **not a simulation of a continuously moving CRT beam**. At 120 Hz a lit
refresh still lasts about 8.3 ms, with the LCD's own response on top. A rolling
exposure model would need source history and time-integrated phosphor decay;
a black horizontal band alone would not reproduce that behavior.

`MYNES_PRESENT_TRACE=/tmp/presents.csv` records every submission with source
frame number, refresh slot and reported display Hz. Ordinary playback traces
continue to count source pictures only. Offscreen captures use Hold, so a still
capture remains comparable across machines.

Validation: CPU tests cover rate eligibility and pulse-energy integration;
actual HDR render-target tests verify emitted-light integration and unchanged
ambient margins. The local 60 Hz desktop correctly remained in Hold (~60.10 submissions/s).
A user test on a 120 Hz MacBook reported improved motion. Its captured trace
contains a continuous 126.77-second BFI segment at 119.78 submissions/s; the last
1,000 submissions average 120.02/s. Bright/dim slots share the same source frame.
These are submission measurements and subjective viewing, not photodiode or
high-speed-camera validation of physical scanout.

`black_floor` is minimum gun drive, applied before transfer and spot deposition.
It preserves scanline structure in residual emission; use ambient light for a
room-lit glass pedestal. Raising black floor does not illuminate blanked raster.


## Recording, advanced controls and validation

Signal chain → RF receiver exposes the complex IF response. Signal chain →
VHS recording / playback controls the optional NTSC composite recording path.
The `VHS SP` preset pairs it with a consumer slot-mask CRT. Slow-decay time and
energy are in Mask / phosphor. The additional tube controls are grouped with
beam, gun, phosphor, glass and wear settings rather than hidden in preset JSON.

See [model changes and evidence](architecture/gpu-realism-validation.md) for
what is implemented, tested, estimated and still outside the simulation.
`python3 frontends/gpu/tests/test_preset_controls.py` audits every shipped preset
against saved TV-field coverage and OSD ranges. Legacy focus/height values apply
only when the corresponding explicit FWHM is zero; otherwise use FWHM.
