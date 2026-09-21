# CRT preset audit

The curated, visually polished group is **Sony PVM-14L2, JVC D-Series, Toshiba 14AF, and Stas's Favourite**, listed first in the preset menu. The other profiles remain available and have had their settings audited, but are not equally researched commercial-model matches. See [hardware evidence](gpu-hardware-research.md) and [current captures](gpu-visual-review.md).


The library contains 18 profiles. Sony, JVC and Toshiba have published hardware references; Stas's Favourite is a generic worn consumer set. The other profiles remain compatibility and exploratory presets. Beam widths, phosphor spectra, regulation and individual tube condition are estimates, not measurements of 18 televisions.

## Physical identities

| Preset | Source | Face; triads across | Dark → white FWHM, lines | Horizontal sigma, samples | Regulation / condition |
|---|---|---|---|---|---|
| Arcade Cabinet | rgb | shadow; 440 | 0.55 → 1.18 | 3 | size response 0.035; rail load 0.025 |
| Basement TV | rf | shadow; 340 | 0.78 → 1.62 | 5.5 | size response 0.07; rail load 0.1 |
| Bedroom RF 1990 | rf | shadow; 410 | 0.62 → 1.32 | 4 | size response 0.12; rail load 0.075 |
| Compact video monitor | svideo | shadow; 520 | 0.54 → 1.02 | 4.2 | regulated |
| Dying CRT | composite | shadow; 350 | 0.76 → 1.68 | 6 | size response 0.14; rail load 0.12 |
| Famicom Kitchen | rf | shadow; 380 | 0.6 → 1.3 | 3.5 | size response 0.1; rail load 0.06 |
| JVC D-Series (nominal) | composite / two-line | slot; 661 | 0.46 → 0.98 | 3.8 | modest supply response |
| Late consumer aperture grille | composite | aperture_grille; 680 | 0.46 → 1.05 | 2.8 | regulated |
| Living Room 1988 | composite | shadow; 440 | 0.58 → 1.22 | 3.5 | size response 0.1; rail load 0.06 |
| Large RGB monitor | rgb | shadow; 760 | 0.38 → 0.75 | 2 | regulated |
| Large consumer shadow mask | composite | shadow; 400 | 0.62 → 1.35 | 5 | size response 0.04; rail load 0.035 |
| Reference composite | composite | aperture_grille; 650 | 0.38 → 0.78 | 1.5 | regulated |
| Retro Gaming Setup | svideo | aperture_grille; 620 | 0.44 → 0.88 | 2.5 | regulated |
| Sony PVM-14L2 (nominal) | composite | aperture_grille; 1070 | 0.38 → 0.90 | 2.5 | regulated |
| Fine aperture grille | svideo | aperture_grille; 1200 | 0.28 → 0.52 | 1.6 | regulated |
| Stas's Favourite | rf | slot; 440 | 0.55 → 1.42 | 2.5 | size response 0.12; rail load 0.08 |
| Studio aperture grille | svideo | aperture_grille; 900 | 0.32 → 0.6 | 1.5 | regulated |
| Toshiba 14AF (nominal) | composite / three-line | slot; 480 estimated | 0.60 → 1.20 | 4.2 | modest supply response |

## Changes made after inspecting renders

- **RGB routing:** Arcade Cabinet and Large RGB Monitor previously bypassed beam, focus and geometry. Both now use the CRT stages. Their source is ideal RGB derived from voltage cycles, not an emulation of a particular NESRGB board or an arcade RGB PPU's palette ROM.
- **Beam identity:** replaced the three overlapping sharpness/height controls with explicit FWHM. Studio, Fine Grille, PVM, consumer grille, arcade and household sets have different spot growth and face pitch. A regulated gun's spot width no longer follows the other two guns' currents.
- **Masks:** Inline slot masks keep RGB columns vertical, with staggered bridges between adjacent triads. Shadow dots have a triangular RGB arrangement. Stas now uses the finer inline slot pattern: the previous coarse delta-dot weave dominated the boss image. Linear half-float coverage tiles and mipmaps replace per-pixel procedural supersampling. Half-float matters: eight-bit mip rounding introduced unequal primary gains. Fine grilles retain their resolved fundamental without inventing wider stripes.
- **Highlights:** hard channel clipping erased bright mask detail. Output now has an RGB-ratio-preserving shoulder tied to actual host headroom, after linear CRT light generation. This is an explicit host-display compromise, not claimed CRT saturation.
- **Dying CRT:** removed keystone, rotation, shear and the decorative sinusoidal wobble. Reduced the compounded hum, noise and focus defects. It now describes weak blue emission, soft corners, DC-restoration recovery and weak regulation in an otherwise stable raster. Overscan's inverse mapping was reversed; it now enlarges/crops the picture against a fixed tube aperture.
- **Wear:** Basement and Kitchen no longer stack strong artificial texture over RF snow. Receiver noise enters before gamma/beam spread and is tied to scan time. Stas's Favourite keeps causal horizontal bright/dark recovery, load-dependent contraction and imperfect convergence.
- **Source/cable:** removed the accidental 14 kHz audio value from video bandwidth. Short leads no longer impose an invented 7 MHz blur. Unjustified long ghosts from short terminated leads were removed; Basement retains a small damaged-connection reflection. The unimplemented legacy coupling capacitor is no longer offered as a working control.
- **Colour/glass:** removed double white-point/gun-tint corrections from clean profiles and arbitrary green halation tints. Clean studio profiles use a common nominal 2.4 gamma, so their identity comes from focus and phosphor geometry. The worn blue gun is explicit, not hidden in a palette.
- **Audio settings:** retained distinct generic speaker families. Cable capacitance/resistance now uses the actual preset cable values. Reduced extreme noise, hum and amplifier drive in the worn profiles. CPU/GPU processing is validated separately; these speaker responses are not hardware measurements.

## Setting-by-setting disposition

The following covers every field present in the shipped JSON files. Zero-valued service/diagnostic controls are intentional: normal operation should not include an unrelated collection of defects. The values in the preset files are the authoritative editable settings.

| Settings | Interpretation and review decision |
|---|---|
| `name`, `description` | State generic identity and avoid commercial-model calibration claims. |
| `connection`, `comb_type`, `comb_notch_depth`, `region` | Composite/RF retain modulated colour; Y/C and RGB are ideal modifications. Sony uses adaptive composite separation, JVC two-line and Toshiba three-line; older profiles retain their notch/separated-source choices. Commercial IC transfer functions remain approximate. Live ROM region takes precedence over a preset. |
| `console_variant`, `speaker_type` | Select nominal audio filters and generic speaker families; not a change to the emulated PPU or CPU. |
| `console_amp_bw`, `console_coupling_R`, `console_psu_hum`, `console_phase_distortion_ns` | Generic output pole/source impedance and supply pickup; NTSC profiles use the published 30 ns 2C02G phase-distortion estimate. 6 MHz is a nominal equivalent pole, not a measured chip-wide specification. The imported `console_coupling_C` field is inactive and omitted from shipped profiles. |
| `video_cable.length_meters`, `resistance_per_m`, `capacitance_per_m`, `connector_resistance`, `impedance` | Passive equivalent shunt-capacitance response. Does not claim skin/dielectric loss or full transmission-line propagation. Nominal 75 Ω termination and plausible short-lead values. |
| `video_cable.shield_effectiveness`, `ghost_delay`, `ghost_level` | Generic pickup strength and optional echo in signal samples. No calibrated shielding-to-noise transfer. Only the damaged RF lead has a small explicit echo. |
| `video_cable.num_sections`, `audio_cable.num_sections` | Legacy metadata; GPU video uses one equivalent pole, not a distributed ladder. Not exposed as a working GPU control. |
| `rf.enabled`, `mod_bandwidth`, `agc_attack_ms`, `agc_release_ms` | Enable the baseband RF model, its FIR and sync-keyed gain dynamics. Bandwidth now updates on preset load. No AM/VSB tuner simulation. |
| `rf.carrier_freq`, `carrier_level_dbm`, `noise_floor_dbm` | Frequency is metadata. Carrier/noise power difference sets complex Gaussian noise before an equivalent negative-AM envelope detector; total noise is defined at that injection point. This is not a measured tuner noise figure. |
| `brightness`, `contrast`, `chroma_gain` | Receiver voltage-domain controls. Black and white checks use neutral baselines; only worn preferences retain deliberate black/contrast changes. |
| `tv.luma_bandwidth`, `chroma_bandwidth`, `fir_ringing`, `luma_peaking`, `luma_notch_depth` | Receiver separation and frequency response. RF/household chroma is narrower than clean Y/C. Ringing/peaking is modest outside deliberately worn receivers. |
| `tv.hue_offset`, `saturation`, `color_temperature`, `r_drive/g_drive/b_drive`, `r_cutoff/g_cutoff/b_cutoff`, `color_killer` | Decoder adjustment, nominal white point, per-gun balance and burst gate. Clean monitors stay neutral; small household tint errors are explicit. |
| `tv.r_bandwidth/g_bandwidth/b_bandwidth`, `gamma` | Gun-voltage bandwidth followed by current transfer. Generic equivalent responses; PVM-14L2 uses Sony's 10 MHz RGB figure. |
| `tv.beam_fwhm_min`, `beam_fwhm_max`, `beam_spot_size`, `bloom_gamma`, `edge_focus`, `velocity_dim` | Spot dimensions, current growth and edge behaviour. FWHM is in scanlines; horizontal sigma remains in signal samples. Bright consumer spots can fill the gaps; focused monitors retain more separation. Gaussian spots omit measured non-Gaussian high-current tails. |
| `tv.convergence_static`, `convergence_dynamic`, `conv_r_x/conv_b_x`, `conv_r_y/conv_b_y` | Small central/edge gun landing errors, larger for worn tubes. Legacy horizontal offsets use signal samples; vertical/generic offsets use drawable pixels, so this part is not a physical millimetre calibration. Nominal PVM is aligned. |
| `tv.beam_current_load`, `video_black_droop`, `video_recovery_us`, `hv_sag`, `focus_breathing` | Causal video-rail/DC restoration and shared supply response. Positive size response contracts the raster; negative expands it. Generic couplings/time constants, not a circuit-component fit. |
| `tv.barrel`, `barrel_v`, `overscan`, `keystone`, `rotation`, `skew_x/skew_y`, `h_pos/v_pos`, `h_size/v_size` | Tube/raster geometry. Household overscan is modest. No preset needs a tilted, sheared trapezoid. Dying CRT is now stable. |
| `tv.h_jitter`, `v_jitter`, `rf_interference`, `geometry_warp`, `scanline_wobble`, `hum_bar_amplitude` | Small explicit timebase/supply faults for worn sets. Decorative quantized RF displacement and sinusoidal line wobble are off throughout the library. |
| `tv.beam_edge_fade`, `beam_edge_overshoot`, `burst_lock_drift`, `burst_lock_drift_width` | Legacy source/edge diagnostics; zero in all shipped presets. They are not required to give a CRT its identity. |
| `tv.mask_type`, `mask_triads`, `mask_pitch_px`, `mask_strength`, `subpixel_layout` | Physical family and total pitch, full mask coverage, host filtering. Host Panel-pixels mode fits integer periods to the current game viewport; CRT-pitch mode preserves nominal density. RGB/BGR ordering is not LCD-subpixel calibration. |
| `tv.persistence_ms`, `persistence_r/g/b`, `motion_threshold` | Generic frame-sampled decay; explicit smoothing is separate. Not a reproduction of a CRT's moving impulse beam on a 60 Hz sample-and-hold panel. Age does not automatically imply huge phosphor trails. |
| `tv.halation`, `halation_tint_r/g/b`, `glass_tint`, `vignette`, `ambient_light`, `black_floor` | Moderate optical spread, neutral scatter colour, glass throughput and room/black level. No arbitrary green glow. Ambient extends into window margins. |
| `tv.noise_level` | Receiver output voltage noise, before gun transfer/beam spread. RF snow is separately introduced before decoding. |
| `tv.hdr_gain` | Linear exposure before host adaptation. Does not claim measured nits; actual HDR headroom comes from SDL. |
| `audio_cable.*`, `audio_cable_length_m` | Source resistance plus wire resistance, cable capacitance and length affect the audio pole. Shielding, echo, impedance and ladder sections are compatibility metadata for audio; noise/hum are explicit separate controls. 8 Ω speaker load was removed from cable characteristic impedance. |
| `audio_psu_hum_amplitude`, `audio_noise_floor`, `audio_saturation_drive` | Explicit generic audible wear, reduced to restrained levels. Console/speaker responses are shared between CPU and GPU. |

The PVM reference is [Sony's published specification](https://www.sony.jp/pro-monitor/products/PVM-14L2/). NES signal values come from [terminated voltage measurements](https://www.nesdev.org/wiki/NTSC_video). [Video-amplifier fault descriptions](https://www.repairfaq.org/REPAIR/F_monfaq.html) support the kinds of streaking/regulation defects, not the numerical tuning of an individual preset. Further assumptions and missing physical models are listed in the [pipeline reference](gpu-pipeline-reference.md).


## Curated colour and connection defaults

| Profile | Connection | White point | R−Y / B−Y gain offset | Gun gamma | Spot growth at white |
|---|---|---:|---:|---:|---:|
| PVM-14L2 | Composite | D65 | 0 / 0 | 2.4 | 25% |
| JVC D-Series | Composite | 9300 K | +16% / −2% | 2.35, small tracking offsets | 40% |
| Toshiba 14AF43 | Composite | 8000 K | +6% / +2.5% | 2.3, small tracking offsets | 45% |
| Stas's Favourite | RF | 7800 K | +10% / −3.5% | 2.2, worn tracking | 55% |

Consumer colour settings are explicit estimates; they are not extracted factory coefficients. The JVC owner record reports cool Standard mode and red push. Toshiba's service procedure specifies visual white-balance adjustment, without establishing our 8000 K target. PVM D65 is documented. Nominal 525-line phosphor primaries are a standards-based approximation to the unmeasured tubes. Decoder colour-difference gains preserve the gray axis; gun balance and phosphor gamut act at their respective stages.

Stas uses channel-3 metadata, −25 dBm sync-tip carrier, −65 dBm injected channel noise and a 4.1 MHz equivalent video corner. RF can be selected on any display; on the tunerless PVM this represents an external receiver. Stock NES composite/RF and hypothetical modified component/RGB sources are distinct choices.
