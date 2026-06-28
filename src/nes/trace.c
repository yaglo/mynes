#include "nes/trace.h"
#include "nes/hooks.h"
#include "nes/cpu_trace.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* ============================================================================
 * Global State
 * ============================================================================ */

uint32_t trace_enabled = 0;
trace_output_fn trace_output = NULL;
uint16_t trace_mem_start = 0x0000;
uint16_t trace_mem_end = 0xFFFF;

/* ============================================================================
 * Default Output (printf)
 * ============================================================================ */

static void trace_printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/* ============================================================================
 * Control API
 * ============================================================================ */

void trace_init(void) {
    trace_enabled = 0;
    trace_output = trace_printf;
    trace_mem_start = 0x0000;
    trace_mem_end = 0xFFFF;
}

void trace_set_output(trace_output_fn fn) {
    trace_output = fn ? fn : trace_printf;
}

void trace_enable(TraceCategory cat) {
    trace_enabled |= cat;
}

void trace_disable(TraceCategory cat) {
    trace_enabled &= ~cat;
}

void trace_set_categories(uint32_t mask) {
    trace_enabled = mask;
}

uint32_t trace_get_categories(void) {
    return trace_enabled;
}

void trace_set_mem_range(uint16_t start, uint16_t end) {
    trace_mem_start = start;
    trace_mem_end = end;
}

/* ============================================================================
 * Category Name Table
 * ============================================================================ */

typedef struct {
    const char *name;
    uint32_t mask;
} TraceCategoryInfo;

static const TraceCategoryInfo category_table[] = {
    {"cpu",       TRACE_CPU_INSTR},
    {"cpudetail", TRACE_CPU_DETAIL},
    {"irq",       TRACE_CPU_IRQ},
    {"ppureg",    TRACE_PPU_REG},
    {"ppuscan",   TRACE_PPU_SCAN},
    {"vram",      TRACE_PPU_VRAM},
    {"apureg",    TRACE_APU_REG},
    {"apuframe",  TRACE_APU_FRAME},
    {"apuirq",    TRACE_APU_IRQ},
    {"memr",      TRACE_MEM_READ},
    {"memw",      TRACE_MEM_WRITE},
    {"timing",    TRACE_TIMING},
    /* Convenience aliases */
    {"ppu",       TRACE_PPU_REG | TRACE_PPU_SCAN | TRACE_PPU_VRAM},
    {"apu",       TRACE_APU_REG | TRACE_APU_FRAME | TRACE_APU_IRQ},
    {"mem",       TRACE_MEM_READ | TRACE_MEM_WRITE},
    {"all",       TRACE_ALL},
    {NULL, 0}
};

uint32_t trace_parse_categories(const char *str) {
    uint32_t mask = 0;
    char buf[256];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, ",|+ ");
    while (tok) {
        for (const TraceCategoryInfo *ci = category_table; ci->name; ci++) {
            if (strcmp(tok, ci->name) == 0) {
                mask |= ci->mask;
                break;
            }
        }
        tok = strtok(NULL, ",|+ ");
    }
    return mask;
}

const char *trace_category_name(TraceCategory cat) {
    for (const TraceCategoryInfo *ci = category_table; ci->name; ci++) {
        if (ci->mask == (uint32_t)cat)
            return ci->name;
    }
    return "unknown";
}

/* ============================================================================
 * Hook Handlers
 * ============================================================================ */

static void trace_on_cpu_step(uint16_t pc, uint8_t opcode, uint64_t cycles) {
    TRACE(TRACE_CPU_INSTR, "[CPU] PC=$%04X OP=$%02X CYC=%llu\n",
          pc, opcode, (unsigned long long)cycles);
}

static void trace_on_cpu_irq(bool is_nmi, uint16_t vec, uint16_t ret) {
    TRACE(TRACE_CPU_IRQ, "[IRQ] %s VEC=$%04X RET=$%04X\n",
          is_nmi ? "NMI" : "IRQ", vec, ret);
}

static void trace_on_mem_access(uint16_t addr, uint8_t val, bool is_write) {
    if (is_write) {
        TRACE_MEM(TRACE_MEM_WRITE, addr, "[MEM] W $%04X = $%02X\n", addr, val);
    } else {
        TRACE_MEM(TRACE_MEM_READ, addr, "[MEM] R $%04X = $%02X\n", addr, val);
    }
}

static void trace_on_ppu_scanline(int scanline, uint64_t frame) {
    TRACE(TRACE_PPU_SCAN, "[PPU] Scanline %d frame %llu\n",
          scanline, (unsigned long long)frame);
}

static void trace_on_ppu_vblank(bool entering, uint64_t cycle) {
    TRACE(TRACE_PPU_SCAN, "[PPU] VBlank %s\n", entering ? "START" : "END");
    (void)cycle;
}

static void trace_on_ppu_reg(uint16_t addr, uint8_t val, bool is_write) {
    TRACE(TRACE_PPU_REG, "[PPU] %s $%04X = $%02X\n",
          is_write ? "W" : "R", addr, val);
}

static void trace_on_apu_frame(int step, bool quarter, bool half, bool irq) {
    TRACE(TRACE_APU_FRAME, "[APU] Frame step=%d Q=%d H=%d IRQ=%d\n",
          step, quarter, half, irq);
}

static void trace_on_apu_reg(uint16_t addr, uint8_t val) {
    TRACE(TRACE_APU_REG, "[APU] W $%04X = $%02X\n", addr, val);
}

static void trace_on_frame(uint64_t frame, uint64_t cycles) {
    TRACE(TRACE_TIMING, "[FRAME] #%llu cycles=%llu\n",
          (unsigned long long)frame, (unsigned long long)cycles);
}

/* ============================================================================
 * Hook Installation
 * ============================================================================ */

void trace_install_hooks(void) {
    if (trace_enabled & TRACE_CPU_DETAIL)
        cpu_trace_install_hook();  /* Full disassembly trace */
    else if (trace_enabled & TRACE_CPU_INSTR)
        debug_hooks.on_cpu_step = trace_on_cpu_step;  /* Simple one-liner */
    if (trace_enabled & TRACE_CPU_IRQ)
        debug_hooks.on_cpu_irq = trace_on_cpu_irq;
    if (trace_enabled & (TRACE_MEM_READ | TRACE_MEM_WRITE))
        debug_hooks.on_mem_access = trace_on_mem_access;
    if (trace_enabled & TRACE_PPU_SCAN)
        debug_hooks.on_ppu_scanline = trace_on_ppu_scanline;
    if (trace_enabled & TRACE_PPU_SCAN)
        debug_hooks.on_ppu_vblank = trace_on_ppu_vblank;
    if (trace_enabled & TRACE_PPU_REG)
        debug_hooks.on_ppu_reg = trace_on_ppu_reg;
    if (trace_enabled & TRACE_APU_FRAME)
        debug_hooks.on_apu_frame = trace_on_apu_frame;
    if (trace_enabled & TRACE_APU_REG)
        debug_hooks.on_apu_reg = trace_on_apu_reg;
    if (trace_enabled & TRACE_TIMING)
        debug_hooks.on_frame = trace_on_frame;
}

void trace_remove_hooks(void) {
    debug_hooks.on_cpu_step = NULL;
    debug_hooks.on_cpu_irq = NULL;
    debug_hooks.on_mem_access = NULL;
    debug_hooks.on_ppu_scanline = NULL;
    debug_hooks.on_ppu_vblank = NULL;
    debug_hooks.on_ppu_reg = NULL;
    debug_hooks.on_apu_frame = NULL;
    debug_hooks.on_apu_reg = NULL;
    debug_hooks.on_frame = NULL;
}
