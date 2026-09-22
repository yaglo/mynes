# Published CRT measurements

## First implemented target: effective veiling glare

[Flynn and Badano (1999), Tables 1–2](https://doi.org/10.1007/BF03168843)
report measurements for a Hitachi SuperScan Elite 751 colour monitor.
The [transcribed dataset](../tools/measurements/hitachi_751_glare.json) records
the geometry, black subtraction, repeatability and reflection coefficients.
The source's Figure 2 is a simulated monochrome example, **not this monitor's
measured point-spread function**. Total veiling glare includes electronic and
optical contributions; the dataset cannot separate them.

Our effective approximation is

```
K = (1 - a) delta + a G_sigma
G_sigma = exp(-r² / (2 sigma²)) / (2 pi sigma²)
q(r) = a [exp(-r² / (2 sigma²)) - exp(-R² / (2 sigma²))]
       / [1 - a exp(-R² / (2 sigma²))]
glare_ratio = 1 / q(r)
```

Here `r` is the dark-disk radius, `R` the outer bright-disk radius, and `q`
is the centre luminance divided by the bright reference. Both terms of `K`
have unit integrated energy before weighting. This model redistributes light
without increasing a uniform field's luminance. It assumes linearity and
shift invariance, with a narrow direct component and a Gaussian scatter tail.
It does not solve electron trajectories or internal optical reflections.

Run the dependency-free fit:

```sh
python3 tools/measurements/fit_crt_glare.py
```

It produces `sigma = 8.144620194 mm` and `a = 0.048294485894`.
These are **fitted parameters under our assumptions**, not measured spot sizes.
Two parameters fit two observations: there are no independent validation
points. A long-tailed or multi-component kernel could also fit these data.

## Renderer and selectable experiment

The renderer now accepts `tv.halation_sigma`, the scatter sigma divided by
picture height. In the Glass menu this is **Halo width / height (0=auto)**.
Positive widths use a normalized 33-tap separable Gaussian with four-sigma
support. Zero retains the existing kernel and width, preserving older presets.
The implementation reuses the three existing halo passes and scratch textures.

**Lab: measured glass scatter** is a generic composite receiver with this fitted
effective scatter. Its physical picture height is explicitly assumed to be
270 mm, giving `halation_sigma = 0.03016526`. It is not a reproduction of the
Hitachi receiver, scanning system, phosphor, mask or beam. Those parts retain
the Reference composite settings. The preset is reproducible with:

```sh
python3 tools/measurements/fit_crt_glare.py \
  --base-preset presets/reference_composite.json \
  --picture-height-mm 270 \
  --output-preset presets/measured_glare_experiment.json
```

Changing the assumed picture height changes the width in image coordinates;
resizing the host window does not change that physical assumption. The preset
uses neutral scatter tint and disables the extra generic internal-scatter
contribution to avoid adding another scatter fraction on top of the fit.
The reflection coefficients remain recorded data, not renderer settings:
converting them requires an illumination model and absolute luminance units.

## Actual GPU check, 2026-09-22

`test_display_fidelity.c` renders area-covered disks, reads linear RGBA16F
output and compares centre luminance to a separate bright-disk rendering.
Beam, mask, ambient and output tone mapping are excluded to isolate this stage.
Its 400 mm square is a test coordinate domain, not the monitor's dimensions.

| Output size | Dark diameter | Published ratio | GPU ratio |
|---|---:|---:|---:|
| 512 × 512 | 10 mm | 25 | 24.6812 |
| 512 × 512 | 20 mm | 44 | 42.0167 |
| 1024 × 1024 | 10 mm | 25 | 24.8121 |
| 1024 × 1024 | 20 mm | 44 | 43.6402 |

The 5% test tolerance accommodates quadrature, reduction, pixel coverage and
finite readback area. It is larger than the paper's reported repeatability;
we do not claim agreement within measurement uncertainty. Higher-resolution
agreement is below 1%. Separate uniform-field checks verify energy preservation.
JSON round-trip, missing-field compatibility and the preset/control audits pass.

Unaveraged, same-phase 1280 × 960 Contra captures show broader light spill into
black regions around bright objects. The overall difference remains modest.
This does not establish a dramatic improvement to the complete CRT image.

## Implemented FW900 spatial and tonal target

The NIDL [Sony GDM-FW900 evaluation](https://ntrl.ntis.gov/NTRL/dashboard/searchResults/titleDetail/ADA415156.xhtml),
publication 751810601-120, September 6, 2001, provides grayscale response,
spatial contrast modulation, uniformity, colour and halation results at
1920 × 1200. The [full report](https://archive.org/download/DTIC_ADA415156/DTIC_ADA415156.pdf)
is public. It is a stronger candidate for a full measured **computer monitor**
model than an arbitrary television with only a service schematic.

Source checks before fitting:

- Printed pages 10–12 disagree on halation normalization. The measured centre
  values are 1.015 and 19.54 foot-lamberts. Their ratio gives the reported 5.19%;
  the displayed formula using the luminance difference gives 5.48%.
- The same section's nominal 0.01%-area patch and 11-pixel side do not agree
  with the stated raster. Do not fit a physical kernel to that geometry yet.
- Contrast modulation at a single pattern frequency is not a full MTF curve.
- A grayscale measurement with changing surrounds can include regulation and
  stray light; it should not automatically become the isolated gun EOTF.

A separate FW900 raster path is now implemented. See [the model and its limits](fw900-model.md)
for the scaler, transfer, resolution fits, physical grille, GPU comparisons and
remaining temporal/optical limitations. It does not turn the original TV model
into an FW900 by merely renaming a preset.
