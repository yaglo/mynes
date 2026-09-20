# GPU video pipeline: implementation and validation

This describes the SDL3 `mynes_gpu` frontend, not the SDL2 composite renderer. The GPU work does not change the CPU/PPU core.

## Signal source

NES palette RAM holds colour/emphasis codes, not RGB values. In composite/RF modes these codes select a time-varying voltage waveform. The cached 512 × 24 tables are DAC voltage samples, not an RGB palette. NTSC uses the published Bisqwit 2C02 voltage model; PAL uses the existing measured 2C07 levels and alternate-line phase mapping.

The active image contains 256 × 240 codes. There are 8 samples per NTSC dot and 10 per PAL dot, at 12 samples per colour-subcarrier cycle. A 341-dot line therefore advances carrier phase by 4/12 cycle in NTSC and 2/12 in PAL. For running games the frontend derives the frame phase from the existing PPU clock, including skipped dots. Synthetic frames use the corresponding timing sequence.

Eligible presets upload the compact code buffer and synthesize the waveform on GPU. Presets using the existing CPU-only edge/current-load effects generate the same active voltage waveform on CPU. Both routes enter the same decoder. Tests compare their output over multiple NTSC and PAL frames.

## Receiver

The raster stage surrounds each active line with sync, porch, and colour burst. Internally, line buffers begin at horizontal sync; active video starts at sample `65 * samples_per_dot`. Analogue buffers contain `341 * samples_per_dot` samples per line; decoded RGB buffers contain only `256 * samples_per_dot`.

The current stages are:

1. DAC voltage synthesis and horizontal raster encoding.
2. Console/cable filtering, optional RF impairments, bandwidth filtering, sync-keyed AGC, and echoes.
3. Burst-gated carrier phase estimation and back-porch black-level restoration.
4. Y/C separation, luma filtering, quadrature chroma demodulation and filtering.
5. PAL parity correction and delay-line averaging when applicable.
6. Receiver colour matrix and gun amplifier response.
7. Raster landing map and beam deposition.
8. Recursive phosphor history in linear light.
9. Fixed phosphor mask, glass scattering, ambient reflection and output encoding.

Burst is measured after the analogue path. The colour killer uses received burst amplitude rather than dark picture pixels. Burst amplitude also controls chroma gain; the detector restores the factor of two lost in quadrature mixing. The color matrix has no hidden saturation boost. AGC compares sync tip with the back porch, independently of scene brightness. NTSC line combs use a broadcast receiver's 2730-sample delay; a NES line is 2728 samples, so the two-sample horizontal displacement is intentional.

S-Video computes Y from each code's full carrier-cycle mean and C from its
remaining modulation. Sync stays on Y and burst stays on C. Console/cable
filtering and reflections apply to both components without introducing luma
into chroma. This ideal separated-source mode bypasses composite-only source
edge effects and luma notching. Externally supplied diagnostic waveforms remain
composite inputs. A monochrome detail chart verifies no false colour in NTSC
or PAL, including cable reflections.

RF noise varies by frame. Hum uses actual sample time including horizontal blanking and the region's frame period. Zero-valued hum and persistence settings disable their effects.

## CRT and HDR

Gun response converts voltage to light before Gaussian beam deposition. Beam profiles have unit area and are integrated over pixel footprints, so changing focus or render size spreads energy rather than creating it. The beam texture follows the actual viewport size, including fractional scanline scales and window resize. Each gun has its own bandwidth filter. Phosphor decay is recursive per-channel history, not just a blend with the previous unaccumulated frame. Reset and resize invalidate history. Optional motion smoothing is a separate display effect, not a 3D receiver comb.

Beam textures, persistence, masks and glass scattering use linear light. Aperture-grille coverage integrates the pixel footprint analytically. Dot and slot masks use footprint samples; unresolved patterns blend toward their mean to reduce aliasing. Masks modulate the existing landed beam rather than blurring neighbouring RGB phosphors a second time. Their normalization preserves average white-field energy, which means resolved phosphor peaks can require substantially more headroom than the field average.

`mask_triads` holds a constant number of RGB triads across the tube, keeping its density stable when resizing. All bundled presets use this mode. Zero selects the legacy `mask_pitch_px` phosphor-cell spacing in drawable pixels. Older files using the misleading `mask_pitch_mm` key still load. These are **not** measured tube pitches in millimetres or automatic calibration to a monitor's physical subpixels. Unresolved triads fade to neutral unit energy near the output Nyquist limit. Mask coordinates use the CRT viewport, not the window including margins.

`hdr_gain` is a linear luminance multiplier. The EDR path writes extended linear sRGB. It checks swapchain setup success and reads current SDL HDR headroom and SDR-white scaling. The SDR path uses the same light model and applies the sRGB transfer function at output. Peaks beyond available headroom clip; an SDR panel cannot reproduce arbitrarily bright, narrow phosphor peaks while retaining both average brightness and black gaps.

Halation redistributes linear light; it is no longer a gamma-domain brightness boost. Screen-reflection, aging and purity controls remain phenomenological approximations.

## Pacing and performance

The renderer waits for an available swapchain image instead of advancing through unavailable frames. A console-rate deadline also prevents emulation from following a 120 Hz display at double speed. This does not provide VRR or an audio-master multi-rate scheduler; sustained machine contention can still reduce playback speed.

Normal GPU display does not download decoded RGB. Explicit diagnostic captures may synchronize/read back. GPU validation is opt-in with `MYNES_GPU_VALIDATION=1`; tests enable it. The editor labels its per-stage measurements as CPU encoding time. No throughput claims should be based on the recent contended runs. The editor now observes topology separately from timing; identical controls/catalogs do not invalidate views. Telemetry is limited to 4 Hz, and the footer to 1 Hz. Optional received-frame logging distinguishes a connected idle-CPU sample from a disconnected editor.

## Validation

- `ctest --test-dir build -R '^gpu_' --output-on-failure`
- `swift test --package-path tools/visualiser`
- `python3 frontends/gpu/tests/test_editor_ipc.py build/bin/mynes_gpu`

The fidelity suite checks CPU/GPU DAC equivalence, burst phase and DC recovery, loss of burst, scene-independent sync AGC, changing/repeatable RF noise, recursive persistence, reset/resize, rendered SDR transfer, linear HDR gain, display-headroom limits, mask energy/colour balance, detector gain under attenuation, fractional-scale beam energy, unresolved masks and ambient-lit margins.

Set `MYNES_CAPTURE_PATH=/tmp/crt.ppm` to export the final CRT render after 180 frames. This is an SDR preview of the final shader, with EDR highlights clipped to reference white. It cannot verify physical HDR luminance. The older screenshot/debug paths export intermediate buffers.

## Limits and sources

This is a sampled behavioural model, not a calibrated reproduction of every NES revision and CRT. Horizontal gates are fixed; there is no dynamic sync-separator/deflection PLL or vertical-sync raster simulation. Burst phase is estimated per line, not through a measured analogue PLL. NTSC borders use blanking rather than per-dot backdrop history. PAL horizontal blanking currently shares the NTSC dot-layout approximation. RF models baseband impairments rather than a complete tuner/VSB carrier/envelope circuit. S-Video derives ideal source-separated Y/C from the PPU voltage codes; it is not a measurement of a particular output modification. RGB is a decoded idealized source, not a native 2C02 output. Tube presets are tuning profiles, not measurements of named specimens.

These limits are explicit so a plausible-looking image is not mistaken for verified hardware equivalence.

References:

- [NESdev: NTSC video and measured 2C02 timing/levels](https://www.nesdev.org/wiki/NTSC_video)
- [NESdev: PAL video and alternate-line decoding](https://www.nesdev.org/wiki/PAL_video)
- [SDL GPU swapchain formats and transfer functions](https://wiki.libsdl.org/SDL3/SDL_GPUSwapchainComposition)
- [SDL dynamic window HDR properties](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties)

## Reference and preset review

`reference_composite.json` is the default for a fresh configuration. It uses a
neutral composite receiver, D65 gun balance, gamma 2.4, a fine aperture grille,
and a dark room without added wear. Existing saved preset selections still win.
Bundled profiles retain their filenames for compatibility but no longer claim
measured reproductions of named TVs. Ordinary profiles have reduced ambient
reflection, unity output gain and no decorative beam noise/jitter. Deliberately
worn profiles retain those effects.

Start comparisons with Reference composite, Studio aperture grille,
Living Room 1988, Bedroom RF 1990 and Arcade Cabinet. Other profiles remain
available as variations and for existing saved selections. The S-Video and RGB
profiles retain the source-model limitations listed above.

Visual review uses PPU-code grayscale, all 64 color codes, fine monochrome bars
and a grid. These expose black lift, hue, cross-color and scanline/mask aliasing.
Offscreen float tests independently check linear energy and HDR headroom; SDR
captures cannot establish actual display luminance or physical CRT equivalence.

## Streaming audio

The APU still owns DAC mixing and anti-alias resampling in the unchanged core.
This frontend takes its 44.1 kHz callback with the core's analogue filters set
to unity, then applies the console → cable → speaker chain exactly once.
CPU and GPU backends use the same coefficients and continuous state. The GPU
processes one short temporal block in a single dispatch with reusable transfer
buffers. There is no second decimator or fixed per-frame sample count. High-pass
coupling, amplifier bandwidth/saturation, three hum harmonics, seeded white
noise, TV coupling and both speaker biquads are implemented. Equivalent console
RC values use nominal filter corners; speakers remain generic resonant/high-cut
models, not measured cabinet/cone responses. Nonlinear processing is at 44.1 kHz.
GPU playback still requires a synchronous audio readback; CPU is the economical
default. `A` or Setup → Audio changes backend without resetting filter history.

SDL receives one mono block per emulated frame. Queue feedback reads **input**
bytes with `SDL_GetAudioStreamQueued`; converted output bytes depend on the
playback device's channel count and sample rate. The APU sample clock stays
fixed; SDL's resampler makes a smoothed correction limited to ±0.5%. The target
is 10 ms queued before a new frame. Queued input exceeding 80 ms is discarded
with a short fade-in so a stall cannot leave old gameplay buffered indefinitely.
ROM changes and browser transitions clear stale audio. These figures exclude
hardware/device latency, and persistent host overload can still underrun.

Signal Studio's Audio controls and preset saves include drive, hum frequency and
harmonics, and noise. Ordinary/reference profiles avoid added hum, hiss and
saturation; explicitly worn profiles retain those effects. The in-game audio
stage bypasses now affect both backends. `S` no longer silently removes part of
the CRT: the old switch is Setup → Diagnostics → Mask/glass bypass. It leaves
decoding, beam deposition and persistence running and is not a grid-only control.

Checks: `ctest --test-dir build -R '^gpu_' --output-on-failure` includes Metal
streaming audio comparisons, irregular block boundaries, backend state transfer,
DC/frequency response and queue-clock drift. For real game captures:

```sh
python3 frontends/gpu/tests/test_audio_playback.py build/bin/mynes_gpu game.nes /tmp/audio-review
python3 frontends/gpu/tests/capture_patterns.py build/bin/mynes_gpu /tmp/crt-review
```

Audio capture diagnostics use `MYNES_AUDIO_CAPTURE` (mono native float32,
44.1 kHz) and `MYNES_AUDIO_TRACE` (CSV queue/input count/backend). The playback
check converts captures to WAV and compares duration/samples from both backends.
`MYNES_GPU_AUDIO=1` selects GPU audio at startup. These are functional checks at
normal pacing, not performance benchmarks.

Audio queue API reference: [SDL queued input bytes](https://wiki.libsdl.org/SDL3/SDL_GetAudioStreamQueued)
and [SDL frequency correction](https://wiki.libsdl.org/SDL3/SDL_SetAudioStreamFrequencyRatio).
