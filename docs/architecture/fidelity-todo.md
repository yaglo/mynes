# Fidelity to-do

The open work from the model-limits review of 23 September 2026 (a 27-agent
workflow that collected 693 stated limitations from the site, docs, presets
and code, assessed 11 model areas, challenged each, and ranked the
improvements). The full plan with its reasoning, sources and cost estimates is
outside the repository at `~/Projects/mynes-compare/2026-09-23-model-limits/plan.md` (with the full assessments beside it)
(the run record is `wf_de267029-54e` under the `mynes-build` session's
workflows). This file tracks status; keep it current when an item lands.

Codes from the plan: impact / confidence / effort, H high, M medium, L low,
S small. Status: `[x]` done on master, `[~]` partly, `[ ]` open, `[m]` waits
for a hardware measurement listed at the end.

## Ranked improvements

- [~] 1. H/H/S. Draw NES pixels at 8:7 and place the picture on the receiver's active raster. Done: the deflection map now uses BT.470 blanking and the console's timings, so dots are 8:7 and blanking shows at the sides until overscan hides it. Open: keep the 15+11 border dots in the active line; documented overscan per preset (JVC 5% H and 4% V per edge, Toshiba V 9±2%, PVM underscan 252×188 mm with about 13/10 mm backdrop borders). WP-F.
- [x] 2. H/H/S. Synchronous video detector per receiver on RF. Done: `rf.detector` (auto = synchronous, or envelope for a pre-1978 diode detector; Video detector in the RF menu) with `rf_if.comp.glsl` taking the in-phase component for a PLL VIF and the magnitude for a diode; `test_fidelity` shows the envelope detector's level shift and second harmonic on chroma over a dark grey and the synchronous detector's exact recovery. The VIF loop's own phase noise is invisible and not carried. WP-B.
- [~] 3. H/H/S. Console audio filter from the schematic. Done: NES-001 16.7 Hz high-pass and the two low-pass poles from `tools/circuits/nes001_audio.cir` with the 74HC04 gate as a MOS pair fitted to its data sheet (rainwarrior measured 16 Hz), the gate's rail window and supply gain, the 7805 ripple from `nes001_psu.cir`, the Famicom's 37 Hz mixing network, the 90 Hz and 440 Hz high-passes removed, `test_audio` pinned to the golden responses. Open: the Famicom's, NES-101's and Dendy's low-passes are generic; the jack level (2.0 V per unit) and the high end want the measurement in `measurement-plan.md` A1. WP-I.
- [~] 4. H/H/S. One keyed top-sync AGC loop across lines and frames. Done: `agc_loop.comp.glsl` walks the frame with one capacitor state carried across frames (charge proportional to the tip's excess with a slew limit, constant discharge; TDA8362's 2 ms and 52 dB per 25 ms as the presets' `agc_attack_ms` and `agc_release_ms`; the peak detector rides the sync window's noise sigma), `agc.comp.glsl` only applies the per-line gains; the burst APC and ACC loop blends 0.05 per line and never reacquires after retrace; the sync slicer keys its 50% level from the porch with a half-dot fine slice, which at a 15 dB carrier-to-noise ratio raised the lines with a found edge from 49 to 238 of 240 and kept the colour. Open: the clamp loop's constant is still the preset's `clamp_lines`; the AGC's gain range is a 0.5 to 2 limit rather than a set's; the M52342SP's constants. WP-B, WP-C.
- [~] 5. H/H/S. RF sound. Done: 75 µs (50 µs PAL) de-emphasis on RF presets (`audio_rf_deemphasis`, auto), FM hiss from the RF stage's carrier and noise levels with the sound carrier assumed 13 dB under vision, and the intercarrier buzz derived each frame from the picture's carrier envelope through the set's sound-detector AM rejection and the modulator's incidental phase modulation (`rf.sound_am_rejection_db`, `rf.icpm_deg`; `audio_chain_rf_buzz`). Open: the NES modulator's actual sound level, deviation and phase modulation, and a set's real AM rejection (measurements A4 and B3). WP-I.
- [ ] 6. H/H/M. Trap-type consumer front end (TDA8362 stand-in, labelled): causal Q=2 trap, 480 ns Y delay, 3-tap peaking with compression, chroma BPF folded into the I/Q FIRs; filter lengths from −3 dB targets. Replaces windowed-sinc luma and blended ringing; chroma requests of 0.25–0.5 MHz currently realise 0.57 MHz. +0.25–0.35 ms. WP-D2.
- [ ] 7. H/M/S. Per-chip demodulation axes and gains (M61283FP, E13169GM, TA7644BP); RGB input no longer takes Tint and Color. WP-D2.
- [m] 8. H/M/S. Choose the PVM comb law from photos of the maintainer's PVM (M1). The adaptive mode falls back to BPF on all NES luma detail. WP-D1.
- [~] 9. H (arcade)/H/S. RGB-PPU sources. Done: 2C03 palette feeds RGB presets. Open: the four 2C04 permutations and the 2C05. WP-A.
- [ ] 10. H (few games)/H/M. MMC5 ExRAM mode 1 and the $5200–$5202 vertical split; MMC5 audio (two pulses, envelope and length, PCM with IRQ, $5015); Sunsoft 5B audio. Test ROMs are in the repo. WP-J.
- [ ] 11. M/H/S. Hum and pickup from physical sources with true NES timing: remove console PSU hum into the beam, shield hum and pickup at the gun, hum on focus; the bar rolls at 240 rows per cycle; zero-mean. WP-E.
- [ ] 12. M/H/S. One physical noise origin on RF: FCC-capped modulator level, losses and noise figure give a CNR in the OSD; remove `tv.noise_level` and gun-level pickup on RF presets; hash the gun noise seed (it repeats every 2 frames). WP-B, WP-E, WP-P.
- [ ] 13. M/H/S. Millimetre and nanosecond units for spot, convergence and jitter; convergence basis in mm. Pixel units change with window size and render scale. WP-F.
- [ ] 14. M/H/M. Band-limited step resampler at exact fractional time; exact 3-input TND; remove the cubic warp and DMC crosstalk. Aliases sit only 30–46 dB down. WP-I.
- [ ] 15. M/H/S. Real horizontal timing: the odd-frame 340-dot line in the raster; PI AFC from the M61283FP filter; colour loop advancing by the true line length; PAL broad pulse of 320 dots. WP-A, WP-C.
- [ ] 16. M/M/M. Persistence: bypass when invisible; Kuhn-shaped P22 decay; per-channel BFI dark level. Saves 1.2–1.4 ms on 12 presets. WP-H.
- [ ] 17. L-M/M/M. Large-signal RGB output stage per gun: causal FIR plus to-black RC limit; retire `asym_rise_fall` (wrong sign), `vertical_smear`, `velocity_mod`. WP-E.
- [ ] 18. M/M/M. PVM ABL/EHT from the G and B boards (attack 0.44–11 ms, release about 0.23 s) using gun_current's current law. WP-E.
- [ ] 19. M/M/L. Physical faceplate glare PSF (TIR ring, electron term, far field over the raster); per-axis transmission falloff instead of the cos⁴ vignette. WP-G2.
- [ ] 20. M/H/S. Grille stripes at 1/6, 1/2, 5/6; mask tiles from physical dimensions; round dots; mask-fit viewport; life-size and cycles-per-degree report. WP-G1.
- [ ] 21. M/M-H/S. Measured primaries: ISETCam Sony P22 for the PVM; FW900 Table II.24-1. WP-G1, WP-K.
- [ ] 22. M/H/S. Room-light tint from CIE colour matching functions on the CPU (0.034 u'v' error at 3200 K now). WP-G2.
- [ ] 23. L/H/S. Cable: identity plus delay for leads ≤5 m; no baseband cable on RF; ghosts become IF multipath with carrier phase. WP-A, WP-B.
- [ ] 24. L/H/S. Packed half2 (I, I·σ²) vertical width; one growth law for both axes; Jacobian dwell (fixes the sign); reduced-resolution deflection map. Isolated highlights under-bloom by 19%. WP-F.
- [ ] 25. M/M/M. Toshiba Video Tone fit, black stretch and ABCL; PVM §2-17 black level; Commodore 1702 luma path from its schematic. WP-D2, WP-E, WP-K.
- [ ] 26. M/H/M. FW900: gain-offset-gamma tone with both BRIGHTNESS setups; RGB variant with OSSC 6 samples per dot; measured geometry; halation about 5.3%. WP-K.
- [x] 27. L-M/L/M. Console output stage from the schematic. Done: `console_follower_tau_ns` from Schenk's NES-001 and the EDC AV module trace, ngspice deck `tools/circuits/nes001_video_chain.cir`, golden file and test. Open: presets keep the 30 ns estimate and the 6 MHz pole until the follower is compared with a console on the PVM. WP-A.
- [~] 28. M/M-L/M. TV audio circuits. Done: PVM-14L2 (AN5278, 100 µF into the 7×5 cm speaker, 199 Hz) and Toshiba 14AF43 (AN5276, 1000 µF into 8 Ω, 20 Hz) output networks as speaker profiles; the AV follower simulated in the audio deck (0.05–0.21% THD at NES levels, so linear); hum derived from the supply circuit; mains pickup as a setting; the video stage's draw on the rail quantified by the PSU deck at 80 to 95 dB under a unit at the jack (not carried). Open: the drivers are class estimates; the set's own supply into its audio amplifier; the acoustic line whine (measurement A5); CPU audio chain by default. WP-I.
- [m] 29. M/M/M. MC141627 at its own 4fsc clock with burst re-referenced after the comb (alias hue error of 4–11°). Needs the M1 scope data. WP-D1.
- [~] 30. L/M/M. Console PSU fault model. Done for the sound: the preset's `psu` block (adaptor, reservoir, load, rejection) is run as its circuit with the 7805's dropout, so a sagging adaptor or a dried reservoir hums by itself (`audio_psu_derive`, held to `nes001_psu.cir`). Open: the same rail into the PPU's DAC ratiometrically and the video hum bar, fault presets only. WP-E.
- [ ] 31. M/H/S. 50 Hz display-mode matching for PAL; capture readback ring; readme.webp with every frame; 4:4:4 web clips. WP-H.
- [ ] 32. none/H/M. `_evidence` provenance per named preset with tests tied to sources. WP-K.
- [ ] 33. L/H/S. CPU composite path on the terminated rails plus a CPU-vs-GPU test; drop the ×0.746 emphasis table. WP-J.

## Work packages

**WP0, enabler (first, serial).** `[ ]` Split `video_gpu.c` into `stages/{console,rf,receiver,yc,decode,amp,beam,temporal,vhs}.c`; `TVDisplayParams` into `params_<area>.h`; `preset_apply.c`, `gpu_osd.c`, `signal_precompute.h`, `crt_color.h`, `crt_display.frag.glsl` and `gpu_display.c` per area; a per-line timing buffer, a per-line supply table, a separate RGB-input matrix, a configurable active width, a cartridge-audio hook, a per-line border buffer, a measured-displacement LUT input; per-stage GPU timing from Metal command buffers. Test: PFM captures of all presets bit-identical before and after.

**WP-A, console source, raster, cable.**
- [~] RGB-PPU palettes (2C03 done; 2C04, 2C05 open).
- [x] Output-stage circuit mode from the schematic (item 27).
- [ ] Remove the 6 MHz pole once the follower is the default.
- [ ] PPU revision selector: 2C02E 2.5°/row, 2C02G about 5°/row.
- [ ] Raster: PAL broad pulse 320 dots; odd-frame short line written into a timing buffer; per-line border from WP-J.
- [ ] Cable as identity plus delay; no baseband cable on RF; pickup injected in mV at the receiver.
- [ ] Module AC coupling (10 µF into about 2.2 kΩ) with the jack-follower DC curve, shipped disabled.
- [ ] iverilog BreakingNES HDL dumps of vidout and hv_decoder as golden per-dot level classes for NTSC and PAL.

**WP-B, RF link and front end.** `[~]` Detector type per receiver (item 2, done); keyed top-sync AGC across frames (item 4, done, with the porch-keyed sync slicer); link budget and CNR (item 12); multipath at complex IF (item 23). Reference decks `nes001_rf_modulator` and `famicom_rf_modulator` (reference only until an SDR capture exists). Test `test_rf_front.c`.

**WP-C, receiver sync and loops.** `[ ]` Remove the per-frame reacquire; PI AFC from the M61283FP pin-2 filter; colour loop on the real line length from WP-A's timing buffer; APC from the pin-36 filter; colour killer with hysteresis; keyed clamp charge; delete `rf_interference`. Reference `m61283fp_loops`. Test `test_receiver_loops.c`.

**WP-D1, Y/C separation.** `[ ]` Store-type uniform (fixed 2730, line-locked 2728, H-reset 910/909/909); remove the 0.65/0.85 blend defaults; rename "2line" to "adaptive" with a migration; `[m]` PVM law from the M1 photos; `[ ]` PVM in PAL with the MC141627 window; `[m]` 4fsc stage gated on M1 scope data. Reference `pvm14l2_fl107_candidates.cir`. Test `test_yc_geometry.c`.

**WP-D2, decoder filters, matrix, sharpness.** `[ ]` Deterministic filters from −3 dB targets; trap-type front end (item 6); demodulation axes per chip (item 7); RGB-input matrix with contrast, brightness and drive only; Toshiba Video Tone; OSD sharpness in register units. References `c1702_luma`, `pvm14l2_aperture_loop`. Test `test_decoder_response.c`.

**WP-E, amplifier, gun, supply, ABL, hum.** `[ ]` Causal output stage with to-black RC limit (item 17); gun law in cathode volts with beam-current feedback; video B+ rail; PVM ABL/EHT (item 18); Toshiba black stretch and ABCL; hum table (item 11); NES PSU fault model (item 30). References `pvm14l2_rgb_drive`, `jvc_gr2_crt_drive`, `pvm14l2_abl`, `nes001_psu`, `tv_rectifier`; retire `crt_video_recovery.cir`. Test `test_amp_circuit.c`.

**WP-F, beam, geometry, convergence.** `[~]` Aspect and raster placement (item 1, partly); `[ ]` mm and ns units (item 13); convergence basis from the PVM service figures; Jacobian dwell and half2 moments (item 24); deflection map at quarter resolution; delete hashed jitter, wobble, rf, edge-fade and overshoot; scan-velocity modulation for late_crt_wega only. Test `test_beam_geometry.c`.

**WP-G1, mask and phosphor colour.** `[ ]` Stripes at 1/6, 1/2, 5/6; tiles from physical dimensions; mask-fit viewport; life-size window; ISETCam primaries (item 21); grey tracking in cut-off and drive; corner gain fix; remove the thermal tint; landing model texture; subpixel self-test. Test `test_mask_colour.c`.

**WP-G2, glass, room, HDR output.** `[ ]` Faceplate PSF (item 19); far field by FFT; per-axis transmission falloff; CIE tint (item 22); room in lux with curved-glass normals; absolute anchoring (PVM 170 cd/m²); Hitachi regression to 0.3%. Test `test_glass_psf.c`.

**WP-H, persistence, presentation, capture.** `[ ]` Persistence bypass and Kuhn shape (item 16); per-channel BFI dark level; 50 Hz mode matching and VRR (item 31); capture ring; readme.webp; 4:4:4 clips. Rig deck `phosphor_charge_amp.cir`. Test `test_persistence.c`.

**WP-I, audio.** `[ ]` Console filter from the schematic (item 3); RF sound de-emphasis, FM noise and buzz (item 5); blip-style resampler and exact TND (item 14); Q4 follower nonlinearity and soft limiter; PVM and Toshiba speaker networks (item 28); HCU04 supply gain; Famicom cartridge routing; CPU audio chain by default with an adaptive queue and underruns in the OSD. References `nes001_audio`, `hvc001_audio`, `pvm14l2_audio_out`, `toshiba_14af43_audio`, `etrom_mixer`. Test `test_audio_circuit.c`.

**WP-J, emulator core.** `[ ]` MMC5 ExRAM mode 1 and split; MMC5 audio; Sunsoft 5B audio (item 10); per-dot border export; `composite.h` on the terminated tables (item 33).

**WP-K, calibration data, FW900, evidence.** `[ ]` FW900 primaries, tone, tracking, halation, geometry and an OSSC RGB variant (item 26); `_evidence` provenance and schema version (item 32); PVM §2-17 black-level test.

**WP-P, preset integration (last).** `[ ]` Run each package's migration; decide arcade 2C03, the persistence tail and the JVC triad count; famicom_kitchen carrier to 91.25 MHz (it is 95.75 MHz now); re-render gallery and showcase; update `docs/gpu-*.md`.

**WP-DOC, site corrections.** `[ ]` Blog 3 (Bisqwit ×0.746 shown as current), blog 4 (coupling capacitor, "RC transmission line", phosphor credited with the comb), blog 5 (180° per line, 21.5 MHz, PVM "3D comb"), blog 6 (superseded beam, soft clamp, noise, 60 Hz full-wave), blog 7 (10 µF into 10 kΩ, pitches and gammas), blog 8 (one shared table), missing prototype notes, hardware.md's unverified "LA76600M 2H CCD", the note that the RF/power module schematic exists, the showcase README's "15x12 (8:7)".

Dependencies: WP0 first, WP-P last; A→C (timing buffer), B→I (CNR), E→I (hum phase), J→A (border), J→I (cartridge audio), F→K (displacement LUT), G1→G2 (backscatter colour).

## Only hardware measurement settles

- [ ] M1. Maintainer's PVM-14L2: board revision (rear label, C board marking); comb law and store type from phone macros of 1-dot and 2-dot stripes, white bar on black, white text on $02, hue boundaries; FL107 response, 4fsc alias, DG/DP, VH strap and menu values with a scope on IC104 pin 3 and IC111 pins 15 and 8; primaries, per-gun EOTF, ABL and white with an i1Display Pro; phosphor decay with a BPW34 charge integrator; spot versus current and position, stripe fill, convergence and linearity with a RAW macro or USB microscope; aperture law and CXA1739S port impedance with a generator; H AFC gain and EHT breathing.
- [ ] NES-001 with its RF/AV module: part markings and in-circuit values that the trace gets wrong (bias node, C5, FC2, choke capacitor, jack capacitor, Mitsumi or Alps); PPU pin 21 and jack waveforms on full-width $x8/$xC rows and black and white screens with a ≥100 MS/s scope into 75 Ω (this decides the follower default); channel 3 IQ capture with an RTL-SDR or HackRF for RF level, AM/AM, AM/PM, sound deviation, buzz and drift; per-unit DAC curve, HCU04 THD, buzz gain and +5 V load current.
- [ ] PAL NES (2C07): differential phase and vertical-sync alignment.
- [ ] RGB PPU (2C03/2C05): DAC linearity per code.
- [ ] JVC and Toshiba (no units here): store type, axes, trap, tone law, whites and SAW response from owner photos, an EEPROM dump, a VNA and a colorimeter; the LA76600M and TC90A45P datasheets must be fetched by hand.
- [ ] Host MacBook panel: hold and BFI response, present timing, latency and SDR white in nits.

Never fixable: host display limits (sample-and-hold, about 2× EDR headroom, 60 Hz only on the M5 panel); chip logic beyond its response to NES-geometry signals; the generic presets that name no chassis; reference photos as absolute calibration.

## VHS

The plan's VHS section listed the limitations of the old baseband model; the FM tape deck model that replaced it (record AGC, pre-emphasis, FM luma, colour-under, per-line transport table, heads, dropouts, playback discriminator and canceller) landed with the vhs-model merge. Still open, from the validation record: adjacent-track chroma crosstalk beyond what the 1H comb cancels, luma crosstalk from the 0.5 H sync offset, the Hi-Fi audio beat, the tracking bar, LP and EP, ACC settling and a head DC offset; VHS audio (linear SP track or Hi-Fi FM); PAL VHS, S-VHS and Y/C; the NES's 227⅓-cycle lines against the colour-under phase rotation; VHS noise from FM playback physics rather than `tv.noise_level`; the persistence tail migration once WP-H lands.

## Added since the plan

Not in the plan but bearing on it: the RGB console encoder source (`encoder_rgb.comp`, standard sync and sine burst in the raster, `dots_per_line` in the signal format) with the identity and cross-colour tests, which give the receiver its first check against a standard-level NTSC signal; and the NES-001 output follower above. See `rgb-consoles.md`.
