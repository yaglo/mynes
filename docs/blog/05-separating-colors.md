# Separating Colors That Were Never Separate

*Comb filters, quadrature demodulation, and why composite video bleeds*

---

NTSC composite video has a fundamental design tension at its core. Luminance and chrominance occupy overlapping frequency bands. The color information -- encoded as amplitude modulation on a 3.579545 MHz subcarrier -- sits right on top of the high-frequency luma detail. The television has to pull them apart. This separation is inherently imperfect, and every imperfection is visible.

This is Stage 6 and Stage 7 of the pipeline, and they produce most of the artifacts people associate with "the NES look."

## Comb filtering: exploiting a phase trick

The NTSC subcarrier inverts phase by 180 degrees every scanline. This was a deliberate engineering decision made in 1953, and it is the key to separating Y and C without a perfect bandpass filter.

Consider two adjacent scanlines at the same horizontal position. Both carry the same luma (roughly -- the image does not change much between lines). But the chroma subcarrier has opposite phase:

```
scanline[n]   = Y + C
scanline[n-1] = Y - C    (subcarrier inverted)
```

Add them: `(Y + C) + (Y - C) = 2Y`. The chroma cancels. Subtract them: `(Y + C) - (Y - C) = 2C`. The luma cancels. This is a comb filter -- named for its frequency response, which has evenly spaced teeth like a comb.

The actual shader implements four modes, each modeling a different class of TV hardware:

```glsl
switch (mode) {
    case 0u: /* Bypass: S-Video input, Y/C already separated */
        y = signal;
        c = 0.0;
        break;

    case 1u: /* 1-line comb: cheap TV */
    {
        int prev_idx = int(tid) - int(samples_per_line);
        float prev_signal = (prev_idx >= 0) ? signal_in[prev_idx] : signal;
        y = (signal + prev_signal) * 0.5;
        c = blend * (signal - prev_signal) * 0.5;
        break;
    }

    case 2u: /* 2-line comb: decent TV */
    {
        // Uses current + 2-lines-ago (same phase) for cleaner luma
        y = (signal + prev2) * 0.5;
        c = blend * (signal - y);
        break;
    }

    case 3u: /* 3-line comb: PVM-grade */
    {
        // Average 4 scanlines -- chroma cancels over 2 complete cycles
        y = (signal + prev1 + prev2 + prev3) * 0.25;
        c = blend * (signal - y);
        break;
    }
}
```

Each mode has a characteristic failure pattern.

**No comb (not even implemented -- just a notch filter):** Cross-color on every transition where luma detail hits the subcarrier frequency. Thin horizontal lines shimmer with rainbow colors. This is the cheapest possible TV, and it looks terrible.

**1-line comb:** Good horizontal detail preservation. But the assumption that "adjacent scanlines have the same luma" breaks down on vertical edges. A sharp horizontal boundary -- like a status bar border -- produces different luma on lines N and N-1. The comb filter mistakes this luma difference for chroma. Result: rainbow fringing on every horizontal edge. This is the artifact most people remember from composite NES.

**2-line comb:** Uses the current scanline and the one two lines back (same subcarrier phase). The intermediate line with opposite phase is skipped. Better luma estimate, but still sensitive to vertical detail that changes over two lines.

**3-line comb:** Averages four consecutive scanlines. The chroma subcarrier completes a full cycle over two scanlines, so averaging four gives excellent rejection. The cost: vertical edges lose sharpness because four lines of luma are smeared together. A Sony PVM with a 3D comb filter does even better by comparing across frames, but the basic 3-line is already very clean.

The `blend` uniform controls comb strength from 0 to 1. At `blend = 0`, no chroma is extracted. At `blend = 1.0`, full comb operation. Intermediate values let the pipeline model TVs with weak comb circuits.

## Quadrature demodulation: recovering I and Q

After the comb filter separates the chroma signal, the color information is still encoded. The I (in-phase, orange-cyan axis) and Q (quadrature, green-magenta axis) components are amplitude-modulated onto cosine and sine carriers at the subcarrier frequency. To recover them, multiply by the carrier and filter out the double-frequency residual.

The modulator shader in IQ demod mode (mode 3) does both channels simultaneously:

```glsl
case 3u: /* I/Q demodulation */
{
    float gain = param_a;
    float s = x * gain;
    data_out[tid]  = s * cos(p);   // I channel
    data_out2[tid] = s * sin(p);   // Q channel
    break;
}
```

The phase `p` is computed per sample from the subcarrier frequency and sample rate: `dp = 2*pi * 3579545 / sample_rate`. But there is a critical detail -- per-scanline phase reset:

```glsl
if (samples_per_line > 0u) {
    uint scanline = tid / samples_per_line;
    uint sample_in_line = tid % samples_per_line;
    p = phase + float(scanline) * line_phase_inc
      + float(sample_in_line) * dp;
}
```

The PPU's subcarrier phase advances by a specific amount each scanline. The demodulator must track this exactly, or the recovered color drifts. The `line_phase_inc` uniform encodes this relationship. Get it wrong and the entire screen has a slowly rotating hue.

After demodulation, the I and Q channels each pass through a FIR lowpass filter (Stage 7's bandwidth limiting). This filter removes the double-frequency component from the multiplication (`cos(w)*cos(w) = 0.5 + 0.5*cos(2w)` -- the `cos(2w)` term must go). But the filter bandwidth also determines how far color spreads horizontally.

## Why composite video bleeds

A 1 MHz chroma bandwidth -- typical for a consumer TV on composite input -- means the FIR filter preserves frequency content up to 1 MHz and suppresses everything above. At a sample rate of ~21.5 MHz (the video signal's sample rate), 1 MHz corresponds to roughly 21 samples per cycle. The impulse response of the FIR spreads over several samples in each direction.

In spatial terms: a sharp color transition at pixel N spreads its I/Q energy across pixels N-4 through N+4. The color bleeds. This is not a rendering artifact. It is a physical consequence of the bandwidth. An expensive PVM with 1.5 MHz chroma bandwidth has a tighter impulse response -- less bleed, sharper color transitions. The difference between "consumer TV color bleed" and "PVM sharpness" is one float: `chroma_bandwidth`.

```c
if (conn <= VIDEO_CONN_COMPOSITE) {
    tv->chroma_bandwidth = 1.0e6f;    /* 1.0 MHz */
} else {
    tv->chroma_bandwidth = 1.5e6f;    /* 1.5 MHz */
}
```

## Dot crawl

The subcarrier phase does not just invert between scanlines -- it also shifts between frames. Over a 2-frame (or 3-frame, depending on the specific phase relationship) cycle, the cross-color pattern at any given pixel rotates through different phases. On a static image, this manifests as a crawling rainbow pattern along sharp luma transitions.

Real TVs cancel dot crawl through temporal averaging -- the phosphor persistence blends the current frame with the previous one. Stage 12 (phosphor screen) implements this with per-channel persistence weights. The crawl pattern, which alternates phase each frame, averages to zero. This is why dot crawl is prominent in screenshots (single frozen frames) but much less visible on actual CRT screens (continuous temporal blend).

The temporal blend shader reads two packed float16x4 buffers -- current and previous beam output -- and blends with per-channel weights modeling P22 phosphor decay: green persists longer than blue (`persistence_g = 1.0`, `persistence_b = 0.65-0.82`). No CPU roundtrip. The cancellation happens entirely on the GPU.

## The same math as radio

None of this is exotic. Quadrature amplitude modulation, comb filtering, FIR bandwidth limiting -- these are the same techniques used in AM/FM radio, telecommunications, and radar signal processing. NTSC is just AM radio with pictures. The 3.579545 MHz subcarrier is a carrier frequency. The I/Q channels are quadrature components. The comb filter is a spatial FIR exploiting known phase relationships.

The NES PPU does not output colors. It outputs a modulated RF waveform. Everything that happens between that waveform and the colors on screen is signal processing, and every imperfection in that processing is an artifact that defined a generation's visual memory.
