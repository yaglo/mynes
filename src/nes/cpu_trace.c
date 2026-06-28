/*
 * CPU Instruction Trace Formatter
 *
 * Produces nestest-compatible output:
 *   C000  4C F5 C5  JMP $C5F5                       A:00 X:00 Y:00 P:24 SP:FD
 */

#include "nes/cpu_trace.h"
#include "nes/cpu_opnames.h"
#include "nes/debug.h"
#include "nes/nes.h"
#include "nes/hooks.h"
#include "nes/trace.h"
#include <stdio.h>
#include <string.h>

/* NES pointer for the hook callback */
static NES *trace_nes = NULL;

int cpu_trace_format(const struct NES *nes_ptr, uint16_t pc, uint8_t opcode,
                     char *buf, int buf_size) {
    const NES *nes = nes_ptr;
    const CPU *cpu = &nes->cpu;

    uint8_t size = cpu_op_size[opcode];

    const char *name = cpu_op_name[opcode];
    if (!name) name = "???";
    CPUAddressMode mode = cpu_op_mode[opcode];

    /* Read operand bytes (side-effect-free) */
    uint8_t lo = (size >= 2) ? debug_read_cpu(nes_ptr, (pc + 1) & 0xFFFF) : 0;
    uint8_t hi = (size >= 3) ? debug_read_cpu(nes_ptr, (pc + 2) & 0xFFFF) : 0;
    uint16_t addr16 = lo | ((uint16_t)hi << 8);

    int pos = 0;

    /* Address */
    pos += snprintf(buf + pos, buf_size - pos, "%04X  ", pc);

    /* Raw bytes */
    pos += snprintf(buf + pos, buf_size - pos, "%02X ", opcode);
    if (size >= 2) pos += snprintf(buf + pos, buf_size - pos, "%02X ", lo);
    else            pos += snprintf(buf + pos, buf_size - pos, "   ");
    if (size >= 3) pos += snprintf(buf + pos, buf_size - pos, "%02X  ", hi);
    else            pos += snprintf(buf + pos, buf_size - pos, "    ");

    /* Mnemonic + operand */
    char operand[32] = "";
    switch (mode) {
    case AM_IMP:
        /* Accumulator-mode shifts show "A" */
        if (opcode == 0x0A || opcode == 0x2A || opcode == 0x4A || opcode == 0x6A)
            snprintf(operand, sizeof(operand), "A");
        break;
    case AM_IMM:
        snprintf(operand, sizeof(operand), "#$%02X", lo);
        break;
    case AM_ZP: {
        uint8_t val = debug_read_cpu(nes_ptr, lo);
        snprintf(operand, sizeof(operand), "$%02X = %02X", lo, val);
        break;
    }
    case AM_ZPX: {
        uint8_t ea = (lo + cpu->X) & 0xFF;
        uint8_t val = debug_read_cpu(nes_ptr, ea);
        snprintf(operand, sizeof(operand), "$%02X,X @ %02X = %02X", lo, ea, val);
        break;
    }
    case AM_ZPY: {
        uint8_t ea = (lo + cpu->Y) & 0xFF;
        uint8_t val = debug_read_cpu(nes_ptr, ea);
        snprintf(operand, sizeof(operand), "$%02X,Y @ %02X = %02X", lo, ea, val);
        break;
    }
    case AM_ABS:
        if (opcode == 0x4C || opcode == 0x20) {
            /* JMP / JSR — just show address */
            snprintf(operand, sizeof(operand), "$%04X", addr16);
        } else {
            uint8_t val = debug_read_cpu(nes_ptr, addr16);
            snprintf(operand, sizeof(operand), "$%04X = %02X", addr16, val);
        }
        break;
    case AM_ABX: {
        uint16_t ea = (addr16 + cpu->X) & 0xFFFF;
        uint8_t val = debug_read_cpu(nes_ptr, ea);
        snprintf(operand, sizeof(operand), "$%04X,X @ %04X = %02X", addr16, ea, val);
        break;
    }
    case AM_ABY: {
        uint16_t ea = (addr16 + cpu->Y) & 0xFFFF;
        uint8_t val = debug_read_cpu(nes_ptr, ea);
        snprintf(operand, sizeof(operand), "$%04X,Y @ %04X = %02X", addr16, ea, val);
        break;
    }
    case AM_IND: {
        /* 6502 indirect JMP page-wrap bug */
        uint16_t target;
        if ((addr16 & 0xFF) == 0xFF) {
            target = debug_read_cpu(nes_ptr, addr16) |
                     ((uint16_t)debug_read_cpu(nes_ptr, addr16 & 0xFF00) << 8);
        } else {
            target = debug_read_cpu(nes_ptr, addr16) |
                     ((uint16_t)debug_read_cpu(nes_ptr, addr16 + 1) << 8);
        }
        snprintf(operand, sizeof(operand), "($%04X) = %04X", addr16, target);
        break;
    }
    case AM_IZX: {
        uint8_t zp = (lo + cpu->X) & 0xFF;
        uint16_t ea = debug_read_cpu(nes_ptr, zp) |
                      ((uint16_t)debug_read_cpu(nes_ptr, (zp + 1) & 0xFF) << 8);
        uint8_t val = debug_read_cpu(nes_ptr, ea);
        snprintf(operand, sizeof(operand), "($%02X,X) @ %02X = %04X = %02X",
                 lo, zp, ea, val);
        break;
    }
    case AM_IZY: {
        uint16_t base = debug_read_cpu(nes_ptr, lo) |
                        ((uint16_t)debug_read_cpu(nes_ptr, (lo + 1) & 0xFF) << 8);
        uint16_t ea = (base + cpu->Y) & 0xFFFF;
        uint8_t val = debug_read_cpu(nes_ptr, ea);
        snprintf(operand, sizeof(operand), "($%02X),Y = %04X @ %04X = %02X",
                 lo, base, ea, val);
        break;
    }
    case AM_REL: {
        int8_t offset = (int8_t)lo;
        uint16_t target = (pc + 2 + offset) & 0xFFFF;
        snprintf(operand, sizeof(operand), "$%04X", target);
        break;
    }
    }

    pos += snprintf(buf + pos, buf_size - pos, "%s %-28s", name, operand);

    /* Register state */
    pos += snprintf(buf + pos, buf_size - pos,
                    "A:%02X X:%02X Y:%02X P:%02X SP:%02X",
                    cpu->A, cpu->X, cpu->Y, cpu->P, cpu->SP);

    if (pos < buf_size - 1)
        buf[pos++] = '\n';
    buf[pos] = '\0';
    return pos;
}

/* Hook callback for TRACE_CPU_DETAIL */
static void cpu_trace_hook(uint16_t pc, uint8_t opcode, uint64_t cycles) {
    (void)cycles;
    if (!trace_nes) return;

    char buf[256];
    cpu_trace_format(trace_nes, pc, opcode, buf, sizeof(buf));

    if (trace_output)
        trace_output("%s", buf);
    else
        printf("%s", buf);
}

void cpu_trace_set_nes(NES *nes) {
    trace_nes = nes;
}

void cpu_trace_install_hook(void) {
    debug_hooks.on_cpu_step = cpu_trace_hook;
}
