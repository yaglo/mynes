# Living Room 1988

*Parameterizing nostalgia with datasheets*

*Part 7 of "Building a NES Emulator That Thinks Like Hardware"*

**Historical development chapter.** The numerical tuning and preset names below
belong to an earlier prototype. The current Living Room preset is generic, not
a datasheet-calibrated Zenith. Its JSON, reflection settings and evidence limits
are documented in the [current 4K preset audit](../gpu-preset-audit.md).

---

What did the NES actually look like?

Not "NTSC with scanlines." Specifically: what did a NES front-loader, connected via a composite cable to a 19-inch Zenith shadow mask TV, in a suburban living room with a lamp on, look like?

The answer is a product of specific electronics, and every component in that chain has a datasheet. The cable has a capacitance spec. The TV has a dot pitch from its service manual. The phosphors have a persistence curve from JEDEC. The living room has a lamp. Each of these shapes the image in measurable ways, and each maps to a parameter in the emulator's preset system.

## The preset, parameter by parameter

Here is the Living Room 1988 preset from `frontends/gpu/presets.c`, and where each value comes from.

**Cable: 2m cheap RCA composite.** The bundled cable that came in the box or was bought for $3.99 at RadioShack. Capacitance is 80 pF/m -- this is the spec for cheap molded RCA cables with thin foil shielding, as opposed to 67 pF/m for decent RG-59 coax or 50 pF/m for quality shielded cables. At 2 meters, the total shunt capacitance is 160 pF, which forms a low-pass filter with the 75-ohm source impedance and softens the signal before it reaches the TV.

```c
.video_cable = {
    .length_meters       = 2.0f,
    .capacitance_per_m   = 80e-12f,    /* 80 pF/m (cheap RCA) */
    .connector_resistance = 0.3f,       /* nickel-plated RCA */
    .impedance           = 75.0f,
    .shield_effectiveness = 0.85f,
},
```

Connector resistance: 0.3 ohms. This is nickel-plated RCA -- not gold (0.05 ohms), not corroded (3.0+ ohms). Just average consumer connectors in decent condition. The shield effectiveness of 0.85 means 15% of external interference leaks through -- noticeable as mild snow, but not distracting.

**TV input: no comb filter.** A 19-inch Zenith in 1988 was a budget set. It separates luma from chroma using simple bandpass filters, not a comb filter. The chroma bandwidth is 0.4 MHz (narrow -- colors bleed) and the luma bandwidth is 4.2 MHz (adequate but not great). For comparison, a Sony Wega with its 2-line comb filter gets 1.0 MHz chroma bandwidth, and a PVM on S-Video gets 1.3 MHz.

```c
.chroma_bandwidth   = 0.4e6f,
.luma_bandwidth     = 4.2e6f,
```

**Hue offset: 2.0 degrees.** Nobody calibrated consumer TVs. The tint knob on the back panel was set at the factory and never touched again. Two degrees is barely perceptible -- a slight warmth to reds -- but it's the kind of drift that makes the image feel "real" rather than clinically correct.

**Gamma: 2.45.** Standard CRT gamma is 2.40 for P22 phosphors. This TV is a consumer set with some age, so gamma has drifted up slightly. A PVM calibrated to spec sits at 2.20. The Basement TV preset pushes to 2.50.

**Shadow mask: 0.60mm dot pitch.** From the Zenith service manual for a 19-inch shadow mask tube. Smaller is sharper: a PVM's aperture grille runs 0.31mm, a 25-inch arcade monitor has 0.28mm shadow mask, and a cheap 13-inch portable has 0.80mm. The mask pitch controls how visible the phosphor structure is at viewing distance.

**Color temperature: 6500K (D65).** The NTSC standard white point. Japanese TVs of the era ran at 9300K (D93) -- noticeably bluer -- which is why the Famicom Kitchen preset sets `color_temperature = 9300.0f`.

**Coupling cap: 10 microfarads.** From the NES-001 motherboard schematic. This DC-blocking capacitor sits between the 2C02 PPU's output and the composite video jack. With a 10k-ohm load impedance, the cutoff frequency is about 1.6 Hz -- low enough to pass all video content but high enough to block DC offset. The Famicom uses a 100 microfarad cap (cutoff at 0.16 Hz), which subtly changes the low-frequency response.

```c
.console_coupling_R = 75.0f,
.console_coupling_C = 10e-6f,       /* NES front-loader */
```

**Ambient light: 0.08.** A lamp is on in the living room. This washes out the blacks slightly and reduces perceived contrast. Zero ambient (dark room) is for the Retro Gaming Setup preset; 0.12 is the Famicom Kitchen with a fluorescent overhead.

**Noise: 0.035.** Mild snow. Visible if you look for it, invisible once you're playing. The shield_effectiveness of 0.85 lets enough interference through the cable to produce this level. The Basement TV preset cranks noise to 0.080.

**Hum bar: 0.02.** The VCR is on the same power strip. 60 Hz mains hum couples into the video signal as a slowly-rolling brightness band. At 0.02 it's subtle -- you'd only notice it on solid-color screens. The Basement TV, with its ground loop from an extension cord, runs at 0.08.

**Convergence: 0.15 static, 0.08 dynamic.** The three electron beams (red, green, blue) don't land on exactly the same spot. Static misconvergence is a fixed offset; dynamic misconvergence varies across the screen, worse at the edges. At these values, you get mild color fringing visible on high-contrast edges -- red shifted right by 2.5 signal samples, blue shifted left by 2.0. Pause the game and look at white text on a black background and you'll see it.

```c
.convergence_static   = 0.15f,
.convergence_dynamic  = 0.08f,
.conv_r_x = 2.5f, .conv_r_y = 0.8f,
.conv_b_x = -2.0f, .conv_b_y = -0.6f,
```

**Halation: 0.06.** Bright areas of the phosphor screen scatter light through the glass faceplate, producing a soft glow around bright sprites. At 0.06 it's gentle -- visible around Mario's white gloves against a dark background, invisible in most gameplay.

**Barrel distortion: 0.04.** The tube face is curved. Lines near the edges bow outward slightly. At 0.04 you can see the curvature but it's not extreme. The Bedroom RF preset runs 0.05 on its smaller, more curved tube.

## The opposite end: Studio PVM

The PVM preset is what reviewers and YouTube retro channels show you. S-Video input (luma and chroma already separated -- no comb filter needed). Aperture grille at 0.31mm pitch. Calibrated D65 white point. beam_sharpness at 1.00 (maximum). Zero noise. Zero hum. Gold-plated connectors at 0.05 ohms.

```c
.connection      = VIDEO_CONN_SVIDEO,
.comb_type       = VIDEO_COMB_BYPASS,
.video_cable = {
    .connector_resistance = 0.05f,   /* gold-plated mini-DIN */
    .shield_effectiveness = 0.95f,
},
.tv = {
    .chroma_bandwidth   = 1.3e6f,
    .beam_sharpness     = 1.00f,
    .noise_level        = 0.0f,
    .hum_bar_amplitude  = 0.0f,
    .gamma              = 2.20f,    /* calibrated */
},
```

Nobody played NES on a PVM in 1988.

## The pathological case: Basement TV

Three meters of corroded coax. Connector resistance at 3.0 ohms (the F-connectors have green patina). Shield effectiveness at 0.45 -- more than half of external interference gets through. Ghost delay of 32 samples with a ghost level of 0.12 -- a visible double image from signal reflections in the corroded connector. RF noise floor at -44 dBm. Heavy snow.

```c
.video_cable = {
    .connector_resistance = 3.0f,
    .shield_effectiveness = 0.45f,
    .ghost_delay          = 32,
    .ghost_level          = 0.12f,
},
.tv = {
    .beam_sharpness       = 0.00f,   /* completely defocused */
    .convergence_static   = 0.40f,
    .conv_r_x = 6.0f, .conv_b_x = -5.0f,
    .noise_level          = 0.080f,
    .hum_bar_amplitude    = 0.08f,
},
.rf = {
    .noise_floor_dbm  = -44.0f,
},
```

Convergence is catastrophic: red shifted 6.0 samples right, blue 5.0 samples left. beam_sharpness is 0.0 -- the beam is completely defocused. Everything is wrong. Every stage of the pipeline is stressed. This is the preset that tests edge cases, and it also happens to be what a lot of kids actually played on.

## The point

"What the NES looked like" isn't one thing. It's a product of specific electronics. Two NES consoles connected to different TVs in different rooms look different in measurable, parameterizable ways. The nine presets in `presets.c` don't simulate nostalgia -- they simulate the specific physical configuration that produced it.

The parameters are physical values: pF/m, MHz, ohms, millimeters, Kelvin. They're harder to tune by hand than perceptual sliders labeled "blur" and "color bleed." But they're easier to verify against datasheets. And if your childhood TV was a 13-inch GE portable with an RF switch box, channel 3, tinny speaker, snow visible in the corners -- there's a preset for that too.
