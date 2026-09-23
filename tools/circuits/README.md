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
[source and uncertainty record](https://yaglo.github.io/mynes-web/research/pvm-14l2/).

## NES-001 PPU-to-jack video path

```sh
python3 tools/circuits/sweep_nes001_video.py /tmp/nes001-video --golden tools/circuits/golden
```

`nes001_video_chain.cir` joins Schenk's motherboard follower (Q1 2SA937, R2
510 Ω, FC2, C5 330 pF) to the AV section of Electronix Corp.'s 1992 trace of
the RF/AV module (10 µF coupling, 330 Ω into a 5600 Ω bias node, a 3.3 µH
choke with a capacitor across it, Q2 2SC1740 follower with 560 Ω, 68 Ω to the
jack) into the TV's 75 Ω. The trace draws the bias node's lower resistor as
330 Ω, which biases Q2 off, so the sweep runs it from 2.2 kΩ to open; the
choke capacitor is run at 2 pF and 380 pF. Transistors are generic; the PPU
source resistance, bead and pin swing (1.5 V for the measured 788 mV at the
jack) are assumed.

One transient per case carries black, a one-pixel and an eight-pixel white
pulse, and forty cycles of the chroma square wave of palette rows $0x to $3x
at the NESdev terminated levels. At the jack the eight-pixel step rises
104–118 ns (10–90%) and falls 11–12 ns in every case: the PNP pulls the
emitter down through the transistor but can only let it rise as R2 charges
C5 towards +5 V, so brighter levels rise more slowly. From that alone the
rows' chroma comes out at 1.07, 1.00, 0.90 and 1.05 of row 1's amplitude,
rotated +5.8°, 0°, −4.0° and +3.9°, with the cycle mean pulled down by 3 to
11 per cent of white. Rows 0 to 2 fall about 5° per row, the NESdev 2C02G
estimate; row 3's small swing does not continue the trend in this deck.
The bias resistor and choke capacitor change these by under 0.02 and 0.5°.

The GPU console stage has this follower as `console_follower_tau_ns`
(`Output follower RC` in the Console menu): R2·C5 = 168 ns, headroom 2.0
swings, steps down at once. `gpu_fidelity_tests` runs it on the same rows
and pulse and holds it to the golden file within 0.05 in gain, 3° and 0.04
of white; it lands at 0.94 against 0.90 on row 2 and 93 ns against 108 ns
on the rise, the difference being the deck's device capacitances and source
resistance. Shipped presets leave it at 0 until it is compared with a
console on the PVM; the earlier 30 ns differential-phase estimate stays as
their default.

## NES-001 audio path

```sh
python3 tools/circuits/sweep_nes001_audio.py /tmp/nes001-audio --golden tools/circuits/golden
```

`nes001_audio.cir` is the NES-001 mixer and amplifier from Schenk's schematic
(AD1 through R4 100 Ω and R7 20 kΩ, AD2 through R3 100 Ω and R8 12 kΩ, AUX
through R9 20 kΩ, C23 1 µF into gate U9E of the 74HC04 with R6 47 kΩ and
C21 220 pF in the feedback, C20 220 pF at the output, FC1 39 µH and C4
0.01 µF) followed by the AV module's follower, 1 µF coupling and 68 µH choke
into the TV's line input (Electronix trace). The gate is a behavioural
inverter swept over open-loop gain 10–40 and output resistance 300 Ω–2 kΩ;
the follower's output resistance and the choke's shunt capacitors are
assumed; the TV input is swept over 10 kΩ, 47 kΩ and 1 MΩ.

The high-pass is 16–18 Hz at the board across the sweep, set by C23 against
the 20k/12k sources and the gate's summing node, which agrees with
rainwarrior's hardware sweep (about 16 Hz, nesdev thread 17745). A 10 kΩ TV
input adds a second pole and moves the jack's corner to 23–26 Hz. The
low-pass is two poles: R6 with C21 (15.4 kHz) and C4 on the gate's output
resistance (15.9 kHz at 1 kΩ, 8 kHz at 2 kΩ, 53 kHz at 300 Ω), so the
resistance of an unbuffered HC04 gate in linear use is the number a
measurement would settle. There is no 440 Hz network anywhere in the path;
the chain's old second high-pass had no circuit behind it and its slot now
carries the output-pin pole. `golden/nes001_audio.h` holds the a_ol=20,
r_out=1k, r_tv=47k response and `test_audio` holds the chain's coupling,
amplifier and TV-input stages to it within 0.5 dB up to 12 kHz.
