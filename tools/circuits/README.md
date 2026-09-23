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
their default. `check_follower_model.py golden/nes001_video.json` runs the
shader's follower law in Python over the same rows and prints it next to the
deck for a few constants, so the constants can be tried without a GPU.
`fit_encoder_axes.py` is the least-squares fit of the RGB encoder's chroma
axes from decoded colour bars that set the 138°/48° axis phases in
`encoder_rgb.comp.glsl`.

## NES-001 audio path

```sh
python3 tools/circuits/sweep_nes001_audio.py /tmp/nes001-audio --golden tools/circuits/golden
```

`nes001_audio.cir` is the NES-001 mixer and amplifier from Schenk's schematic
(AD1 through R4 100 Ω and R7 20 kΩ, AD2 through R3 100 Ω and R8 12 kΩ, AUX
through R9 20 kΩ, C23 1 µF into gate U9E of the 74HC04 with R6 47 kΩ and
C21 220 pF in the feedback, C20 220 pF at the output, FC1 39 µH and C4
0.01 µF) followed by the AV module's follower (Q4 with its 7500 Ω and
3900 Ω loads), 1 µF coupling and 68 µH choke into the TV's line input
(Electronix trace). The gate is a level-1 NMOS/PMOS pair fitted to the
Nexperia 74HCU04 data sheet: forward transconductance about 35 mA/V at 5 V
(Fig. 12), open-loop gain 20, VOL 0.15 V at 4 mA, so the output resistance
in linear use follows as Gol/gm ≈ 570 Ω instead of being assumed. The
follower's transistor and the choke's shunt capacitors are generic or
assumed; the TV input is swept over 10 kΩ, 47 kΩ and 1 MΩ.

Results: the high-pass is 16.7 Hz at the board (17.4 Hz at the jack into
47 kΩ, 25 Hz into 10 kΩ), set by C23 against the 20k/12k sources and the
gate's summing node, which agrees with rainwarrior's hardware sweep (about
16 Hz, nesdev thread 17745). The low-pass is 17.1 kHz at the jack from two
poles, R6·C21 at 15.4 kHz and C4 on the gate's closed-loop output
resistance; the chain's first-order stages match the deck within 0.06 dB to
12 kHz with the second pole at 21.5 kHz. Gain is +4.8 dB from the pulse pin
and +9.2 dB from the TND pin; with the APU's 0.3 V pin swing (uXe and
lidnariq, nesdev thread 56) the jack sees 0.52 V and 0.87 V peak, which is
where `AUDIO_JACK_VOLTS_PER_UNIT` (2.0 V) comes from. Distortion at those
levels is 0.05 to 0.21% THD, so the gate is linear until its rails
(VCC − 2 V, 3 V peak to peak), which the chain carries as the gate rail
window. The gate's supply-to-jack gain is +9 dB (2.8 V/V), the number the
PSU deck's ripple is multiplied by. There is no 440 Hz network anywhere in
the path; the chain's former second high-pass had no circuit behind it and
its slot now carries the output-pin pole.

## NES-001 power supply

```sh
python3 tools/circuits/sweep_nes001_psu.py /tmp/nes001-psu --golden tools/circuits/golden
```

`nes001_psu.cir` is the Electronix trace's supply: the NES-002's 9 VAC into
a bridge, 2200 µF, then a 7805 (behavioural, with the TI data sheet's 62 dB
minimum and about 73 dB typical ripple rejection at 120 Hz and 10 mΩ of
load regulation) into 100 µF, with the console as a 0.4 to 0.8 A load and
the modulator as 40 mA on the raw rail. The transformer's source
resistance (0.8 Ω) and the load current are assumptions; the trace labels
the raw rail +13 V, which needs an unloaded adaptor above 9 V. At 0.6 A the
reservoir shows 3.7 V peak to peak of ripple and the +5 V rail 0.16 mV peak
at 120 Hz (73 dB) to 0.57 mV (62 dB), with the 240 Hz component a fifth of
that. Through the gate's +9 dB the jack carries 0.45 to 1.6 mV peak of
120 Hz hum, 58 to 70 dB below full scale: the NES-001 presets' ripple
setting (0.00023 units) is this, not an audible authored hum. Below about
7 V on the raw rail the 7805 drops out and the ripple passes; that case is
not in the presets.

## Monitor speakers

The PVM-14L2's audio board (service manual G 4/4): line input through
C3511 10 µF and R3512 220 Ω, C3501 10 µF into the AN5278's input with
R3506 6.2 kΩ and C3507 0.047 µF on its LT pin (the data sheet's flat
configuration is 6.2 kΩ and 0.01 µF; the larger capacitor eases the treble
cut), gain 30 dB, TONE pin at 2.1 V (near the data sheet's flat 2.5 V),
output through C3510 100 µF to the 7×5 cm speaker (part 1-544-063-12). At
8 Ω that capacitor is a 199 Hz high-pass, the dominant feature of the
PVM's sound. The Toshiba 14AF43 (service manual G-9): AN5891 tone control
into the AN5276 (34 dB) with 6.8 kΩ and 3.9 nF at its inputs, outputs
through 1000 µF to 8 Ω 5 W speakers, a 20 Hz corner. Both drivers remain
class estimates; the amplifier output networks are in `speaker_presets[]`
in `audio_chain.c` and `test_audio` checks the 199 Hz corner.

To hear the whole account, `tools/audio/record_presets.sh rom outdir` records
a ROM through the frontend with every preset that has a distinct audio path
(the two measured monitors, the RF sets, the Famicom, the arcade monitor) and
prints each recording's mean level and its level below 120 Hz, where the hum
and the speaker capacitors show.

## Sources not in the repository

The vendor documents behind the decks are copyrighted and are cited, not
committed: the Nexperia 74HCU04 data sheet (forward transconductance and
open-loop gain, figures 12 and 13), the Panasonic AN5276 and AN5278 data
sheets, the Sony PVM-14L2 service manual (audio board, sheet G 4/4), the
Toshiba 14AF43 service manual (sheet G-9), the TI LM7805 data sheet (ripple
rejection), and Electronix Corp.'s 1992 trace of the NES-001 RF/AV module.
Schenk's NES-001 and HVC-001 schematics are CC BY-SA 4.0 and are linked
above rather than copied.
