# Adding Features

## Adding a Mapper

### 1. Create the Implementation File

Create `src/nes/mappers/mapper_XXXX.c`. Implement the 5 `MapperOps` functions
and export a `const MapperOps` struct. Use an existing mapper as a template.

Minimal example (mapper 0 style):

```c
#include "mapper_ops.h"

static void mapperXX_init(Mapper *m) {
    m->prg_bank0 = 0;
    m->prg_bank1 = (m->prg_banks > 1) ? 1 : 0;
    m->prg_ram_enabled = true;
}

static uint8_t mapperXX_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t bank = (addr < 0xC000) ? m->prg_bank0 : m->prg_bank1;
        uint32_t offset = (addr & 0x3FFF) + (bank * 0x4000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000 && m->prg_ram_enabled) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapperXX_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        // Bank switching logic here
    } else if (addr >= 0x6000 && addr < 0x8000) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t mapperXX_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        if (m->has_chr_ram) return m->chr_ram[addr];
        return m->chr_rom[addr % m->chr_rom_size];
    }
    return 0;
}

static void mapperXX_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[addr] = val;
    }
}

const MapperOps mapperXX_ops = {
    .init      = mapperXX_init,
    .cpu_read  = mapperXX_cpu_read,
    .cpu_write = mapperXX_cpu_write,
    .ppu_read  = mapperXX_ppu_read,
    .ppu_write = mapperXX_ppu_write,
};
```

### 2. Register the MapperOps

In `src/nes/mappers/mapper_ops.h`, add the extern declaration:

```c
extern const MapperOps mapperXX_ops;
```

### 3. Wire into the Dispatcher

In `src/nes/mapper.c`, add to `mapper_ops_for()`:

```c
case XX: return &mapperXX_ops;
```

And update `mapper_supported()`:

```c
bool mapper_supported(uint8_t number) {
    return number == 0 || number == 1 || ... || number == XX;
}
```

### 4. Update the ROM Loader

In `src/nes/rom.h`, update the supported mapper check in `nes_rom_load()`:

```c
if (rom->mapper > 4 && rom->mapper != 7 && rom->mapper != 10
    && rom->mapper != XX) {
    return ROM_ERR_MAPPER;
}
```

### 5. Add to CMakeLists.txt

Add the source file to the `nes_static` library:

```cmake
add_library(nes_static STATIC
    ...
    src/nes/mappers/mapper_XXXX.c
)
```

### 6. Add Mapper-Specific State (if needed)

If the mapper needs state beyond the common fields, add prefixed fields to
the `Mapper` struct in `src/nes/mapper.h`:

```c
typedef struct Mapper {
    ...
    /* MapperXX specific */
    uint8_t mapperXX_bank_select;
    uint8_t mapperXX_banks[8];
    bool mapperXX_irq_enabled;
    ...
} Mapper;
```

## Adding a Hook

### 1. Define the Callback Type

In `src/nes/hooks.h`, add a new typedef:

```c
typedef void (*hook_my_event_fn)(/* parameters */);
```

### 2. Add to DebugHooks

```c
typedef struct {
    ...
    hook_my_event_fn on_my_event;
} DebugHooks;
```

### 3. Create the Dispatch Macro

```c
#define HOOK_MY_EVENT(args...) \
    do { if (debug_hooks.on_my_event) debug_hooks.on_my_event(args); } while(0)
```

### 4. Insert the Macro at the Right Point

In the component code (e.g., `nes.h`, `ppu.h`, `apu.h`), call the macro
at the appropriate location:

```c
HOOK_MY_EVENT(param1, param2);
```

### 5. Add No-Op Fallback (for standalone headers)

If the macro is used in a header that might be compiled without `hooks.h`:

```c
#ifndef HOOK_MY_EVENT
#define HOOK_MY_EVENT(...) ((void)0)
#endif
```

### 6. Add Trace Support (optional)

In `src/nes/trace.c`, add a handler function and wire it in
`trace_install_hooks()`:

```c
static void trace_on_my_event(/* params */) {
    TRACE(TRACE_MY_CAT, "[TAG] format...\n", /* args */);
}

void trace_install_hooks(void) {
    ...
    if (trace_enabled & TRACE_MY_CAT)
        debug_hooks.on_my_event = trace_on_my_event;
}
```

## Adding a Trace Category

### 1. Define the Category Bit

In `src/nes/trace.h`, add to the `TraceCategory` enum:

```c
typedef enum {
    ...
    TRACE_MY_CAT = 1 << 12,  // Next available bit
    TRACE_ALL    = 0xFFFF     // May need to widen if > 16 categories
} TraceCategory;
```

### 2. Register in the Category Table

In `src/nes/trace.c`, add to `category_table`:

```c
static const TraceCategoryInfo category_table[] = {
    ...
    {"mycat", TRACE_MY_CAT},
    ...
};
```

### 3. Write the Trace Handler

In `src/nes/trace.c`:

```c
static void trace_on_my_event(/* hook params */) {
    TRACE(TRACE_MY_CAT, "[MYCAT] details...\n", /* args */);
}
```

### 4. Install in trace_install_hooks()

```c
if (trace_enabled & TRACE_MY_CAT)
    debug_hooks.on_my_event = trace_on_my_event;
```

### 5. Clean up in trace_remove_hooks()

```c
debug_hooks.on_my_event = NULL;
```

The new category is now available via CLI (`--trace mycat`) and the
programmatic API (`trace_parse_categories("mycat")`).

## Adding a New Test

### Unit Test

1. Create a test file (e.g., `tests/ppu/test_new_feature.c`).
2. Add to `CMakeLists.txt`:
   ```cmake
   add_executable(test_new_feature tests/ppu/test_new_feature.c)
   target_link_libraries(test_new_feature PRIVATE nes_static)
   add_test(NAME new_feature_tests COMMAND test_new_feature)
   ```
3. Add to the `alltests` target dependencies.

### ROM Test

1. Create a test script:
   ```
   WAIT_FRAMES 600
   DUMP_SCREEN
   STOP
   ```
2. Add to `scripts/run_all_tests.sh`:
   ```bash
   run_test "My Test" ./bin/test_runner path/to/rom.nes --blargg
   ```

## Adding a New Frontend

The public API in `include/nes/nes.h` provides everything needed:

```c
#include <nes/nes.h>

nes_t *nes = nes_create();
nes_load_rom_file(nes, "game.nes");
nes_set_audio_sample_rate(nes, 44100);

while (running) {
    nes_run_frame(nes);

    // Video: get framebuffer in your preferred format
    nes_get_framebuffer_rgba(nes, texture_data, 255);
    // Or: nes_get_framebuffer_rgb565(nes, display_buffer);

    // Audio: set callback for streaming
    nes_set_audio_callback(nes, audio_callback, user_data);

    // Input
    nes_set_controller(nes, 0, buttons);
}

nes_destroy(nes);
```

Supported framebuffer formats: RGB888, RGBA8888, BGRA8888, RGB565.

## Related Files

- `src/nes/mappers/mapper_ops.h` -- MapperOps vtable
- `src/nes/mapper.h` -- Mapper struct
- `src/nes/mapper.c` -- Mapper dispatcher
- `src/nes/hooks.h` -- Hook system
- `src/nes/trace.h` -- Trace categories
- `src/nes/trace.c` -- Trace implementation
- `include/nes/nes.h` -- Public API
- `CMakeLists.txt` -- Build system
