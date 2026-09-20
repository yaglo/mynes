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

Burst is measured after the analogue path. The colour killer uses received burst amplitude rather than dark picture pixels. AGC compares sync tip with the back porch, independently of scene brightness. NTSC line combs use a broadcast receiver's 2730-sample delay; a NES line is 2728 samples, so the two-sample horizontal displacement is intentional.

RF noise varies by frame. Hum uses actual sample time including horizontal blanking and the region's frame period. Zero-valued hum and persistence settings disable their effects.

## CRT and HDR

Gun response converts voltage to light before Gaussian beam deposition. Beam profiles have unit area, so changing focus spreads energy rather than creating it. Phosphor decay is recursive per-channel history, not just a blend with the previous unaccumulated frame. Reset and resize invalidate history. Optional motion smoothing is a separate display effect, not a 3D receiver comb.

Beam textures, persistence, masks and glass scattering use linear light. Aperture-grille coverage integrates the pixel footprint analytically. Dot and slot masks use footprint samples; unresolved patterns blend toward their mean to reduce aliasing. Masks modulate the existing landed beam rather than blurring neighbouring RGB phosphors a second time. Their normalization preserves average white-field energy, which means resolved phosphor peaks can require substantially more headroom than the field average.

`mask_pitch_px` is phosphor-cell spacing in drawable pixels. Older files using the misleading `mask_pitch_mm` key still load. This is **not** a measured tube pitch in millimetres or automatic calibration to a monitor's physical subpixels.

`hdr_gain` is a linear luminance multiplier. The EDR path writes extended linear sRGB. It checks swapchain setup success and reads current SDL HDR headroom and SDR-white scaling. The SDR path uses the same light model and applies the sRGB transfer function at output. Peaks beyond available headroom clip; an SDR panel cannot reproduce arbitrarily bright, narrow phosphor peaks while retaining both average brightness and black gaps.

Halation redistributes linear light; it is no longer a gamma-domain brightness boost. Screen-reflection, aging and purity controls remain phenomenological approximations.

## Pacing and performance

The renderer waits for an available swapchain image instead of advancing through unavailable frames. A console-rate deadline also prevents emulation from following a 120 Hz display at double speed. This does not provide VRR or an audio-master multi-rate scheduler; sustained machine contention can still reduce playback speed.

Normal GPU display does not download decoded RGB. Explicit diagnostic captures may synchronize/read back. GPU validation is opt-in with `MYNES_GPU_VALIDATION=1`; tests enable it. The editor labels its per-stage measurements as CPU encoding time. No performance claims should be based on the recent contended runs.

## Validation

- `ctest --test-dir build -R '^gpu_' --output-on-failure`
- `swift test --package-path tools/visualiser`
- `python3 frontends/gpu/tests/test_editor_ipc.py build/bin/mynes_gpu`

The fidelity suite checks CPU/GPU DAC equivalence, burst phase and DC recovery, loss of burst, scene-independent sync AGC, changing/repeatable RF noise, recursive persistence, reset/resize, rendered SDR transfer, linear HDR gain, display-headroom limits, and mask energy/colour balance.

Set `MYNES_CAPTURE_PATH=/tmp/crt.ppm` to export the final CRT render after 180 frames. This is an SDR preview of the final shader, with EDR highlights clipped to reference white. It cannot verify physical HDR luminance. The older screenshot/debug paths export intermediate buffers.

## Limits and sources

This is a sampled behavioural model, not a calibrated reproduction of every NES revision and CRT. Horizontal gates are fixed; there is no dynamic sync-separator/deflection PLL or vertical-sync raster simulation. Burst phase is estimated per line, not through a measured analogue PLL. NTSC borders use blanking rather than per-dot backdrop history. PAL horizontal blanking currently shares the NTSC dot-layout approximation. RF models baseband impairments rather than a complete tuner/VSB carrier/envelope circuit. S-Video is a bandwidth/topology approximation, not separately captured hardware Y/C output. Tube presets are tuning profiles, not measurements of named specimens.

These limits are explicit so a plausible-looking image is not mistaken for verified hardware equivalence.

References:

- [NESdev: NTSC video and measured 2C02 timing/levels](https://www.nesdev.org/wiki/NTSC_video)
- [NESdev: PAL video and alternate-line decoding](https://www.nesdev.org/wiki/PAL_video)
- [SDL GPU swapchain formats and transfer functions](https://wiki.libsdl.org/SDL3/SDL_GPUSwapchainComposition)
- [SDL dynamic window HDR properties](https://wiki.libsdl.org/SDL3/SDL_GetWindowProperties)
