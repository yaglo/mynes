/*
 * NES Debug Hooks - Zero-overhead event callbacks
 *
 * Hook callbacks are called at critical points in the emulation.
 * When a hook is NULL, the cost is a single pointer comparison.
 */

#ifndef NES_HOOKS_H
#define NES_HOOKS_H

#include <stdint.h>
#include <stdbool.h>

/* ============================================================================
 * Hook Callback Types
 * ============================================================================ */

typedef void (*hook_cpu_step_fn)(uint16_t pc, uint8_t opcode, uint64_t cycles);
typedef void (*hook_cpu_irq_fn)(bool is_nmi, uint16_t vector_addr, uint16_t return_addr);
typedef void (*hook_mem_access_fn)(uint16_t addr, uint8_t value, bool is_write);
typedef void (*hook_ppu_scanline_fn)(int scanline, uint64_t frame);
typedef void (*hook_ppu_vblank_fn)(bool entering, uint64_t cpu_cycle);
typedef void (*hook_ppu_reg_fn)(uint16_t addr, uint8_t value, bool is_write);
typedef void (*hook_apu_frame_fn)(int step, bool quarter, bool half, bool irq);
typedef void (*hook_apu_reg_fn)(uint16_t addr, uint8_t value);
typedef void (*hook_frame_fn)(uint64_t frame_number, uint64_t cpu_cycles);

/* ============================================================================
 * Global Hooks Structure
 * ============================================================================ */

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

extern DebugHooks debug_hooks;

/* ============================================================================
 * Hook Macros - zero overhead when callback is NULL
 * ============================================================================ */

#define HOOK_CPU_STEP(pc, op, cyc) \
    do { if (debug_hooks.on_cpu_step) debug_hooks.on_cpu_step(pc, op, cyc); } while(0)

#define HOOK_CPU_IRQ(is_nmi, vec, ret) \
    do { if (debug_hooks.on_cpu_irq) debug_hooks.on_cpu_irq(is_nmi, vec, ret); } while(0)

#define HOOK_MEM_ACCESS(addr, val, is_write) \
    do { if (debug_hooks.on_mem_access) debug_hooks.on_mem_access(addr, val, is_write); } while(0)

#define HOOK_PPU_SCANLINE(scanline, frame) \
    do { if (debug_hooks.on_ppu_scanline) debug_hooks.on_ppu_scanline(scanline, frame); } while(0)

#define HOOK_PPU_VBLANK(entering, cycle) \
    do { if (debug_hooks.on_ppu_vblank) debug_hooks.on_ppu_vblank(entering, cycle); } while(0)

#define HOOK_PPU_REG(addr, val, is_write) \
    do { if (debug_hooks.on_ppu_reg) debug_hooks.on_ppu_reg(addr, val, is_write); } while(0)

#define HOOK_APU_FRAME(step, quarter, half, irq) \
    do { if (debug_hooks.on_apu_frame) debug_hooks.on_apu_frame(step, quarter, half, irq); } while(0)

#define HOOK_APU_REG(addr, val) \
    do { if (debug_hooks.on_apu_reg) debug_hooks.on_apu_reg(addr, val); } while(0)

#define HOOK_FRAME(frame_num, cycles) \
    do { if (debug_hooks.on_frame) debug_hooks.on_frame(frame_num, cycles); } while(0)

/* ============================================================================
 * Control Functions
 * ============================================================================ */

void hooks_init(void);
void hooks_clear(void);

#endif /* NES_HOOKS_H */
