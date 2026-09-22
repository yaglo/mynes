# Signal and CRT model validation

This pass implements the five stages in [the plan](gpu-realism-plan.md).
Model invariants, published specifications, generic circuit simulations and
physical measurements are different evidence. No new physical measurements of
a PVM-14L2, NES RF module or VCR were made.

## Optical emission and temporal response

Matte surface scatter now filters emitted phosphor coverage, rather than
blurring only the image behind an untouched grille. Its radius follows phosphor
pitch. A uniform field keeps its mean light while grille contrast decreases.
The optional drive/color response uses unmasked current; changing drawable
pitch no longer changes its transfer curve. This remains a generic material
control, not a fitted spectral model.

Optional fast and slow decay states have separate time constants and a shared
integrated-energy weight. Both have unit DC gain; elapsed emulated frames affect
both, and reset/resize cannot read stale state. A second history allocation and
its memory traffic exist only when the tail is enabled. This is a frame-sampled
two-exponential approximation, not a continuous scanout or universal P22 model.
[Kuhn's measured CRT study](https://www.cl.cam.ac.uk/~mgk25/ieee02-optical.pdf)
shows that P22 is a broad family, with mixtures of exponential and power-law
responses. Its coefficients cannot be assigned directly to the named presets.
The VHS profile's 1.5% / 8 ms tail is explicitly an estimate.

Source-current deposition now continues widening above reference white in
both axes. Horizontal kernels remain normalized. In an isolated PVM-profile
GPU beam test, drive 0.1/0.25/0.5/0.75/1.0/1.2 produced equivalent FWHM
0.388/0.423/0.529/0.690/0.900/1.102 scanlines. Integrated current stayed within
0.01%. These numbers validate the selected curve; they are not Sony measurements.

## RF

The channel produces a negative-AM complex envelope and independent complex
Gaussian noise. A complex FIR then filters **both** before envelope detection.
The asymmetric IF admits one sideband more strongly, allowing quadrature
(envelope) distortion that a post-detection video lowpass could not reproduce.
The nominal transition uses the System-M 0.75 MHz reference and a selectable
video edge. [Canada's published System-M specification](https://ised-isde.canada.ca/site/spectrum-management-telecommunications/en/devices-and-equipment/broadcasting-equipment-standards/bts-3-television-broadcasting)
specifies 4.2 MHz main and 0.75 MHz vestigial sidebands. The implementation uses
those scales as a **generic receiver** reference, not proof of a particular
console modulator or receiver IF shape. The local AM source is not represented
as a broadcast VSB transmitter.

IF asymmetry and tuning offset are saved, live controls. Carrier frequency
remains channel metadata. There is no RF oscillator, intercarrier sound,
measured SAW filter or complete NES RF/power-module circuit here. DC normalization
keeps constant levels independent of IF tuning. The scalar visualizer shows
voltage before modulation and after detection; it does not plot I/Q as if they
were successive video samples.

A GPU sideband probe measured amplitudes 0.030511 and 0.000003 for equal opposite
sidebands. Constant carrier levels survive the filter/detector. Decoded-grey
noise correlation across lags 1–12 stayed below 0.007; seeds do not repeat with
the NES's color-phase cycle.

## VHS

`vhs_sp_consumer` records the NES composite signal and plays its recovered
response into a consumer CRT. The tape stage precedes TV sync/burst reception.
It separates a recovered luma lowpass from narrow chroma sidebands, adds envelope
delay without delaying the chroma oscillator, and models small residual line
clock/phase errors and playback noise. Sync and burst traverse the same path.
It is active only for NTSC composite/RF; component, RGB and separated Y/C inputs
bypass this composite recording path.

The format architecture follows the luma-FM/color-under distinction described
in [JVC's HR-S8000U manual](https://library.mikesservers.com/J/JVC/HRS-8000U/HRS8000U_OM_SONY.pdf)
and [US6459848B1](https://patents.google.com/patent/US6459848B1/en), which identifies
VHS chroma at 629 kHz. Here that down/up conversion is collapsed into its
recovered response. 2.5 MHz luma, 0.35 MHz chroma, delay and error strengths are
estimated playback characteristics. Playback luma equalization now adds a DC-neutral
high-frequency shelf and a causal tail while retaining the separation filter's
stopband. Separate luma and chroma noise envelopes use signal-sample coordinates;
luma mixes fine grain with a longer horizontal component, while chroma noise
occupies narrow envelopes around the reconstructed subcarrier. Their RMS
controls specify voltage before the receiver, not final screenshot pixel noise.

Transport timing and colour-phase errors interpolate continuously through a
random field over successive source frames. Grain is renewed each source frame.
Optional head-switch displacement is confined to the last six active picture
lines (usually cropped by overscan). Dropouts are sparse horizontal reductions
of recovered signal with a local noise increase. These are phenomenological
playback defects; magnetic recording, FM threshold/demodulation, tracking servos,
tape speed and VHS audio remain outside the model. No particular deck's noise
spectrum or transport constants are claimed as measured.

After the September 22 4K review, the SP preset uses 0.018 luma and 0.004
chroma RMS, 180 ms transport correlation, 100 ns switching displacement,
and 0.15 dropouts/second at 65% peak loss. Chroma delay is 250 ns. Receiver
brightness 0.08 and luma contrast 0.94 make ordinary black grain visible while
keeping the white patch within 1% of its previous output. These are authored
preset choices; see the [before/after audit](../gpu-preset-audit.md). The noise is upstream of receiver
clamping, bandwidth limits, and the tube's gun cutoff: below-black NES colours
can remain visually quiet. A mandatory 10–15/255 black pedestal or 1–2 NES-pixel
chroma displacement would not be a format-wide physical calibration.
All playback controls are available under Video → VHS recording / playback;
older saved profiles retain their legacy noise and default new defects to zero.

Actual GPU tests preserve DC grey and the 3.58 MHz carrier, while attenuating
an upper sideband 1 MHz away from the carrier by more than 49 dB relative to the
carrier. Tests also check noise RMS, horizontal/vertical and inter-frame
correlation, smoothly changing timing, switching-band and dropout locality,
maximum offsets across workgroup/line boundaries, and DC preservation with
playback equalization. This checks the implemented model, not a particular
VCR's measured response.

Validation on 2026-09-22: Metal readback measured 0.00999 luma RMS for a
0.010 request, adjacent-sample correlation 0.981 and adjacent-line correlation
−0.003. Five targeted suites passed (fidelity, preset JSON, control coverage,
signal precomputation, and complete pipeline). Matching 1280×960 captures were
inspected. The complete VHS video-chain benchmark, 60 frames after 12 warm-up
frames, averaged 6.680 ms at 1280×960 and 12.463 ms at 2560×1920. These are
CPU-submit-to-GPU-fence times, excluding emulation, audio, vsync and readback;
they are not a physical presentation-cadence measurement or a before/after
speedup claim.

## Raster edge and fixed glass aperture

The source bounds and beam spot define the raster perimeter. The former second
UV-space fade (roughly ten display pixels wide at 1280 pixels) has been removed.
The final optics pass clips emission and specular room reflection against the
fixed curved glass aperture with one-pixel coverage antialiasing. Service size,
position and overscan move the raster inside that aperture. Outside it, the
same diffuse ambient surround as the render-target clear is retained, independent
of emission gain. This is a consistent surround treatment, not a measured bezel
material or full three-dimensional tube face. Simulated ambient reflection and
specular glare are gated together by the host's Room reflections setting (**G**),
off by default. It zeroes their final display uniforms without mutating preset
parameters or suppressing intrinsic bloom/internal glass scatter. The real-render
regression (`python3 frontends/gpu/tests/test_room_reflections.py build/bin/mynes_gpu`)
compares six 3840×2160 captures: legacy default-off, explicit on, saved on,
explicit off overriding saved on, zero room strengths, and zero internal scatter.
Default-off is byte-identical to enabling reflections with both room strengths
zero; intrinsic scatter still changes the image.

The existing analytic aperture-grille filtering and mip-filtered slot/dot masks
remain resolution-aware. An unresolved fine mask is expected to average out;
forcing it sharp would introduce aliasing rather than improve fidelity.

## Controls and calibration

All 121 saved TV fields (including conditional legacy focus controls) are
represented in the OSD. RF and VHS have their own signal-chain submenus.
The live editor inherits remaining float controls from the OSD; its transport
now accepts up to 192 controls. Enums/toggles remain available in the OSD.
Preset dirty tracking covers all saved numeric state. Live cable reflection
and RF hum updates no longer wait for a preset reload. Zero audio noise/hum
in a preset now means zero instead of inheriting an enabled default.

`test_preset_controls.py` checks every shipped preset's finite values and OSD
ranges. This does not establish that every estimated parameter is physically
correct for every unit. Metadata (`num_sections`, RF carrier frequency,
unused console coupling capacitance and unused audio-cable metadata) remains
readable for compatibility, without a nonfunctional OSD knob.

Use the `beam`, `recovery`, `chroma`, `pluge` and `chart` options of
`frontends/gpu/tests/capture_patterns.py` for reproducible PPU-code patterns.
Measure each final linear capture phase separately. Phase averaging is useful
for some color/energy comparisons but can conceal or broaden beam structure.
The independent [RC/ngspice check](../../tools/circuits/README.md#crt-rail-implementation-check)
compares actual GPU load samples with an equivalent circuit.

## Final checks and complete playback

The 12 GPU CTest targets pass, including preset/control coverage, signal and
rendered-output fidelity, audio and playback. Editor IPC passes fragmented and
stalled-reader traffic, preset editing/saving and the expanded controls. The
Swift protocol suite passes all five tests. Offscreen output is independent of
the hidden window's drawable size.

The generic 12 µs rail circuit and actual GPU state agree within 1.86e-7
normalized volts across 256 samples (RMS 7.95e-8). This establishes the RC
implementation, not that a particular television uses those component values.

[Complete UHD playback](../gpu-benchmark-results.md#uhd-playback-and-high-refresh-presentation)
includes core, every applicable signal/CRT stage, GPU audio with deadline
fallback, and periodic full readback. The final run renders all five profiles at about 60.1 fps with no skipped picture. The preceding RF and VHS runs skipped one and two; both runs are recorded, and the difference is not claimed as a speedup. Physical vsync, photon timing and acoustic latency are outside this
measurement. Shared-memory FIR input reuse and symmetric IF tap pairing reduce
redundant work; the new response is not advertised as a free performance gain.

All 21 preset stills are recaptured at 3840×2880. Native crops use one phase;
phase clips retain consecutive frames. The PVM keeps a finer grille and narrower
dim spots, JVC/Toshiba show broader consumer spots, and RF/VHS change the signal
before the CRT. Legacy profiles still overlap and are optional looks. These
SDR exports cannot establish HDR luminance or match a photographed tube without
known camera exposure and display settings.


The TV menu now has a translucent RGB background and a bottom-strip adjustment
view. GPU checks cover alpha mixing, unaffected pixels and removal of stale
OSD data; navigation checks cover entering without changing a value and returning
to the same row. The pass is disabled when no menu or notice is present. The
original PPU codes remain unchanged. Documentation includes both menu views.

GPU audio now starts by default, retaining the deadline-based CPU fallback.
An offscreen 600-frame Mario capture exercised the default with no environment
override: 591 GPU-processed blocks, 440,216 samples, maximum CPU/GPU difference
0.00000801, maximum queued audio 53.6 ms. Both backends kept supplying samples
through a forced 250 ms presentation stall. All 12 GPU tests passed after
integrating the independent mapper updates. `MYNES_GPU_AUDIO=0` remains an
explicit CPU override; benchmark CPU modes now set it rather than relying on
the previous default.

## Initial 60 Hz presentation validation (historical)

This section records the initial implementation; the 2026-09-22 matched-refresh
update below supersedes its pacing description on 60 Hz panels.

`--presentation 60hz` / Host display → Presentation → 60 Hz hold used absolute
deadlines, one frame of preparation margin, and one frame in flight. NTSC
frontend pacing changes from 60.0988 to 60 frames/s (0.16% slower wall time);
audio resampling applies the same ratio. Emulated cycles and carrier phases
are unchanged. PAL keeps its native slower cadence. Neither frame averaging
nor phase freezing is involved.

The final 900-frame offscreen Mario comparison at 1280×960, with the complete
PVM chain and default GPU audio, measured:

| Mode | Submissions/s | Median interval | p95 interval | Longest interval | Skipped pictures |
|---|---:|---:|---:|---:|---:|
| Native Hold | 60.079 | 16.507 ms | 20.917 ms | 25.516 ms | 0 |
| 60 Hz hold | 60.000 | 16.667 ms | 16.727 ms | 17.922 ms | 0 |

All 900 source-frame phases matched. Maximum queued audio was 50.3 ms in the
60 Hz run, with 886 GPU-processed blocks and deadline fallback for the others.
Earlier runs without preparation margin showed p95 intervals around 20 ms and
up to four missed pictures; those observations motivated the extra margin.
This is a submission measurement under the observed load, not a physical
120 Hz scanout measurement or a guarantee against scheduling stalls. The local
live-preview display reported 60 Hz. Repeat with
`frontends/gpu/tests/test_presentation_playback.py`; presentation traces now
include actual source phase and selected mode for on-device diagnosis.

## Metal fence and presentation follow-up (2026-09-21)

The reported hang sampled the main thread in SDL's swapchain fence wait while
the playback thread continued GPU audio. SDL 3.4.16 has two upstream-confirmed
Metal fence races relevant to that concurrent workload. macOS now statically
links that release with fixes 10309e3 and d8451e5; the build verifies the patched
source hash. No Homebrew installation changes are required. Swapchain capacity
is polled with empty command buffers cancelled, so that fence wait no longer
blocks the event loop. Drawable acquisition can still wait for platform vblank.
The original intermittent hang was not deterministically reproduced.

Real Metal drawable timestamps exposed a limitation in the earlier offscreen
60 Hz validation. On the M5 Pro's reported 120 Hz display, visible frames could
alternate 8.33/25 ms despite roughly 16.67 ms CPU submissions. A local SDL
extension now submits early and schedules native Hold / 60 Hz Hold presentation
with `presentDrawable:atTime:`. Source phase is unchanged. With two intervals of
startup lead, the final bundled build's visible M5 Pro run recorded 1,739
intervals after warmup: 1,735 near 16.67 ms, two at 25 ms, one at 12.5 ms and
one at 8.33 ms. All drawables were shown. This substantially reduces the
observed irregular exposure but does not eliminate host presentation misses.
Fully occluded runs returned zero drawable presentation timestamps and are not
evidence of visible cadence.

The final bundled build's local 60 Hz windowed Reference Composite test recorded
839 visible intervals at 16.67 ms, zero source skips and 899 GPU audio blocks.
The 13 GPU CTests pass, including 20,000 concurrent fence/readback cycles and
120 frames of a static composite colour edge: same-phase output repeats while
the two carrier phases remain distinct. Separate 900-frame Hold / 60 Hz runs
preserved identical carrier phase on all 900 matching source frames, with no
source skips. These checks do not constitute a
photodiode measurement or prove that every host scheduling stall is eliminated.

## MacBook Air, PAL and MMC5 follow-up (2026-09-22)

On the M5 MacBook Air's fixed 60 Hz panel, BFI falls back to an ordinary lit
presentation. Its stable appearance pointed to the different timed-present path
used by Hold. Both hold modes now use ordinary vsync when the display refresh
matches the target within 0.5%, with one frame in flight. Playback follows that
display period and waits for room in its bounded queue instead of dropping
source phases. Unmatched refresh rates retain the Metal timed-present path.

Visible Metal timestamps at a 2560×1542 drawable, Reference Composite and GPU
audio enabled recorded 539/539 intervals near 16.67 ms in native Hold. The final
60 Hz Hold run recorded 838/839 near 16.67 ms and one 50 ms interval. Both had
zero skipped source frames and zero unshown drawables. Thus the competing-clock
path is removed on this panel, but an occasional host stall remains measurable;
this is not a claim of perfect physical-panel exposure or a subjective flicker
test. GPU audio can fall back to CPU processing within its deadline.

PAL now uses the 16:5 CPU/PPU master-clock ratio, regional APU frame sequencer,
noise and DMC periods. Legacy iNES files with explicit European filename tags
can select PAL when their header leaves the timing bit unset. Triangle register
writes preserve oscillator phase, a stopped triangle retains its DAC level,
and halt/reload writes are ordered correctly against length-counter clocks.
Volume writes no longer retrigger envelopes. All ten bundled PAL APU timing
ROMs pass; their README states they were verified on a PAL NES. They are now a
permanent CTest alongside the direct APU checks and AccuracyCoin.

MMC5 now routes each nametable quadrant independently (CIRAM, ExRAM or fill),
selects CHR banks by fetch type and sprite size, and derives its scanline IRQ
from actual PPU reads including the odd-frame boundary. A scripted Castlevania
III (USA) replay at frame 6,000, BLK 1-02, matched an independent FCEUmm render at
all 61,440 pixel positions after palette-colour remapping. The equivalent PAL
replay and an offscreen composite render retained the background walls. The
user's later missing-wall screenshot was not reproduced with this build. These
checks do not cover every MMC5 feature: extended attributes, vertical split and
PCM still need separate work. Asterix PAL's raw framebuffer now has its blue
background; its reported audio hum still needs listening confirmation.
