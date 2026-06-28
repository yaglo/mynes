# Coding Conventions

## Language and Standard

- **C11** (`-std=c11`) with no compiler extensions required.
- No platform dependencies in the core library -- only `<stdint.h>`,
  `<stdbool.h>`, `<stddef.h>`, `<string.h>`, `<stdlib.h>`, `<math.h>`.

## Naming Conventions

### Types

Types use PascalCase:

```c
typedef struct PPU { ... } PPU;
typedef struct APU_Pulse { ... } APU_Pulse;
typedef struct CPUSnapshot { ... } CPUSnapshot;
typedef struct MapperOps { ... } MapperOps;
```

### Constants and Macros

Constants and macros use ALL_CAPS with underscores:

```c
#define PPU_WIDTH          256
#define CTRL_NMI_ENABLE    0x80
#define TRACE_CPU_INSTR    (1 << 0)
#define NES_EXIT_PASS      0
#define HP_ALPHA_90HZ      0.996863
```

### Functions

Functions use snake_case with a component prefix:

```c
void ppu_init(PPU *ppu);
void ppu_step(PPU *ppu);
uint8_t ppu_reg_read(PPU *ppu, uint16_t addr);

void apu_step(APU *apu);
float apu_mix_sample(APU *apu);

void mapper_init(Mapper *m, ...);
uint8_t mapper_cpu_read(Mapper *m, uint16_t addr);

void debug_get_cpu_snapshot(const NES *nes, CPUSnapshot *out);
uint8_t debug_read_cpu(const NES *nes, uint16_t addr);

void trace_init(void);
uint32_t trace_parse_categories(const char *str);

void hooks_init(void);
void hooks_clear(void);
```

### Hook Macros

Hook dispatch macros use HOOK_ prefix with ALL_CAPS:

```c
HOOK_CPU_STEP(pc, op, cyc)
HOOK_MEM_ACCESS(addr, val, is_write)
HOOK_PPU_SCANLINE(scanline, frame)
```

### Private/Internal Functions

File-scoped functions use `static` with descriptive names:

```c
static void mapper1_shift_write(Mapper *m, uint16_t addr, uint8_t val);
static void trace_on_cpu_step(uint16_t pc, uint8_t opcode, uint64_t cycles);
```

## Header-Only Implementation

The performance-critical core (PPU, APU, NES system integration) is
implemented as `static inline` functions in headers:

```c
// In ppu.h:
static inline void ppu_step(PPU *ppu) { ... }
static inline uint8_t ppu_reg_read(PPU *ppu, uint16_t addr) { ... }

// In apu.h:
static inline void apu_step(APU *apu) { ... }
static inline float apu_mix_sample(APU *apu) { ... }

// In nes.h:
static inline void nes_step(NES *nes) { ... }
static inline uint8_t nes_cpu_read(CPU *cpu, uint16_t addr) { ... }
```

This allows the compiler to inline across the entire hot loop. The trade-off
is that `nes.h` `#include`s the generated CPU code and other headers,
creating a single compilation unit for the core.

Separately-compiled files (`.c` files) are used for:
- Debug utilities (`debug.c`, `cpu_trace.c`)
- Trace system (`trace.c`)
- Hook state (`hooks.c`)
- Screen dump (`screen_dump.c`)
- Script parser (`script.c`)
- Mapper dispatch and implementations (`mapper.c`, `mappers/*.c`)

## Hook Macros Pattern

Hooks use a consistent pattern: a do-while(0) macro that checks for NULL:

```c
#define HOOK_XXX(args...) \
    do { if (debug_hooks.on_xxx) debug_hooks.on_xxx(args); } while(0)
```

Components that might be compiled standalone provide no-op fallbacks:

```c
// In ppu.h:
#ifndef HOOK_PPU_REG
#define HOOK_PPU_REG(addr, val, is_write) ((void)0)
#endif
```

## Struct Layout

Component state structs group related fields with comments:

```c
typedef struct PPU {
    /* Rendering position */
    uint16_t dot;
    uint16_t scanline;
    uint64_t frame;

    /* Internal registers (loopy) */
    uint16_t v, t;
    uint8_t x;
    bool w;

    /* External registers */
    uint8_t ctrl, mask, status, oam_addr;

    /* Memory */
    uint8_t vram[PPU_VRAM_SIZE];
    uint8_t palette[PPU_PALETTE_SIZE];

    /* Callbacks */
    uint8_t (*cart_read)(struct PPU *ppu, uint16_t addr);
    void *user_data;
} PPU;
```

## Const Correctness

Read-only parameters and snapshot functions use `const`:

```c
void debug_get_cpu_snapshot(const struct NES *nes, CPUSnapshot *out);
uint8_t debug_read_cpu(const struct NES *nes, uint16_t addr);
const char *debug_format_cpu_flags(uint8_t p);
```

## Error Handling

- ROM loading returns error codes (enum values):
  ```c
  typedef enum nes_error { NES_OK = 0, NES_ERROR_INVALID_ROM, ... } nes_error_t;
  ```
- Test runner uses exit codes: 0=pass, 1=fail, 2=timeout, 3=ROM error.
- Internal functions use NULL checks with early return.

## Comment Style

- File headers use `/* ... */` block comments with a brief description.
- Section headers use boxed `/* ==== */` separators.
- Inline comments use `//` for brief notes or `/* ... */` for explanations.
- NES hardware behavior references nesdev wiki terminology.

```c
/* ============================================================================
 * Rendering Helpers
 * ============================================================================ */

/* Increment coarse X (with wrapping/nametable switch) */
static inline void ppu_inc_x(PPU *ppu) {
    if ((ppu->v & 0x001F) == 31) {
        ppu->v &= ~0x001F;      /* Coarse X = 0 */
        ppu->v ^= 0x0400;       /* Switch horizontal nametable */
    } else {
        ppu->v++;               /* Increment coarse X */
    }
}
```

## Lookup Tables

Constant data uses `static const` arrays at file scope:

```c
static const uint8_t apu_length_table[32] = { 10, 254, 20, 2, ... };
static const uint8_t apu_duty_table[4][8] = { ... };
static const uint8_t ppu_palette_2c02[64][3] = { ... };
```
