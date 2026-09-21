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
estimated playback characteristics. Magnetic recording, FM threshold noise,
head switching, dropouts, tracking, tape speed and VHS audio are not simulated.

Actual GPU tests preserve DC grey and the 3.58 MHz carrier, while attenuating
an upper sideband 1 MHz away from the carrier by more than 49 dB relative to the
carrier. This checks recovered bandwidth, not a particular VCR's response.

## Controls and calibration

All 116 saved TV fields (including conditional legacy focus controls) are
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

## Optional 60 Hz presentation

`--presentation 60hz` / Host display → Presentation → 60 Hz hold uses absolute
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
