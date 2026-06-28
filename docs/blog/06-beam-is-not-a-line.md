# The Electron Beam Is Not a Line

*Brightness-dependent bloom, convergence error, and CRT physics*

---

Most CRT shaders darken every other row. Every pixel in the dark row gets the same treatment -- multiply by 0.3, maybe 0.5. The result is a uniform grid of dark horizontal bars over the image. It looks nothing like a real CRT, and the reason is simple: the electron beam is not a line.

The beam has a Gaussian cross-section. Its width depends on brightness. A dark pixel produces a narrow beam -- the gap between scanlines is visible, dark, empty phosphor. A bright pixel produces a wide beam that blooms into adjacent lines, filling the inter-scanline gap with light. This is why CRT photographs of bright scenes never show visible scanlines. The beam is too wide. The gaps are filled.

This single physical fact -- that beam width varies with brightness -- accounts for most of what makes real CRT footage look different from CRT shader output. Stage 11 of the pipeline models it.

## The beam profile algorithm

Each output pixel composites contributions from three adjacent NES scanlines: the current line and its two neighbors. For each scanline, the shader computes a brightness-dependent Gaussian weight.

The core of `beam_profile.comp.glsl`:

```glsl
for (int soff = -1; soff <= 1; soff++) {
    // Read RGB from the pre-blurred signal buffer
    float lR = rgb_in[(uint(r_line) * signal_w + uint(r_sx)) * 3u + 0u];
    float lG = rgb_in[(uint(g_line) * signal_w + uint(g_sx)) * 3u + 1u];
    float lB = rgb_in[(uint(b_line) * signal_w + uint(b_sx)) * 3u + 2u];

    // Luminance determines beam width
    float lY = clamp(0.299 * lR + 0.587 * lG + 0.114 * lB, 0.0, 1.0);
    float bloom_t = pow(lY, bloom_gamma);
    float sv = sigma_narrow + (sigma_wide - sigma_narrow) * bloom_t;
    float inv2s = 1.0 / (2.0 * sv * sv);

    // Gaussian beam intensity at this vertical distance
    R += lR * exp(-(rd * rd) * inv2s);
    G += lG * exp(-(gd * gd) * inv2s);
    B += lB * exp(-(bd * bd) * inv2s);
}
```

The `bloom_gamma` parameter controls the nonlinearity of the bloom curve. At `bloom_gamma = 1.0`, the relationship is linear -- beam width scales directly with brightness. At `bloom_gamma = 1.5` (the default), moderate brightness values produce relatively narrow beams, and the bloom effect is concentrated in the upper brightness range. This matches real CRT behavior: the beam broadening accelerates as current increases.

The two sigma values define the range. `sigma_narrow = 0.20` for dark pixels: a tight Gaussian that barely extends beyond the scanline center. `sigma_wide = 0.70` for full-brightness pixels: wide enough that adjacent scanlines overlap significantly. The interpolation `sigma = sigma_narrow + (sigma_wide - sigma_narrow) * bloom_t` maps the full brightness range onto this sigma range.

The Gaussian `exp(-d^2 / 2*sigma^2)` computes the beam intensity at vertical distance `d` from the scanline center. When sigma is small (dark pixel), the falloff is steep -- almost no energy reaches the adjacent line. When sigma is large (bright pixel), the falloff is gentle -- significant energy bleeds up and down, filling the inter-scanline gap.

## Per-channel convergence error

A color CRT has three electron guns -- red, green, blue -- physically separated in the tube neck. Each gun fires its beam through the shadow mask (or aperture grille) to hit its corresponding phosphor dots. The guns must be aligned so all three beams converge on the same triad at every screen position.

Perfect convergence across the entire screen is impossible. The deflection geometry means that alignment achieved at the center drifts at the edges. The beam shader models this as per-channel offsets that scale with distance from the center:

```glsl
float edge_factor = cx * cx + cy * cy;
float r_cx_off = conv_r_x * edge_factor;
float r_cy_off = conv_r_y * edge_factor;
float b_cx_off = conv_b_x * edge_factor;
float b_cy_off = conv_b_y * edge_factor;
```

Green is the reference channel -- it reads from the unshifted sample position. Red and blue read from offset positions, both horizontally (in signal samples) and vertically (in output rows). The offset is zero at screen center (`edge_factor = 0`) and maximum at the corners.

A well-calibrated PVM has convergence offsets near zero -- you would need a test pattern to see the error. The Basement TV preset sets `conv_r_x = 6.0, conv_b_x = -5.0`. That is 6 signal samples of red shift and 5 samples of blue shift in the opposite direction, increasing toward the corners. Every sharp edge has visible red-blue color fringing. White text on black gains colored halos. It is immediately, viscerally recognizable as "that old TV."

## Per-pixel Gaussian noise

Electronic noise in a CRT signal path is Gaussian-distributed -- thermal noise in resistors, shot noise in transistors. Uniform random noise (what most shaders use) has the wrong distribution. The visual difference is subtle but real: Gaussian noise has occasional larger excursions that give the "snow" its characteristic texture.

The shader generates six independent hash streams per pixel using Murmur3, then applies the Box-Muller transform to convert uniform random values to Gaussian:

```glsl
float nr = sqrt(-2.0 * log(u1)) * cos(u2);
float ng = sqrt(-2.0 * log(u3)) * cos(u4);
float nb = sqrt(-2.0 * log(u5)) * cos(u6);
```

Three independent Gaussian samples -- one per channel. The noise amplitude is modulated by local luminance:

```glsl
float noise_scale = noise_level * (1.0 - 0.8 * clamp(luma, 0.0, 1.0));
```

More noise in shadows, less in highlights. This models the signal-to-noise ratio: the noise floor is constant, but bright areas have more signal, so the noise is proportionally less visible. Dark areas, where the signal is weakest, show the most snow.

## Mains hum

The power supply's 60 Hz ripple modulates the beam brightness. A cheap TV with poor power supply filtering shows a slowly rolling brightness bar -- a horizontal band of slightly different brightness that drifts up through the frame over several seconds.

Real rectifier-derived hum is not a pure sine wave. It has harmonics:

```glsl
float hum_wave = sin(hum_phase)
               + 0.40 * sin(2.0 * hum_phase + 0.8)
               + 0.15 * sin(3.0 * hum_phase + 1.5);
```

The fundamental at 60 Hz, a 120 Hz second harmonic at 40% amplitude, and a 180 Hz third harmonic at 15%. The phase offsets (0.8, 1.5 radians) model the non-ideal phase relationships in a real full-wave rectifier. The bar rolls slowly because `frame_counter * 0.006` advances the phase by a fraction of a radian per frame.

## Other beam physics

The shader handles several more physical effects, each a few lines of GLSL:

**Horizontal timebase jitter.** Two incommensurate sine waves produce a slow, irregular horizontal sway of the entire image -- like a CRT whose horizontal oscillator caps are drifting. The amplitude is controlled by `h_jitter`. PVMs have rock-solid timebase (`h_jitter = 0.0`). A cheap TV has visible wobble.

**Edge focus degradation.** The beam defocuses at screen edges -- longer throw distance, yoke astigmatism. `edge_focus` widens the beam sigma by `(1 + edge_factor * edge_focus)`. A value of 0.3 makes corner text noticeably softer than center text.

**Velocity dimming.** The beam sweeps faster at the edges of the screen (nonlinear deflection). Faster sweep means less energy deposited per pixel. The attenuation is `1.0 - edge_factor * velocity_dim`. This is separate from vignette, which models the optical cos^4 illumination falloff.

**Geometry warp.** Pincushion distortion plus S-correction, applied as horizontal position shifts modulated by vertical position. The Wega preset has `barrel = 0.0` (flat tube). The Basement TV has `barrel = 0.05, barrel_v = 0.08` -- significantly more vertical curvature from an aged deflection yoke.

**RF interference.** Electromagnetic interference from nearby electronics produces a stepped vertical zigzag: `floor(sin(sy * 0.45) * rf_interference + 0.5)`. The `floor` is important -- real RF interference produces discrete pixel shifts, not smooth undulation.

## Output format

The beam shader writes packed float16x4: two uint32 values per pixel, encoding RGBA as half-precision floats via `packHalf2x16`. Values above 1.0 are preserved -- the phosphor can overshoot SDR white during bloom, and subsequent stages (glass halation, tone mapping) need the full dynamic range. Soft-clamping at 4.0 prevents numerical fireflies.

## Why this stage matters most

The beam profile shader is roughly 200 lines of GLSL. It is the most visually impactful stage in the pipeline. Every preceding stage -- the DAC, the cable model, the comb filter, the chroma demodulator -- feeds into this one. The composite decode could be mathematically perfect, producing flawless RGB values. Without the beam profile, the result looks like an LCD with a color filter. With it, the image has the depth and texture of a real display: dark areas with visible scanline structure giving way to bright areas where the beam fills the gaps, edges softly fringed with convergence error, a faint snow of Gaussian noise in the shadows.

The beam is not a line. It is a probability distribution whose parameters depend on the signal. Everything else follows from that.
