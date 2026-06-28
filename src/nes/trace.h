/*
 * NES Trace System
 *
 * Runtime-configurable trace categories with memory range filtering.
 * Uses the hook system for delivery — trace handlers register as hook callbacks.
 */

#ifndef NES_TRACE_H
#define NES_TRACE_H

#include <stdint.h>
#include <stdarg.h>

/* ============================================================================
 * Trace Categories (bitmask)
 * ============================================================================ */

typedef enum {
    TRACE_NONE       = 0,
    TRACE_CPU_INSTR  = 1 << 0,   /* CPU instruction (PC, opcode, regs) */
    TRACE_CPU_DETAIL = 1 << 1,   /* CPU instruction (disassembly) */
    TRACE_CPU_IRQ    = 1 << 2,   /* IRQ/NMI events */
    TRACE_PPU_REG    = 1 << 3,   /* PPU register reads/writes */
    TRACE_PPU_SCAN   = 1 << 4,   /* Scanline/dot events */
    TRACE_PPU_VRAM   = 1 << 5,   /* VRAM reads/writes */
    TRACE_APU_REG    = 1 << 6,   /* APU register writes */
    TRACE_APU_FRAME  = 1 << 7,   /* Frame counter events */
    TRACE_APU_IRQ    = 1 << 8,   /* APU IRQ events */
    TRACE_MEM_READ   = 1 << 9,   /* Memory reads */
    TRACE_MEM_WRITE  = 1 << 10,  /* Memory writes */
    TRACE_TIMING     = 1 << 11,  /* CPU/PPU cycle counts */
    TRACE_ALL        = 0xFFFF
} TraceCategory;

/* ============================================================================
 * Trace Output
 * ============================================================================ */

typedef void (*trace_output_fn)(const char *fmt, ...);

extern uint32_t trace_enabled;
extern trace_output_fn trace_output;
extern uint16_t trace_mem_start;
extern uint16_t trace_mem_end;

/* ============================================================================
 * Trace Macros
 * ============================================================================ */

#define TRACE(cat, fmt, ...) \
    do { if ((trace_enabled & (cat)) && trace_output) \
        trace_output(fmt, ##__VA_ARGS__); } while(0)

#define TRACE_IF(cat, cond, fmt, ...) \
    do { if ((trace_enabled & (cat)) && (cond) && trace_output) \
        trace_output(fmt, ##__VA_ARGS__); } while(0)

#define TRACE_MEM(cat, addr, fmt, ...) \
    TRACE_IF(cat, (addr) >= trace_mem_start && (addr) <= trace_mem_end, \
             fmt, ##__VA_ARGS__)

/* ============================================================================
 * Control API
 * ============================================================================ */

void trace_init(void);
void trace_set_output(trace_output_fn fn);
void trace_enable(TraceCategory cat);
void trace_disable(TraceCategory cat);
void trace_set_categories(uint32_t mask);
uint32_t trace_get_categories(void);
void trace_set_mem_range(uint16_t start, uint16_t end);

/* Parse comma-separated category string: "cpu,ppu,mem" or "all"
 * Returns bitmask of matched categories. */
uint32_t trace_parse_categories(const char *str);

/* Get display name for a category */
const char *trace_category_name(TraceCategory cat);

/* Install/remove trace handlers as hook callbacks.
 * Call after enabling categories. */
void trace_install_hooks(void);
void trace_remove_hooks(void);

#endif /* NES_TRACE_H */
