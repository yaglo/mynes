# GPU realism implementation plan

This is an engineering worklist, not a claim of hardware calibration. The
reference is published NES measurements and nominal CRT specifications, with
the Sony PVM-14L2 as one target. Unit-specific aging and calibration require
measurements of that unit.

1. **Optical ordering:** scatter emitted phosphor light, including its mask;
   make material response independent of drawable resolution. Check black,
   uniform-field energy, resolved grille contrast and resize invariance.
2. **Temporal emission and beam:** support more than one decay timescale;
   check impulse decay, constant-field energy, history reset and frame skips.
   Retain published-versus-estimated distinctions for coefficients.
3. **RF receiver:** add an asymmetric complex IF response before envelope
   detection. Check identity, constant levels, sideband response and noise
   continuity. Do not mistake a local console modulator for a broadcast VSB
   transmitter or claim that carrier-frequency metadata is a tuned circuit.
4. **VHS:** add a separate, optional NTSC recording/playback path and preset.
   Model recovered luma/chroma bandwidth and timing before CRT reception;
   preserve sync/burst and keep RF noise distinct from tape impairments.
5. **Calibration and review:** provide repeatable signal/beam patterns, inspect
   actual game captures at native output resolution and consecutive phases,
   and benchmark the complete UHD video/audio path sequentially.

Acceptance requires actual GPU output tests and usable saved controls. New
models are documented as equivalent responses where component values or
measurements are missing. Existing four tube profiles must retain their
distinct masks, resolution, color response and regulation. A separate VHS
profile does not replace those four direct-view setups.

## Completed implementation and remaining calibration

All five implementation steps are complete, with GPU invariants, ngspice
comparison, preset/OSD coverage, native game captures and full-path benchmarks
recorded in [validation](gpu-realism-validation.md). The requested TV-style
translucent menu and bottom-strip parameter editing are included; their RGB
insertion occurs after the receiver and before the tube.

Physical calibration remains open: unit-specific beam profiles, phosphor
spectra/decay, RF IF measurements and VCR response need measured reference data.
Published specifications and generic circuits do not establish those values.

## Receiver-specific follow-up

The [sharpening and interference worklist](https://yaglo.github.io/mynes-web/research/sharpening/) tracks
the 21 September request for correct per-monitor circuitry and the linked NES
squiggly-lines fault. Completion of the five earlier steps does not complete
that newer work: chip-specific sharpening/decoder responses and console
power-board faults remain open. The first correction separates luma extraction
from sharpening so the latter cannot bypass the former's rejection.
