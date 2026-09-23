# Architecture Overview

## What This Is

A cycle-accurate NES emulator written in C11. The CPU is generated from a
Scheme DSL, the PPU runs dot-by-dot, and the APU produces filtered audio at
44.1 kHz. The library has zero platform dependencies -- frontends supply
video/audio/input via callbacks.

## Component Diagram

```
 +-----------+     +-----------+     +-----------+
 |   CPU     |     |   PPU     |     |   APU     |
 | (cpu_gen) |     | (ppu.h)   |     | (apu.h)   |
 +-----+-----+     +-----+-----+     +-----+-----+
       |                 |                 |
       v                 v                 v
 +---------------------------------------------------+
 |                    NES (nes.h)                     |
 |  - Memory mapping    - DMA controller              |
 |  - Clock scheduler   - NMI edge detection          |
 |  - Controller I/O    - Hook dispatch               |
 +---------------------------------------------------+
       |                 |                 |
       v                 v                 v
 +-----------+     +-----------+     +-----------+
 |  Mapper   |     |  Debug    |     |  Trace    |
 | (mapper.h)|     | (debug.h) |     | (trace.h) |
 +-----------+     +-----------+     +-----------+
```

## Data Flow

1. `nes_step()` is the master clock. Each call advances one CPU cycle.
2. APU steps first (so IRQ is visible to CPU on the same cycle).
3. DMC DMA check -- steals 3-4 CPU cycles if the DMC sample buffer is empty.
4. OAM DMA check -- steals 513-514 CPU cycles if $4014 was written.
5. PPU steps 3 times per CPU cycle (1:3 clock ratio on NTSC).
6. CPU steps once (microcode state machine advances by one micro-op).
7. NMI edge detection runs after PPU, with delay logic for correct timing.

## Key Design Decisions

**Header-only core.** The PPU (`ppu.h`), APU (`apu.h`), and NES system
(`nes.h`) are implemented entirely in `static inline` functions inside headers.
This lets the compiler inline the hot loop and enables link-time optimization
across the entire emulation core. Separately-compiled files (`debug.c`,
`trace.c`, `mapper.c`) are reserved for debugging and mapper dispatch.

**DSL-generated CPU.** The 6502 is defined in `src/cpu/nes6502.dsl`, a
microcode description language compiled to C by `tools/dsl2c.scm` (Chicken
Scheme). This produces `generated/cpu_gen.c`, a single `cpu_step()` function
with a `switch(uPC)` state machine. Each case is one CPU sub-cycle.

**Zero-overhead hooks.** The debug hook system (`hooks.h`) uses function
pointers checked with a NULL test macro. When no debugger is attached, the
cost is one branch per hook site (predicted not-taken).

**Mapper vtable.** Each mapper implements a `MapperOps` struct with 5 function
pointers (`init`, `cpu_read`, `cpu_write`, `ppu_read`, `ppu_write`).
The dispatcher in `mapper.c` resolves the vtable by mapper number.

## Source Layout

```
src/
  cpu/nes6502.dsl          DSL microcode source
  cpu/nes6502.c            Hand-written CPU (legacy, not used when DSL builds)
  ppu/ppu.h                Dot-accurate PPU (header-only)
  nes/nes.h                System integration, memory map, DMA, clock
  nes/apu.h                APU audio synthesis (header-only)
  nes/hooks.h              Event hook system
  nes/debug.h + debug.c    Side-effect-free state inspection
  nes/trace.h + trace.c    Configurable trace logging
  nes/cpu_trace.h + .c     Nestest-compatible disassembly
  nes/mapper.h + mapper.c  Mapper dispatcher
  nes/mappers/             Per-mapper implementations
  nes/blargg.h             Blargg test auto-detection
  nes/screen_dump.h + .c   Nametable-to-ASCII conversion
  nes/script.h + .c        Test scripting engine
  nes/screenshot.h         PPM screenshot utility
  nes/debug_chr.h          CHR tile debug renderer
  nes/rom.h                iNES ROM parser
  nes/exit_codes.h         CI exit codes
tools/
  dsl2c.scm                DSL-to-C compiler (Chicken Scheme)
  test_runner.c            Headless test execution with Blargg, scripting, traces
frontends/
  sdl/main.c               SDL2 desktop frontend
examples/
  headless/main.c          Minimal headless ROM runner
```

## Build Targets

| Target | Description |
|--------|-------------|
| `nes_static` | Core static library (links all mappers, hooks, debug, trace) |
| `test_cpu` | CPU instruction correctness tests |
| `test_cpu_cycles` | CPU cycle count tests |
| `test_ppu` | PPU rendering tests |
| `test_nes` | NES integration tests |
| `test_rom` | ROM loading tests |
| `accuracy_coin` | AccuracyCoin accuracy suite |
| `test_runner` | Headless test runner (Blargg, scripts, traces) |
| `nes_sdl` | SDL2 frontend (optional, requires SDL2) |
| `run_rom` | Simple headless ROM runner |

## Driving the Core

Frontends include `nes/rom.h` and `nes/nes.h` from `src/` and link
`nes_static`. `frontends/headless/main.c` is the smallest complete example:

```c
static ROM rom;
static NES nes;

nes_rom_load(&rom, "game.nes");
nes_init(&nes);
nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size,
                rom.chr_rom, rom.chr_size, rom.mirroring);
nes_reset(&nes);

while (running) {
    nes_set_controller(&nes, 0, buttons);
    nes_run_frame(&nes);
    display(nes.ppu.framebuffer);   /* 256x240 RGB888 */
}

nes_rom_free(&rom);
```

`nes.ppu.index_framebuffer` holds the palette index and emphasis bits of
each pixel for the composite video paths. Audio samples arrive through
`apu_set_audio_callback()`, and `src/nes/state.h` saves and loads states.
