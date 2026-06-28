/*
 * CPU Instruction Trace Formatter
 *
 * Produces nestest-compatible trace output with disassembly and register state.
 * Uses side-effect-free debug reads to avoid affecting emulation.
 */

#ifndef NES_CPU_TRACE_H
#define NES_CPU_TRACE_H

#include <stdint.h>

struct NES;

/* Format a trace line for the instruction that just completed.
 * pc: address of the instruction, opcode: the instruction byte.
 * Writes to buf (max buf_size chars). Returns chars written. */
int cpu_trace_format(const struct NES *nes, uint16_t pc, uint8_t opcode,
                     char *buf, int buf_size);

/* Install CPU trace as the on_cpu_step hook.
 * Requires the NES pointer to be set via cpu_trace_set_nes() first. */
void cpu_trace_set_nes(struct NES *nes);
void cpu_trace_install_hook(void);

#endif /* NES_CPU_TRACE_H */
