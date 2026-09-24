# PAL-CRT (vendored)

Source: https://github.com/LMP88959/PAL-CRT
Version: v1.2.2 (pal_core.h `PAL_MAJOR/MINOR/PATCH`)
Author: EMMIR (2018-2023)
License: Permissive custom (see `LICENSE`). Attribution requested but not required.

These files are vendored verbatim from the upstream repo so we can use
PAL-CRT's 2C07 signal encoder + decoder as the composite pipeline for
PAL ROMs. The upstream hard-codes `PAL_SYSTEM` in `pal_core.h`; we've
switched it to `PAL_SYSTEM_NES` for our use.

## Files

- `pal_core.c` / `pal_core.h` — Core encoder/decoder. The decoder
  (`pal_demodulate`) does burst-extracted phase alignment and 1H
  delay-line V averaging, which is what makes real PAL decoding
  authentic.
- `pal_nes.c` / `pal_nes.h` — NES-specific encoder. Generates 2C07
  square waves from HardWareMan's oscilloscope-measured voltage
  levels and stamps the proper colorburst per line.
- `pal.h` — Non-NES PAL system header. Not used in NES mode but
  included for `PAL_SETTINGS` struct definition completeness.

## Integration

See `src/nes/composite.h` (the `comp_process` PAL branch) for how
this is wired into our pipeline. Short version: for PAL ROMs we
initialize a `PAL_CRT` state once, and each frame feed the NES
index framebuffer through `pal_modulate` + `pal_demodulate` to get
an RGB framebuffer, then layer our post-processing (scanline curve,
vignette, bloom) on top.

## Local changes

- `pal_core.h`: `#define PAL_SYSTEM PAL_SYSTEM_NES` (upstream default
  is `PAL_SYSTEM_PAL`).
- `pal_nes.c`: `#define SWAP_RED_GREEN_EMPHASIS_BITS 1` (upstream
  default is `0`). We feed PAL-CRT a pixel whose bit 6 comes from raw
  `PPUMASK` bit 5, which on PAL hardware is green — so PAL-CRT needs to
  swap R↔G internally to match our bit layout.
- `pal_core.c` / `pal_core.h`: the chroma-correction (Hanover) delay
  line moved from a function-static in `pal_demodulate` into
  `struct PAL_CRT`, and is cleared at the start of each field. Upstream
  shares it across instances and never resets it, so a field's first
  line averaged with the previous field's last.
