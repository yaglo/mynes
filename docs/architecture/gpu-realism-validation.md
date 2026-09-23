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

`vhs_sp_consumer` records the NES composite signal on an NTSC SP consumer
deck and plays it back into a slot-mask CRT. Two compute stages run on every
raster line before TV sync and burst reception: `vhs_tape` (record,
transport, FM tape, demodulator) and `vhs_playback` (dropout compensator,
luma playback, 1H chroma comb). The host builds a timing table and a dropout
list for each frame in `vhs_deck.c`. The stage is active only for NTSC
composite and RF; separated, component and RGB inputs bypass it. Units inside
the kernels are IRE referenced to the input sync. The deck's keyed AGC holds
NES sync at -40 IRE (119.4 IRE per chain unit) and the deck outputs 7.14 mV per
IRE, so its output is standard level, 8% above the NES itself.

### Record

- Luma: third-order Bessel low-pass at 3.4 MHz with a colour trap (Q 2),
  more than 40 dB down at fsc (IEC 60774-1 6.1.1). IEC pre-emphasis, time
  constant 1.3 us and X = 4 (IEC fig. 23), clipped at 160% white and 40% below
  sync tip (IEC 6.1.2; JVC table 3-2-1 gives +10/-5% and +-10%). Deviation
  3.4 MHz at sync tip to 4.4 MHz at white (IEC table 1).
- Chroma: second-order Butterworth band-pass at fsc +-0.5 MHz on the
  composite, a product detector on the fixed 12-sample carrier grid, and a
  record ACC that holds the burst envelope at 20 IRE. The burst is recorded
  6 dB hot, so the colour-under noise on it is halved.

### Transport and tape

The recorded frequency and the colour envelope are read back at t - tau(t)
from the timing table. The carrier grid itself never moves: the deck's
up-converter is built from a crystal fsc reference locked to burst (APC) and
40 fH locked to the playback sync (AFC) (JVC colour playback). Timing moves
luma and chroma content by the same amount and leaves the hue alone. The
earlier stage shifted the whole composite, which rotated hue by 1.29 degrees
per ns of timing error.

The timing table (per raster line, per frame) is a pure function of the deck
seed and the emulated frame number, measured on a Panasonic PV-7450 capture
(DH):

- a fixed bow per head, fitted with 10 scan harmonics: 77 ns RMS on head A,
  39 ns on head B;
- a varying part over 22 scan harmonics whose per-harmonic RMS (62.5, 14.3,
  7.6, 2.9 and 1.2 ns for harmonics 1, 2, 3-4, 5-9 and 10-22) was found by
  inverting the DH band analysis (240 lines per field, a per-field linear
  detrend, Hann periodogram; bands 0-70, 70-140, 140-300, 300-600 and
  600-1500 Hz at 45.4, 23.3, 10.9, 6.4 and 4.1 ns), so the same analysis of
  the table returns those bands. The detrend removes 40% of the first
  harmonic's power, which is why its RMS exceeds its band. The parameter
  `tbe_varying_ns` is the bands' quadrature sum, 52.5 ns for the DH deck.
  40% of the power persists as a damped resonance with a 2 s decay and a
  1.7 s period, the rest is new every field: field correlation 0.40, 0.27
  and 0.04 at lags 1, 12 and 24 against DH's 0.41, 0.35 and 0.05, where a
  plain exponential cannot hold 0.35 to lag 12 and fall to 0.05 by lag 24;
- 5 ns RMS of white per-line jitter;
- the head switch 6.5 H before vertical sync (NES line 238.5; JVC, IEC 5-8 H)
  with a step at each switch, a 1.5 dB RF envelope sag over the 3 lines before
  it and a random RF phase jump at it. DH measured +1700 ns (B to A) and
  -80 ns (A to B) on an interchange tape: the sum of the two, which sets the
  -3.1 ns per line ramp, is the tension term that any tape shows, and their
  difference is the dihedral term that cancels when the deck that recorded
  the tape plays it, since each head then reads its own track. The presets
  describe that same-deck case, so both steps are +810 ns; the DH values are
  a menu setting. A whole-number switch position switches at the start of
  its line.

The deck's own filters and delay line hold the output about 1 us behind its
input. The table removes that fixed delay, so the TV sees the timing error and
not a constant offset.

On tape, the FM carrier gets 2700 Hz RMS of modulation noise below 0.4 MHz,
the record high-pass (third order at 1.6 MHz: 24 dB down at 629 kHz, 13 dB at
1 MHz, IEC fig. 22) and the head/tape loss. Spacing loss falls exponentially
with frequency, a straight line in dB; three real poles a factor 1.65 apart,
placed together for the measured -2 dB/MHz at 3.9 MHz (DH carrier level:
+0.8 dB at 3.45 MHz, -1.2 dB at 4.45 MHz), follow that line within 1 dB over
1.4-6 MHz for every tilt the menu allows, up to 4.4 dB/MHz. Playback adds
RF noise at a carrier-to-noise density of 95.5 dB Hz (head B 0.8 dB worse).
The noise is real, so its analytic form has only positive frequencies: the
white complex draws pass a fourth-order low-pass of 7 MHz rotated to
+7.5 MHz, which keeps the density on the positive side out to where the
playback RF low-pass sets the noise bandwidth and takes the image at -1.4 to
-6 MHz down by 9 to 23 dB. A lost carrier then demodulates to the noise's
zero-crossing rate, above white, as a pulse-count discriminator does. Then
the playback RF filters (second-order high-pass at 1.4 MHz, JVC fig. 3-2-12;
second-order low-pass at 6 MHz). A limiter and pulse-count
discriminator (JVC's delay-line switching demodulator) turn the phase advance
per sample into luma, which then passes a third-order Bessel at 3.0 MHz
(JVC fig. 3-2-16), de-emphasis, the noise canceller (the high band above
0.5 MHz, limited to +-3 IRE, is subtracted; US4698696 gives 0.3-1 MHz and 2-5%
of white), a two-tap aperture (233 ns, k = 0.2) and the Y delay line. The Y
delay is computed from the group delays of the actual filters.

The colour-under envelope gets 0.8 IRE of noise per component (DH), the
playback band-pass (second order, 0.5 MHz), the APC/AFC residual phase and
the 1H comb (current line plus the glass delay line, 227.5 carrier cycles).
The APC residual comes from a second-order loop at 1 kHz (an assumption: JVC
calls APC comparatively rapid) driven by the timing and by the burst phase
noise the modelled chroma noise gives: the noise on the burst, recorded 6 dB
hot, averaged over the 10-cycle burst gate against the 20 IRE burst, 0.65
degrees per line at the default noise (DH measured 2.1 degrees, which
includes its own measurement noise). The loop samples one burst per line,
so its natural frequency is held to 2 kHz, below where the per-line
recurrence would run away.

Luma RF runs at 12 fsc as an analytic signal. The record FM high-pass removes
more than 10 dB below 1 MHz, so the sidebands a real signal folds around 0 Hz
are already gone. Every IIR filter is a set of first-order partial-fraction
sections from the bilinear transform, each factor prewarped at its own corner,
run as a two-level scan across the threadgroup.

### Dropouts

Dropouts are placed on tape from the DH rates: 32.8 per second reaching
-6 dB, 13.3 at -10 dB, 8.0 at -15 dB, 5.5 at -20 dB, 4.25 at -30 dB and 1.5 at
-40 dB. Their length depends on depth (median 2.1 us for -6 to -10 dB,
15.5 us to -15 dB, 42 us to -20 dB, 43 us to -30 dB, 70 us deeper). 31% of
them continue on the next track 1.5 H later (DH), up to four tracks, each
0.6-1 times as deep. The rates count every event, so births are divided by
the 1.278 events each produces. Depth sets a head-to-tape spacing; Wallace
spacing loss, 54.6 d f / v dB at 5.8 m/s, costs a 1 um defect 37 dB at 3.9 MHz
but only 5.9 dB at 629 kHz, which is why the colour survives most dropouts.

The dropout detector integrates the RF envelope over 0.35 us and switches at
-15 dB with 1.94 dB of hysteresis (vhs-decode). The compensator substitutes
the recovered luma one glass delay line earlier, up to four lines back (JVC
recirculating loop), with a click at each switch from the random phase jump
between two FM signals. Without it the pulse-count discriminator turns the
lost carrier into a white streak. Shallow dropouts only raise the noise.

### TV line PLL

The VHS preset sets the TV's horizontal loop to a second-order PLL (150 Hz
in the worn-tape preset, 250 Hz for the fallback) with damping 0.7, its
detector gain raised 2.5 times for the 21 lines from vertical sync (TDA2579:
head-change jumps are restored within the vertical blanking). It free-runs
through lines without a sync edge and is never reset, so the head switch
bends the top of the next field. A 1 us step at line 238.5 leaves -72 ns at
the first picture line, -52 ns at line 20 and -3 ns at line 40. The 250 Hz
bandwidth is an assumption, and the largest single control over how much
tape timing the viewer sees: at 100 Hz the mid-field residual is 99 ns RMS,
at 500 Hz 8 ns. The loop samples one sync per line, so its natural frequency
is held to 1 kHz and its V-blank gain to what keeps the per-line recurrence
stable.

### TV sync separator and black clamp

A VCR moves lines by up to a few microseconds, and the receiver has to
follow without turning timing into level. The sync separator first finds
the trailing edge on the composite averaged over one subcarrier cycle,
anywhere from 3 us early to 4 us late (dots 8 to 46), where the earlier
separator searched only 1.3 us around the nominal edge and lost lock on
VCR lines. It then slices the raw samples within a dot of that edge, with
the tip and black windows placed relative to the edge instead of the line
start: the tip over 8 dots of the pulse's interior, the slice halfway to the
front porch capped at half the NES sync depth, and black over two whole
subcarrier cycles 21 to 24 dots behind the edge. With the sync where the
NES puts it these are the earlier fixed windows, so clean composite and RF
pictures keep their level and position (within 1e-3 linear RMS on the
Contra boss frame through the PVM, Toshiba and RF presets; RF differs only
in which noise crossings each line picks, with the same edge jitter).

The black measurement charges a keyed clamp with a time constant in lines
(`clamp_lines`, a menu item under Y/C separation, saved with the preset).
Its default is the receiver's earlier fixed 0.35 per line, 2.3 lines; no
measured value for a named TV has been found. The clamp now holds through
vertical retrace instead of taking the first line after it outright, which
moves the top rows of an AC-coupled picture by a few 1e-4. The clamp time
constant decides how much tape noise becomes whole-line flicker: through
the whole chain on a flat grey field with the deck's defaults, the whole-row
share of the decoded luma's temporal variance is 17.8% at 2.3 lines, 6.4% at
8 and 2.8% at 64.

The 30 Hz head-alternating brightness the review measured (1.2% on flat
grey with the interchange skews) is gone with this receiver at the default
clamp: head A and head B fields decode within 0.006%, and every band of
rows within 0.034%. A black window placed a dot earlier, 20 dots behind the
edge, doubles the whole-row share (the deck's burst tail still reaches it);
one placed later runs into the NES border, which carries the backdrop
colour.

### Left out

- Chroma crosstalk from the adjacent track: the 1H comb cancels it where the
  neighbouring picture is vertically uniform; about -25 dB of half the colour
  step remains at colour boundaries.
- Luma crosstalk: NES fields are 262 lines, so the neighbour's syncs are 0.5 H
  off. With 5 um of tracking error that is about 1 IRE at 1.1 MHz after
  de-emphasis and 0.4 IRE after the canceller.
- The Hi-Fi audio beat at carrier - 1.7 MHz, the tracking bar, LP and EP, ACC
  settling (the NES burst is constant) and a head DC offset.

### Tests and measurements

`gpu_vhs_deck_tests` checks the host model: filter responses (record Y
-136 dB at fsc; record FM high-pass -24.4 dB at 629 kHz and -12.6 dB at
1 MHz; tape slope +0.91 / -1.18 dB, within 1 dB of a straight line over
1.4-6 MHz, and -4.6 dB/MHz at the -4.4 setting; the noise shaping 0 dB at
7.5 MHz, -11 dB at -1.4 MHz and -35 dB at -6 MHz), the APC burst noise
0.65 degrees, tables identical for the same seed and frame, field
correlation 0.36, 0.23 and 0.02 at lags 1, 12 and 24, the DH band analysis
of the table returning 44.2, 22.3, 10.9, 6.6 and 4.0 ns (quadrature 51.2 ns
against the 52.5 set), no line in the spectrum of the per-frame timing
change (peak 2.2 times the median), switch steps equal to the skews at a
half-line and a whole-line switch position, line-to-line jitter 7.04 ns
(5 ns times root 2), every deck control at each end of its menu range giving
a finite, bounded table, 32.6 dropouts per second at -6 dB, 5.76 at -20 dB
and a recurrence of 0.303.

`gpu_fidelity_tests` runs both kernels on synthetic NES rasters and compares
them with a numpy reference model built from exact analog responses and real
RF:

| Property | GPU | Reference | Earlier stage |
|---|---|---|---|
| Grey $10 level | 80.00 IRE | 80.0 | |
| Output burst | 39.8 IRE p-p | 40 | |
| Luma noise 0-2.4 MHz at black, canceller off | 0.850 IRE | 0.87 | about 2.0 |
| White / black noise | 1.314 | 1.26 | 1.0 |
| Share below 0.5 MHz / 2-2.9 MHz | 6.7% / 46.3% | 6% / 49% | 42% below 0.5 MHz |
| Noise autocorrelation 0.5 / zero | 87 / 140 ns | 93 ns | 222 / over 300 ns |
| Luma noise with canceller | 0.54 IRE, 28% below 0.5 MHz | 0.49-0.62, 21-28% | |
| Chroma noise | 0.53 IRE per component, adjacent lines 0.47 | 0.53-0.55, 0.46-0.49 | 0.51, 0 |
| Hue change under timing error | 0.001 deg | 0.00 | 1.29 deg per ns |
| Edge displacement per ns of timing | 1.001 | 0.999 | |
| $0F-$30 rise shortfall at 1 / 3 us | 21.2 / 4.1 IRE | 22.0 / 4.5 | none |
| $0F-$30 fall excess at 1 us | 13.5 IRE | 14.9 | none |
| White clip 200%: shortfall at 0.5 us | 16.0 IRE | 15.8 | |
| Dropout with compensator: output minus line above | +0.02 IRE | within 1 | |
| Dropout without compensator | +49.9 IRE | +54 | |
| 0.25 um defect, extra noise | 2.2 IRE RMS | 2.4 | |
| Head switch, 1 us steps: line before, switch line before and after x_switch, line after | 0 / 0 / 1000 / 1000 ns, no shear | | |
| Whole chain, flat grey, timing only: head A against head B luminance | 0.006%, worst band of rows 0.034% | | 1.2% |
| Whole chain, flat grey, deck defaults: whole-row share of the temporal variance of decoded Y, 2.3 / 8 / 64 line clamp | 17.8% / 6.4% / 2.8% | | 16% |

The noise is independent between lines (r = 0.007) and frames (r = -0.006),
with a flat per-column variance (5.5%) and no lattice at the earlier stage's
8.59- and 143-sample spacings.

On screen (1024x768, 60 frames of the same static fields as the review of the
earlier stage, then Super Mario Bros.), [see the before and after
values](#vhs-screen-measurements).

<a id="vhs-screen-measurements"></a>
| Measurement | Earlier stage | FM deck |
|---|---|---|
| Blue field, per-row colour against the clean run: ΔE76 RMS / max | 4.64 / 28.9 | 0.79 / 24.2 |
| Blue field, per-row a* correlation between frames at lag 1 / 5 | 0.97 / 0.68 | 0.14 / 0.15 |
| Super Mario Bros. sky, per-row colour against VHS off: ΔE76 RMS / max | 6.45 / 25.7 | 1.29 / 5.3 |
| Bars, edge displacement RMS | 6.8 ns | 14.4 ns |
| Bars, edge displacement correlation between frames | 0.58 | -0.13 |
| Bars, edge motion power below 4 Hz | 60% | 12% |
| Grey grain, RMS relative to the level | 3.7% | 1.2% |
| Grey grain, horizontal correlation 0.5 point | 337 ns | 185 ns |
| Grey grain, kurtosis / correlation between frames | 2.88 / 0.00 | 3.04 / 0.02 |
| Grey grain, share of variance in whole lines | 27% | 40% |

The rows with the largest ΔE76 are dropouts. The per-row a* pattern that
remains correlated (0.14-0.23 at every lag, higher at even lags) is the
per-head bow left in the APC residual, which repeats with each head. Edge
displacement is larger because the timing is now the measured transport
residual after the TV's line PLL (about 15 ns mid-field), with a new component
every field instead of a glide.

The whole-line share is a TV effect, not the deck's: at the deck output it
is 1.5% (`gpu_fidelity_tests`), 17.8% at the TV's decoded luma with the
default clamp, and it measured 40% on screen, where the beam spot smooths
the fine grain and leaves the whole-line offsets as they are. A slower
clamp lowers it (see the TV sync separator and black clamp section); the
default stays at the earlier rate until a TV's clamp is measured. The
on-screen table above was measured before the band calibration and the
same-deck skews; the whole-chain rows in the kernel table are the current
figures.

GPU time of the two kernels on an M5 (10-core GPU), measured interleaved
with the earlier kernel in the same run: the tape stage takes 0.73 ms and playback 0.12 ms (median;
0.88 ms together), against 1.93 ms for the earlier 129-tap stage. The complete
VHS chain benchmark (`--benchmark`, 60 frames after 12) measured 8.2 ms at
1280x960 and 11.2-11.4 ms at 1920x1440, 1.0-1.4 ms below the earlier stage in
the same session; at 2560x1920 both builds measured 16.5-16.7 ms. With the
one-sided noise shaping (four more complex sections in the tape stage), the
three-pole tape loss and the receiver's sync separator, the same benchmark
gives 6.3 ms at 1280x960, 8.6 ms at 1920x1440 and 11.9 ms at 2560x1920
(medians; the Sony PVM-14L2 preset 3.5, 5.2 and 7.5 ms and Bedroom RF 1990
5.2, 7.5 and 10.9 ms in the same run), on a display pass that has changed
since the earlier figures, so only the differences between presets compare.

Presets saved before this model have no `"model": 2` in their `vhs` block.
When such a block is enabled, the SP consumer defaults replace it, the TV
gets the 250 Hz line PLL if it had none, and one line goes to stderr:
`vhs: preset uses the pre-2 VHS model; loaded SP consumer defaults`. All deck
controls are under Signal chain > VHS recording / playback; the line PLL is
under Y/C separation.

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
