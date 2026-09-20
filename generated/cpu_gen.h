#ifndef CPU_GEN_H
#define CPU_GEN_H

/* Auto-generated 6502 microcode - DO NOT EDIT */
#include <stdint.h>
#include <stdbool.h>

typedef struct CPU {
    uint8_t A, X, Y, SP, P, IR, DL, ADL, ADH;
    uint16_t PC, uPC;
    bool page_cross, branch_taken;
    bool irq_pending, nmi_pending, reset_pending;
    bool rdy;  /* RDY line - when false, CPU is halted (for DMA) */
    uint16_t last_read_addr;  /* Last address read by CPU (for DMA halt cycles) */
    uint8_t effective_i; /* I flag value used for next IRQ poll (1-instr delayed for CLI/SEI/PLP) */
    uint8_t ignore_h;  /* SHA/SHX/SHY/TAS: skip H register in store value when DMA halts on the dummy-read cycle (AccuracyCoin SHA test sub-test 7+, mirrors C# Emulator.cs IgnoreH) */
    uint16_t interrupt_vector;
    uint8_t irq_sampled;
    uint8_t nmi_sampled, nmi_armed;
    uint8_t irq_armed; /* Interrupt sampled before the final cycle, dispatched at fetch */
    uint64_t cycles;
    uint8_t (*mem_read)(struct CPU *cpu, uint16_t addr);
    void (*mem_write)(struct CPU *cpu, uint16_t addr, uint8_t val);
    void *user_data;
} CPU;

extern const uint16_t cpu_entry[256];

static inline void cpu_init(CPU *cpu) {
    cpu->A = cpu->X = cpu->Y = 0; cpu->SP = 0xFD; cpu->P = 0x24;
    cpu->PC = cpu->uPC = cpu->IR = cpu->DL = cpu->ADL = cpu->ADH = 0;
    cpu->page_cross = cpu->branch_taken = 0;
    cpu->irq_pending = cpu->nmi_pending = cpu->reset_pending = 0;
    cpu->rdy = true;  /* CPU ready to run */
    cpu->last_read_addr = 0;
    cpu->effective_i = 1;  /* I flag set at init */
    cpu->ignore_h = 0;
    cpu->interrupt_vector = 0;
    cpu->irq_sampled = cpu->nmi_sampled = 0;
    cpu->irq_armed = cpu->nmi_armed = 0;
    cpu->cycles = 0;
}

void cpu_step(CPU *cpu);
uint16_t cpu_get_next_read_addr(CPU *cpu);
bool cpu_next_is_write(CPU *cpu);
bool cpu_next_is_sha_dummy_read(CPU *cpu);

#endif /* CPU_GEN_H */
