# Receiver sharpening and interference audit

Reviewed 21 September 2026. **The library does not yet reproduce every named
monitor's complete circuitry.** Working controls, different preset values and
passing implementation tests are not evidence of matching those circuits.
This is the current implementation record and open worklist, not a completed
calibration claim.

## What is implemented and selectable

| Stage | Current choices | Remaining fidelity boundary |
|---|---|---|
| Y/C separation | Horizontal notch, two-line/1H, adaptive three-line, three-line/2H, bypass | Shared equivalent responses; no proprietary chip decision logic or motion-adaptive 3D comb. PAL uses horizontal separation and its delay line. |
| Luma bandwidth | Cutoff, FIR length/window, trap depth | Windowed FIR approximation; not measured amplitude/group delay per receiver. |
| Sharpness | Generic horizontal highpass shelf applied to recovered Y; 0 bypasses | No independent peaking frequency/Q, coring, limiter, asymmetric preshoot/overshoot, vertical detail or chip-specific control law. |
| Chroma | I/Q bandwidth, hue, gain, delay-line correction | No model-specific chroma transient improvement or measured decoder nonlinearity. |
| RGB amplifiers | Per-channel bandwidth, rise/fall approximation, smear | No full transistor/circuit model. The legacy `velocity_mod` adds a luminance derivative to voltage; it does not change beam velocity/dwell. The OSD now calls it Edge derivative. |
| Tube and supply | Spot width/growth, mask, convergence, recovery, regulation, decay, optics | Shared behavioral equations with estimated unit-dependent parameters. |

Sharpness now runs after the Y bandwidth/trap and after the chroma branch has
read its input. Previously, adding a highpass directly to the separation FIR
could reintroduce rejected chroma and out-of-band noise. The new cascade
preserves the receiver's rejection. It remains generic, and its normalized
0–1 scale is not a television's factory menu scale.

RGB/direct bypass receiver sharpening. The source is currently an ideal
voltage-derived separated-output modification, not a simulated NESRGB board.
S-Video/component retain a luma path; a particular model's input restrictions
still need to be checked independently.

## Hardware evidence

* **Sony PVM-14L2:** APERTURE is the user sharpness adjustment. Its MC141627
  separator includes adaptive enhancement functions beyond our shared comb.
  We have not reproduced its vertical enhancement/coring or exact aperture
  transfer. [Operation manual](https://consolemods.org/wiki/images/2/2a/Sony_PVM-20L2_PVM-14L2_PVM-9L3_PVM-9L2_Operation_Manual.pdf),
  [service manual](https://consolemods.org/wiki/images/f/fc/PVM-L2_Service_Manual.pdf).
* **Sony PVM-20M4U:** Sony specifies 0 to +6 dB aperture correction and says
  APERTURE has no effect on RGB. The exact frequency response and knob law
  are still needed; the published gain range alone does not specify them.
  [Sony operation manual, pages 6–7 and 16](https://pro.sony/s3/cms-static-content/operation-manual/3859663251.pdf).
* **Toshiba 14AF43:** Sharpness is present and velocity modulation is explicitly
  absent. The schematic separates the LA76600M comb from the M61283FP jungle
  IC. The latter's Video Tone register has a typical 2.5 MHz response of
  −2 dB at minimum and +10 dB at maximum relative to its centre setting.
  Those are IC test specifications, not a calibrated TV menu mapping. Its
  CTI register also does not prove that every set enables CTI.
  [Service manual](https://consolemods.org/wiki/images/f/f7/Toshiba_14AF43_Service_Manual.pdf),
  [Renesas/Mitsubishi IC datasheet](https://www.renesas.com/en/document/dst/m61283fp-rej03f0054-0100z).
* **JVC AV-27D201:** the GR2 service manual documents separate TV, external
  and component DETAIL settings, APA DL and PR/OVR settings. A single shared
  symmetric filter does not reproduce all of those adjustments. Service
  register values alone are insufficient to infer a filter transfer.
  [JVC service manual, page 17](https://cdn.sunthar.com/documents/shared/3615fded-76e7-4d48-b9c8-53d580242633-JVC_AV-32D201_32D501_SM_EN.pdf).
* **Commodore 1702:** no user sharpness knob is listed among the seven picture/
  sound controls. That does not mean the internal video path has no peaking;
  the service material includes aperture compensation and peaking components.
  The fixed path must be derived from the correct 1702 schematic revision,
  rather than treating the related 1701 circuit notes as identical hardware.
  [User manual](https://manuzoid.com/manuals/0OL5G-Commodore%201702%20User%20manual),
  [1701/1702 service manual](https://www.valoroso.it/file-share/documenti-manuali/Commodore-1701-1702-service-manual.pdf).

## All 21 looks: identity and next target

Specific targets below are research/implementation targets, **not a claim that
the existing JSON values are already calibrated to them**. Generic wear/room
looks remain generic, as allowed by the user. Filenames are retained for saved
setup compatibility.

| Preset file | Current route | Target/status |
|---|---|---|
| `arcade_cabinet` | RGB | Generic arcade tube; receiver sharpness bypassed. |
| `basement_tv` | RF | Generic worn receiver/tube; retain limited sharpness effect. |
| `bedroom_rf_1990` | RF | Generic period consumer receiver. |
| `commodore_1702` | Y/C | Target Commodore 1702 fixed video response; current quick Sharpness is an emulator override. |
| `dying_crt` | Composite | Generic aged consumer chassis; separate faults from sharpening. |
| `famicom_kitchen` | RF | Generic Japanese compact receiver. |
| `jvc_d_series_2000` | Composite | AV-27D201; implement GR2 detail response and input-specific controls. |
| `late_crt_wega` | Composite | Keep generic until a specific WEGA chassis and its SVM path are implemented. |
| `living_room_1988` | Composite | Generic period consumer receiver. |
| `nec_xm29_arcade` | RGB | Current large-monitor look remains generic; no fabricated NEC calibration. |
| `rca_colortrak_1986` | Composite | Current large shadow-mask look remains generic; no specific RCA circuit claimed. |
| `reference_composite` | Composite | Deliberately generic neutral receiver, not a commercial TV. |
| `retro_gaming_setup` | Y/C | Generic consumer aperture grille. |
| `sony_pvm_14l2` | Composite | PVM-14L2; exact separator/enhancer/aperture response remains open. |
| `sony_pvm_20m4u` | Y/C | Target PVM-20M4U aperture response and gain law; current fine-grille look remains nominal. |
| `stass_favourite` | RF | Generic personal worn slot-mask set; keep preferred condition independent of chassis. |
| `studio_pvm` | Y/C | Target PVM-14L2 Y/C input; current studio look remains nominal. |
| `toshiba_14af43` | Composite | 14AF43 / M61283FP; implement tone response; no SVM. |
| `vhs_sp_consumer` | Composite/VHS | Target 14AF43 display behind a separate generic tape path. |
| `vivid_living_room` | Composite | Generic personal consumer picture settings. |
| `warm_desktop_monitor` | Y/C | Target 1702 circuit with personal white balance/room settings; not a factory preset. |

## Basement TV check

The shipped profile has a 2.8 MHz luma corner, a 4 MHz RF channel corner,
4.2–4.8 MHz RGB amplifiers, horizontal beam sigma 5.5 signal samples, plus
convergence error and focus growth. Peaking cannot undo those later losses.
A 1024×960 real-GPU synthetic grayscale-edge capture at Sharpness 0 and 1
confirmed a nonzero but smaller final-light change than Reference composite.
This verifies that the control works; it is not a measured old-TV comparison.
After the ordering fix, the central grayscale patch's linear RGB RMS change
was 0.00357 for Basement TV and 0.01626 for Reference composite. These values
depend on this synthetic pattern and capture setup, not just the receiver.

The GPU regression increased a retained tone from 0.092996 to 0.104119 while
the rejected carrier stayed below 0.00001 and DC remained 0.4. Additional
live-update checks preserved filtered chroma on composite, line-combed,
separated Y/C and PAL routes. Build, signal-precompute, pipeline, fidelity
and preset-control checks passed.

## Squiggly lines: separate mechanisms

The [AtariAge thread supplied by the user](https://forums.atariage.com/topic/275680-nes-squiggly-lines/)
contains a first-hand report of repairing a similar console by replacing two
100 µF capacitors in its power/RF module. The original poster tried another
power supply without success. This supports investigating internal supply
filtering, but does not establish the waveform/frequency or diagnose every
NES edge artifact. The thread does not report a completed repair of the
original poster's console.

Current source paths are distinct:

1. **Composite edge structure:** `dac_2c02.comp.glsl` emits the palette DAC
   waveform at the PPU-derived pixel/line/frame carrier phase. The receiver's
   Y/C separation, `receiver_demod.comp.glsl`, chroma FIRs and matrix convert
   it back to RGB. Finite separation/bandwidth and the colour-edge phase can
   produce serration/crawl without any power fault. Reference composite sets
   `console_psu_hum`, `hum_bar_amplitude`, `h_jitter`, `v_jitter`,
   `rf_interference`, `scanline_wobble` and `noise_level` to zero. Its edge
   phase regression runs with noise pickup disabled and still changes between
   carrier phases, while repeated identical phases do not drift.
2. **Existing supply effects:** RF-path sinusoidal 50/60 Hz pickup, CRT
   brightness hum bars and generic focus/load effects. These are not a
   rectifier/reservoir-capacitor/regulator model. Console hum is currently
   injected electrically into RF, not the common composite source; the
   same parameter also feeds generic CRT effects. That stage ownership needs
   correction before claiming to reproduce a console PSU fault.
3. **Geometry faults:** `deflection.comp.glsl` adds optional per-line jitter,
   slow sway and decorative legacy interference/wobble. These are not proof
   of a power/RF-board failure, and are off in Reference composite.

Do not freeze the PPU colour phase or add a screen-space squiggle to imitate a
fault. Native composite edge structure, a defective console and irregular
host presentation need separate tests. Matching the linked photographs to a
specific mechanism still requires better reference evidence.

## Open worklist

- [x] Put existing TV picture controls, including Sharpness, together under Picture.
- [x] Apply sharpening after Y extraction without bypassing its trap/bandwidth;
  test DC, detail gain, carrier rejection, stopband and RGB bypass on the GPU.
- [x] Identify the misleading legacy velocity-modulation control as a voltage
  derivative rather than physical SVM.
- [ ] Implement separate, selectable circuit responses for the five specific
  targets above, with fixed versus user-adjustable stages and input bypasses.
  Include chip-specific peaking frequency/shape, gain law, softening, coring,
  limiting, preshoot/overshoot and vertical enhancement only where supported.
- [ ] Derive/measure 1702 fixed response, PVM aperture responses, JVC GR2 detail
  processing and Toshiba tone response. Record the schematic revision,
  component/IC source and which coefficients remain estimated.
- [ ] Check each receiver's clamp/AGC, burst PLL, Y/C decisions, decoder matrix,
  chroma transients, RGB amplifier, ABL and input-specific ordering. Add model
  choices where the topology differs; changing parameter values is insufficient.
- [ ] Add a distinct **console power/RF-board fault** model for the linked
  squiggly-lines investigation. Establish the failing component's function,
  ripple spectrum and coupling path from schematics/measurements; investigate
  rectifier harmonics and higher-frequency pickup separately. Couple upstream
  faults into both composite and RF where appropriate, including sync/burst
  response, without imposing the same fault on healthy sets or the TV supply.
- [ ] Validate edge serration separately with flat grey/colour fields, colour
  boundaries, multiburst and consecutive source phases. Compare RF/composite/
  separated Y/C, fault on/off, and actual display timestamps. A source artifact
  must not be confused with a slow presentation beat.
- [ ] Match measured beam, phosphor, optics and supply behavior per monitor.
  Record input, menu/service settings, unit condition and capture timing.

Completing these unchecked items is required before claiming model-specific
accuracy. Existing tests establish implementation properties, not that these
hardware tasks are finished.
