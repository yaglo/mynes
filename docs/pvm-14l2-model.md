# Sony PVM-14L2: circuit evidence and implementation

Audited 21 September 2026, without photographs or electrical measurements of
Stas's unit. This is a **nominal model constrained by published specifications**,
not a calibrated digital twin. The supported preset is `sony_pvm_14l2.json`;
other grille looks are not silently converted into this chassis.

## Sources and revision boundary

* [Sony L2 brochure, MK10009V1IW02NOV, specification table](https://www.adcom.it/public/images/pdf/pvm-14l2%20demo.pdf).
* [Sony PVM-14L2/20L2 service manual, 2nd edition](https://archive.org/download/trinitron_pvm14l2/trinitron_pvm14l2.pdf).
  The cover lists SY serials 2100001–2199999, AUS 2600001–2699999 and CH
  6100001–6199999. Stas's serial/revision has not been established. References
  below identify the inspected circuit, rather than asserting every production
  revision is identical.
* [Motorola MC141627FT datasheet](https://pdf.dzsc.com/27F/MC141627FT_1084067.pdf),
  pin descriptions, block diagram and VH truth table. An original manufacturer
  document hosted by a distributor; the internal comb algorithm is not published.
* [Sony Japanese specifications](https://www.sony.jp/products/catalog/SPC_PVM-20N6J.PDF),
  the L2 table, separate from the N-series table in the same document.

## Signal routing recovered from the service manual

Printed pages 5-1/5-2 and B(1/5), B(2/5), printed 9-5/9-6:

1. Composite passes through CXA2163AQ IC104's selection path to MC141627FT
   IC111. Its Y/C outputs return through FL108/109 to IC104 for decoding.
2. Y/C input bypasses the composite separator and enters IC104's decode path.
3. Component bypasses composite/chroma decoding and supplies Y/Cb/Cr to
   CXA1739S IC231. Component **does reach its Y/aperture path**.
4. RGB is buffered/clamped and switched with OSD into IC231's separate RGB
   inputs, pins 10–12. It bypasses the external Y aperture network.

MyNES follows the composite/Y-C/RGB distinctions, but its ideal component
shortcut currently forms RGB before the CRT stages, bypassing receiver Y
filtering/aperture. This remains an explicit gap. RGB/component cable losses,
input clamping and the exact IC matrix are also not circuit models.

## Changes supported by specifications

| Mechanism | Preset/control | Implementation and boundary |
|---|---|---|
| Horizontal AFC | `h_afc_tau_ms = 1` | First-order phase tracking at the actual line period. Independent of colour-burst acquisition; holds phase through missing sync and vertical retrace. Not a full oscillator/phase-detector circuit or frequency capture model. |
| Aperture range | `aperture_max_db = 6` | Existing Sharpness 0–1 maps linearly to 0–6 dB **peak gain of the aperture stage alone**. Zero bypasses. The dB-linear knob law and highpass-shelf frequency shape are assumptions. Downstream/upstream losses still determine visible sharpness. |
| RGB bandwidth | `rgb_bandwidth_3db = 1`, R/G/B = 10 MHz | Fit the FIR amplitude to -3 dB at 10 MHz. Previously the windowed-sinc cutoff was placed there, giving approximately -6 dB. Other frequencies and group delay are still FIR approximations; 10 MHz is not a guaranteed end-to-end optical response. |

Zero/missing new fields preserve legacy presets. The new settings save/load and
update live. Everyday **Picture → Sharpness** remains the aperture control;
advanced Video menus expose aperture maximum gain, AFC time and bandwidth
interpretation. No new per-frame pass or GPU readback is needed.

AFC uses `alpha = 1 - exp(-line_period / tau)`. It updates the horizontal state
only when horizontal sync is valid. Burst/black-level loops retain their existing
generic laws. The time constant changes signal tracking, not emulation speed,
presentation cadence or the NES carrier phase sequence.

## Aperture network: what the schematic actually establishes

B(2/5), IC231 CXA1739S:

* Y enters TP106/JL142 after R583 (100 ohm). The main branch uses R248
  (1.2 kohm), then C240 (47 nF) into pin 3, Y.
* A **separate feed from TP106**, not from SHP OUT, passes through
  R247 (1.8 kohm) in parallel with C234 (39 pF). R249 (3.9 kohm) shunts the
  following node. L230 (15 uH), C239 (20 pF) and R255 (zero-ohm link) lead
  to pin 8, SHP IN.
* Pin 5, SHP OUT, has R256 (4.7 kohm) and C201 (220 pF) to ground. C238
  (39 pF) couples that output back into the main Y node before C240.
* Pin 6, SHP SET, receives the external aperture control voltage.

This is stronger evidence than identifying an IC with a sharpening feature.
It still does not supply the active input/output impedances, polarity, internal
transfer, gain-vs-control law, limiting or tolerances of the complete circuit.
The renderer therefore retains an explicitly approximate shelf; the 6 dB
constraint is not described as reproducing this circuit.

The [partial SPICE deck](../tools/circuits/pvm14l2_aperture_input.cir) and
[sweep](../tools/circuits/sweep_pvm_aperture.py) isolate TP106-to-SHP-IN with
five hypothetical resistive loads. The ideal L/C resonance is 9.189 MHz, while
sampled maxima vary from 11.80 MHz to the sweep's 30 MHz upper boundary as the
load changes from 1 to 100 kohm. **Neither is the monitor's aperture peak.**
This experiment demonstrates why assigning 9.189 MHz as a calibrated sharpening
frequency would be unjustified. The sweep never imports its curves into a preset.

## Comb-chip vertical enhancement is disabled

B(1/5) connects IC111 VH pin 42 **directly to +5 V**, on the same rail as
DVCC pin 44. R200 (10 kohm) connects that rail to Q130's collector and
PAL/NTSC pin 47: it is the mode-select pull-up, not a link that makes VH
follow PAL/NTSC. Motorola's truth table gives VH high = enhancement off.
Consequently the traced revision disables the vertical enhancer in **both
NTSC and PAL**. This is a schematic inference, not a probe measurement.
C7/C3/D7 are strapped high; the remaining shown C/D configuration pins are
low. MODE0/1 and BYPASS are low in normal operation.

Do not implement an always-on vertical sharpener merely because MC141627 has
one. MyNES's shared adaptive NTSC comb is not its proprietary correlation logic;
the PAL horizontal separator/delay-line model is also not its PAL comb.
ADC/DAC quantization at the chip's 4fsc clock is not currently reproduced.

## ABL, focus and tube mechanics: keep the unknowns explicit

The manual's page 5-2 describes Q239 **PIC ABL** pulling IC231 pin 46 and Q235
**BRT ABL** pulling pin 7, with different reference thresholds and Q242 switching
references for aspect ratio. These are two control paths, not a single arbitrary
brightness multiplier. The ABL input voltage versus cathode current and the IC's
control laws have not been established; generic rail sag is not relabelled as
this ABL circuit. Nominal PVM sag remains disabled. Protection trip values must
not be used as normal ABL activation thresholds.

Page 5-3 explicitly labels the Q501/Q502/Q503/T501 focus output circuit
**PVM-20L2 only**. It must not be copied into a 14L2 simulation. The 14L2 focus
adjustment and the existing estimated edge-focus/spot-growth behavior do not
constitute a recovered dynamic-focus transfer function.

The existing grille count (267.5 mm / 0.25 mm = 1070), active aspect and D65
white point have published bases. 600 TVL describes horizontal resolution
normalized to picture height; it does not define 600 stripes or scanline width.
The existing spot FWHM, current-dependent growth, convergence, glass scattering,
phosphor primaries and decay remain estimates. P22 identifies a phosphor family,
not unique decay curves or spectra. Beam-current white-balance feedback is
also distinct from brightness limiting and EHT regulation.

## Verification and reproduction

```sh
cmake --build build --target mynes_gpu test_signal_precompute test_presets test_preset_json test_pipeline test_fidelity -j 8
ctest --test-dir build --output-on-failure -R '^gpu_(signal_precompute_tests|preset_tests|preset_control_audit|preset_json_tests|pipeline_test|fidelity_tests)$'
python3 tools/circuits/sweep_pvm_aperture.py /tmp/pvm-aperture
```

On the development Mac's Metal backend, all six selected suites passed:

* Horizontal phase-step response follows the 1 ms exponential at NTSC and PAL
  sample rates, survives consecutive dispatches, holds during vertical/invalid
  sync, and recovers without a burst-acquisition horizontal snap. A live
  parameter update checks the host-to-shader time-constant conversion.
* Aperture spectra swept at both sample rates match requested 0/1.5/3/4.5/6 dB
  peaks within 0.0001 dB, preserving DC and symmetry. Actual GPU tests retain
  the luma trap, preserve chroma routing and bypass sharpness for RGB.
* Actual GPU RGB impulse responses measure **-3.000001 dB at 10 MHz** on each
  channel. CPU sweeps verify the -3 dB definition over 2–10 MHz at both clocks.
* Preset JSON round trips and the control audit cover the new settings.
* SPICE and an independent complex-impedance calculation agree within
  1.4e-15 absolute complex voltage gain for all five passive-network cases.

These tests establish implementation behavior. They do not validate an unseen
physical tube. Remaining measurements for a calibrated unit are aperture
multiburst/edge response versus control, decoded colour transfer, spot profiles
versus current and position, ABL/EHT load steps, grille/geometry alignment and
phosphor temporal/spectral response. The proprietary comb decision logic also
needs characterization or a sufficiently detailed implementation reference.
