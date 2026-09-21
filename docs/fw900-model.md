# Sony GDM-FW900 empirical monitor model

Select **Sony GDM-FW900 + scaler**. `tv.monitor_model = 1` selects a separate
1920 × 1200 deposition path; zero preserves the 240-line television model.
The Electron beam menu also exposes this choice. This is an empirical spatial
and tonal approximation, not a circuit-level reproduction of every FW900 stage.

## Source and signal path

[NIDL report 751810601-120, September 6, 2001](https://archive.org/download/DTIC_ADA415156/DTIC_ADA415156.pdf)
([NTIS catalogue](https://ntrl.ntis.gov/NTRL/dashboard/searchResults/titleDetail/ADA415156.xhtml))
tested 1920 × 1200 at 85 Hz, with an 18.870 × 11.818 inch image.
The transcribed tables and qualifications are in
[`fw900_nidl.json`](../tools/measurements/fw900_nidl.json).

The FW900 cannot directly accept the NES's 15 kHz raster. This preset places
our **generic composite decoder** before an explicit external scaler:

1. Decode and filter NES voltage using the existing receiver.
2. Resample horizontally into 1600 pixels and repeat each NES line five times.
3. Place that 1600 × 1200 image in a 1920 × 1200 raster with 160-pixel side bars.
4. Apply the measured tone response, then deposit each monitor raster line.
5. Apply the grille, effective phosphor-primary transform and host output transfer.

The tube occupies a 16:10 viewport; the game remains 4:3. Horizontal scaling
uses linear voltage interpolation; vertical scaling uses line repetition.
These scaler choices are explicit assumptions, not a measured scaler product.
The receiver's MHz bandwidth settings describe the external decoder, not the
FW900's high-bandwidth RGB electronics. The old 240-line gun/spot stages are
bypassed so the tube is not applied twice.

**Temporal conversion to 85 Hz is not implemented.** Source and host presentation
retain their existing cadence. Applying the report's spatial response at that
cadence is an approximation. Persistence is disabled in this preset because
this dataset does not provide the required decay measurement.

## What the data constrain

- **Tonal response:** 256 measured values, linearly interpolated, normalized as
  `(L(code) - 0.102) / (31.12 - 0.102)` in foot-lamberts. This is a system response
  measured with varying surrounds, not an isolated gun EOTF. Applying it equally
  to R/G/B is an assumption; channel-specific transfer measurements are absent.
  Input is limited to the measured 0–255 domain. The model removes the measured
  black offset rather than inventing its split between ambient, leakage and glare.
- **Resolution:** the nine-site one-on/one-off contrast-modulation grids fit
  Gaussian equivalents separately for horizontal sample-and-hold video and
  vertical scanline impulses. At the centre, sigma is 0.43517 horizontal raster
  pixels and 0.46480 vertical raster lines. This is **not a complete measured MTF**
  or a unique separation of video amplifier and beam blur. Four-sigma support
  and analytical pixel-area integration prevent undersampled scanline flicker.
- **Uniformity:** the nine white-field measurements range from 28.2 to 31.1 fL.
  They are normalized to the centre and interpolated bilinearly. Positions use
  10% insets following the procedure; outside those anchors, values clamp.
  No additional generic vignette is applied.
- **Grille:** 0.23 mm centre and 0.27 mm edge pitch, over the 479.298 mm image
  width. Quadratic interpolation of pitch and its integrated phase keep stripes
  continuous. Stripe widths/gaps remain nominal; they are not independently
  measured. The grille is filtered when unresolved, and its physical density is
  never quantized to a minimum three-host-pixel triad.
- **Effective primaries:** Table II.22.1's R (.602,.344), G (.280,.599),
  B (.152,.073), normalized to D65 for reproducibility. These are effective test
  coordinates, not phosphor spectra. The report's different white/colour sections
  disagree; this preset does not claim to reproduce its 9200 K test white exactly.
  White-balance controls use the inverse measured tone curve in this mode.

Geometry and convergence controls remain available as generic adjustments.
The measured model replaces generic spot width/growth, focus scaling and gamma
controls; changing those does not refit the measurements. No unmeasured dynamic
spot growth, regulation, damper-wire dimensions or halation kernel is added.
The report's halation normalization and patch geometry conflict (see
[measurement notes](crt-measurements.md)); the preset therefore disables that
extra contribution. The effective transfer and resolution measurements already
include effects that cannot be separated from these data alone.

## Reproduction and verification

```sh
python3 tools/measurements/build_fw900_model.py --check
cmake --build build -j 8
ctest --test-dir build --output-on-failure -R 'gpu_(fidelity|preset_json|preset|video_chain_helper).*'
build/bin/mynes_gpu --preset presets/sony_gdm_fw900.json path/to/game.nes
```

The dependency-free generator records every fitted coefficient. GPU tests feed
native 1920 × 1200 test signals into the same shader, sample all nine sites at
1/8-pixel footprints, and read back linear RGBA16F. The measured Cm values differ
from the report by at most 0.0048 (absolute), including integration and half-float
rounding; the tolerance is 0.012. White uniformity, middle grey, black, and the
external scaler's side bars are checked separately. These checks establish that
the implementation matches its fitted targets, not independent validation of a
complete physical monitor. Legacy TV energy and spot-growth tests still pass.

An unaveraged 1280 × 800 Contra capture has no broad 240-line TV gaps, as expected
for this high-resolution PC raster. At ordinary window sizes its fine grille
largely averages away. This monitor is a different look from a domestic NES TV;
more conspicuous scanlines are not evidence of a more accurate FW900.
