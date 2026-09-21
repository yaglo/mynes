# Consumer CRT review, 2026-09-22

The review identified over-broad raster profiles and overly cleanly resolved
phosphor patterns in four generic looks. These changes are **tuning assumptions**,
not new measurements of four specific tube/chassis combinations.

| Profile | Changes |
|---|---|
| Bedroom RF 1990 | In-line slot mask; dark/white FWHM 0.40/0.95 source lines; less peripheral defocus; horizontal spot growth 0.30; light fine surface scatter; RF floor -52 dBm and a 0.6% short echo. Existing Y/C decoder retained. |
| Basement TV | FWHM 0.48/1.10 lines, smaller horizontal spot with growth, less glass attenuation and gun imbalance. Voltage noise 0.025 → 0.003, RF floor -44 → -50 dBm. Reduced lifted black and broad halo. |
| Compact video monitor (`commodore_1702.json`) | Smaller horizontal spot, FWHM 0.42/0.90 lines, less convergence error, modest horizontal growth. Separated Y/C path retained; still explicitly generic, not a measured 1702. |
| Arcade Cabinet | Narrower dark spot, horizontal growth 0.45, modest fine surface scatter. RGB remains an explicit ideal voltage-derived source, not a stock NES output or a PlayChoice-10 palette model. |

The echo is a generic reception-path impairment; its delay is not calculated
from the preset's short cable length. Fine surface scatter uses the existing
post-mask filter, whose coefficients are nominal. It does not enlarge the mask
holes when the electron beam grows. Electron spot growth and optical spreading
are separate mechanisms. No current-dependent mask erasure has been added.

A CRT has finite beam width in **both** axes. For example, the
[AAPM TG18 report](https://www.aapm.org/pubs/reports/or_03.pdf) discusses beam-current
dependence of spot size and the distinction between continuous and structured
phosphor screens. Vertical bandwidth blur should not be introduced by treating
RF luma filtering as a two-dimensional image blur. This implementation keeps
receiver bandwidth filtering horizontal, before tube deposition. RF noise
already enters the RF path; clean RF is possible, so noise is a preset reception
condition rather than an unavoidable property of RF.

## Brightness check

The current PVM Contra platform uses NES code `0x10`, not peak white. Its measured
DAC rails in the emulator give `(840 - 312)/(1100 - 312) = 0.67005` voltage.
With gamma 2.4 that is approximately 0.383 linear emitted light. The current
1280 × 960 SDR capture's flat region (x 550–629, y 510–529) averages approximately
0.371 after decoding sRGB. Its average encoded channels are about 157/255, not
105/255. A different crop, old build, host output mode or image scaling can change
these numbers; this check does not establish the conditions of the review image.

Beam kernels already conserve linear energy, as do the normalized masks.
Averaging encoded screenshot bytes is not a luminance measurement. No global
compensating gain or PVM decoder retune was applied without evidence for it.
SDR highlight compression can still lose energy when resolved phosphor peaks
exceed host headroom; HDR and physical mask density affect that limitation.

## Matched image checks

Same Contra PPU framebuffer, phase 4, 1280 × 960 SDR, integer mask periods,
60 Hz hold, frame 60. Samples use linear-light luminance and are not calibrated
photometer measurements. A small flat platform region (x 555–599, y 510–529)
shows these changes in the row-average `(max-min)/(max+min)` modulation:

| Look | Before | After | Mean linear Y before / after |
|---|---:|---:|---:|
| Bedroom RF | 0.121 | 0.284 | 0.430 / 0.427 |
| Basement | 0.070 | 0.162 | 0.211 / 0.303 |
| Compact Y/C | 0.121 | 0.222 | 0.368 / 0.368 |
| Arcade RGB | 0.100 | 0.208 | 0.384 / 0.384 |

The raster is clearer without purchasing that contrast through a global loss
of light. Basement becomes brighter by removing its excessive preset attenuation.
Mask comparisons should be viewed at 1:1: scaling encoded screenshots can create
colour moiré that is absent in six-column linear-light averages of the source.
