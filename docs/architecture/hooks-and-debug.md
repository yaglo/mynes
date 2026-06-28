# Hooks and Debug Infrastructure

## Hook System

The hook system (`src/nes/hooks.h`) provides 9 event callbacks that fire at
critical points during emulation. When a hook is NULL, the cost is a single
branch instruction (predicted not-taken).

### Architecture

```c
typedef struct {
    hook_cpu_step_fn     on_cpu_step;
    hook_cpu_irq_fn      on_cpu_irq;
    hook_mem_access_fn   on_mem_access;
    hook_ppu_scanline_fn on_ppu_scanline;
    hook_ppu_vblank_fn   on_ppu_vblank;
    hook_ppu_reg_fn      on_ppu_reg;
    hook_apu_frame_fn    on_apu_frame;
    hook_apu_reg_fn      on_apu_reg;
    hook_frame_fn        on_frame;
} DebugHooks;

extern DebugHooks debug_hooks;  // Single global instance
```

### Zero-Overhead Macros

Each hook site uses a macro that checks for NULL before calling:

```c
#define HOOK_CPU_STEP(pc, op, cyc) \
    do { if (debug_hooks.on_cpu_step) debug_hooks.on_cpu_step(pc, op, cyc); } while(0)
```

Components include `hooks.h` and use these macros at the appropriate points.
The PPU and APU headers provide no-op fallback definitions for standalone
compilation.

### Hook Installation

```c
hooks_init();                              // Zero all hooks
debug_hooks.on_cpu_step = my_handler;      // Install one hook
// ... run emulation ...
hooks_clear();                             // Remove all hooks
```

The trace system (`trace.c`) installs its own handlers via
`trace_install_hooks()`.

## Debug Snapshots

The debug API (`src/nes/debug.h`) provides side-effect-free state inspection
through snapshot structs:

### CPUSnapshot

```c
typedef struct {
    uint16_t PC;
    uint8_t A, X, Y, SP, P;
    uint8_t IR;             // Current opcode
    uint16_t uPC;           // Microcode position (0 = instruction boundary)
    uint64_t cycles;
    bool irq_pending, nmi_pending, reset_pending;
} CPUSnapshot;
```

### PPUSnapshot

```c
typedef struct {
    int scanline, dot;
    uint16_t v, t;          // Loopy registers
    uint8_t fine_x;
    bool w;                 // Write toggle
    uint8_t ctrl, mask, status, oam_addr;
    bool in_vblank, sprite0_hit, sprite_overflow;
    uint64_t frame_count;
} PPUSnapshot;
```

### APUSnapshot

```c
typedef struct {
    int frame_counter_cycle;
    bool frame_irq_flag, frame_irq_inhibit, five_step_mode;
    bool dmc_irq_flag;
    APUChannelSnapshot pulse1, pulse2, triangle, noise;
    struct { bool enabled; int bytes_remaining; uint8_t output_level; } dmc;
} APUSnapshot;
```

### Safe Memory Reads

The debug API provides reads that skip I/O register side effects:

```c
uint8_t debug_read_cpu(const NES *nes, uint16_t addr);     // Returns 0 for $2000-$401F
uint16_t debug_read_cpu_word(const NES *nes, uint16_t addr);
uint8_t debug_read_ppu_vram(const NES *nes, uint16_t addr); // Via mapper for CHR
uint8_t debug_read_oam(const NES *nes, uint8_t index);
```

These are used by the CPU trace formatter to read instruction operands
without corrupting PPU state.

### Flag Formatting

```c
const char *debug_format_cpu_flags(uint8_t p);
// Returns "NV-BDIZC" with set flags uppercase, clear flags lowercase
// Example: "Nv-bdIzc" means N=1, I=1, all others clear
```

## CHR Debug Renderer

`src/nes/debug_chr.h` renders both pattern tables as a 256x128 grayscale
image:

```c
uint8_t buf[256 * 128 * 3];
debug_chr_render(nes, buf);
debug_chr_save_ppm(nes, "chr_debug.ppm");
```

The left half is pattern table 0 ($0000-$0FFF), the right half is pattern
table 1 ($1000-$1FFF). Each tile is 8x8 pixels with 4 grayscale levels.

## Screen Dump

`src/nes/screen_dump.h` converts the first nametable (32x30 tiles) to ASCII
using a configurable character map:

```c
CharMap cm;
charmap_set_default(&cm);   // AccuracyCoin-compatible layout
screen_dump_print(&ppu, &cm);
```

Default mapping: tiles $00-$09 -> '0'-'9', $0A-$23 -> 'A'-'Z', $24 -> ' '.
Custom maps can be loaded from files.

## Screenshot Utility

`src/nes/screenshot.h` saves the PPU framebuffer as a PPM image:

```c
screenshot_save_ppm(ppu, "frame.ppm");  // 256x240 RGB
```

## Related Files

- `src/nes/hooks.h` + `hooks.c` -- hook definitions and global instance
- `src/nes/debug.h` + `debug.c` -- snapshots and safe reads
- `src/nes/debug_chr.h` -- CHR tile visualization
- `src/nes/screen_dump.h` + `screen_dump.c` -- nametable-to-text
- `src/nes/screenshot.h` -- PPM screenshot
