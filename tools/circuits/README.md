# NES motherboard video buffer

This is a reproducible **sensitivity experiment**, not a calibrated replacement
for MyNES's console output stage. Open `nes_output_buffer.kicad_sch` in KiCad;
run the matching external ngspice deck with:

```sh
brew install ngspice
python3 tools/circuits/sweep_nes_output.py /tmp/nes-buffer-sweep
```

The sweep writes operating-point logs, full AC curves and a JSON summary for
27 combinations of drive bias, downstream load and ferrite inductance. It uses
small-signal AC analysis about each bias; it does not measure large-signal
clipping, transient recovery or differential phase of an actual console.

## Known circuit and assumed devices

The topology and component identities come from N. Schenk's
[hardware-checked NES-001 schematic](https://github.com/schenkzoola/NES/tree/56e5018491fcc8e0cd591d16a97c8a7b1d723983/NES-001%20Console),
revision 1.0, June 5, 2020. PPU VOUT drives the base of PNP Q1 (2SA937), whose
collector is grounded and emitter is pulled to 5 V through R2 (510 Ω).
The emitter feeds VIDEO_OUT through ferrite FC2; C5 (330 pF) shunts VIDEO_OUT
to ground. The RF/power module is outside that schematic.

The SPICE transistor is explicitly generic: **it is not a 2SA937 device model**.
Its junction capacitances, gain, transit time, source resistance (100 Ω),
ferrite loss (10 Ω), inductance and load are assumptions. A ferrite's complex,
frequency-dependent impedance is not represented by a constant series R–L.
R101/R102/L101/R103 in the KiCad sheet identify assumed equivalents, not original
board reference designators. The matching SPICE deck calls them Rppu/Rfc/Lfc/Rload.
The KiCad sheet documents connectivity; run the `.cir` file for simulation.

## What the sweep establishes

With an assumed 100 nH bead and 1 kΩ load, changing drive bias from 1 V to 2 V
changes carrier phase from about −7.7° to −10.9°. Those are model outputs,
not NES measurements. They show why a fixed linear pole cannot describe all
possible operating points of a transistor buffer. Other assumed loads and bead
values change the result substantially.

A directly attached 75 Ω load puts this particular model in cutoff. That is a
useful boundary check: **this motherboard node is not the terminated RCA jack**.
Cutoff cases retain absolute gain but report no normalized response or phase;
normalizing negligible transfer would give a meaningless large positive gain.

MyNES's DAC table already uses published terminated output levels. Inserting
this partial buffer downstream of that table would double-count unknown parts
of the measured output path. Keep the renderer's existing empirical model until
the RF/power module, source boundary and device response are established. A
real replacement needs the complete output path and agreement with measured
levels, bandwidth and differential phase—not just a plausible SPICE curve.

## Attribution and license

`nes_output_buffer.kicad_sch` and `nes_output_buffer.cir` are adaptations of
N. Schenk's NES-001 schematic, licensed under
[Creative Commons Attribution-ShareAlike 4.0](https://creativecommons.org/licenses/by-sa/4.0/).
Changes: isolated output buffer, generic transistor, assumed source/load/ferrite,
new KiCad layout and executable SPICE analysis. These two adapted files remain
under CC BY-SA 4.0, separately from the emulator's source license. Symbol graphics
are from the [KiCad symbol library](https://gitlab.com/kicad/libraries/kicad-symbols),
used under its CC BY-SA 4.0 license with the library exception.

## CRT rail implementation check

`crt_video_recovery.cir` is an independent **generic equivalent circuit**, with
1 kΩ and 12 nF giving the shader's assumed 12 µs recovery. It is original work,
not a recovered service schematic. A 64-dot load pulse checks charge and decay:

```sh
(cd build && MYNES_CRT_MEASUREMENTS=/tmp/gpu-rail.csv bin/test_fidelity)
python3 tools/circuits/measure_crt_recovery.py /tmp/gpu-rail.csv
```

The 256 actual GPU state samples and interpolated ngspice transient agreed to
maximum error 1.86e-7 and RMS 7.95e-8 normalized volts in the 2026-09-21 run.
The test deliberately removes rail feedback to isolate the RC equation. It
validates the assumed filter, not the capacitance, regulation, EHT behavior or
recovery time of any Sony, JVC or Toshiba set.

## PVM-14L2 aperture input branch

```sh
python3 tools/circuits/sweep_pvm_aperture.py /tmp/pvm-aperture
```

`pvm14l2_aperture_input.cir` transcribes the passive TP106-to-IC231-pin-8
branch of Sony's B(2/5) schematic, with reference designators and values.
Five assumed port resistances produce CSV frequency responses and a JSON
summary. An independent complex-impedance divider checks every SPICE point.
The CXA1739S active transfer and output injection branch are absent. Neither
the ideal LC resonance nor a peak in these partial curves is the television's
aperture peak. Nothing in this sweep changes renderer coefficients. See the
[source and uncertainty record](../../docs/pvm-14l2-model.md).
