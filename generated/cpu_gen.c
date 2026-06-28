/* Auto-generated 6502 microcode - DO NOT EDIT */
#include "cpu_gen.h"

void cpu_step(CPU *cpu) {
    cpu->cycles++;
    /* RDY line - when low, CPU is halted (for DMA) */
    if (!cpu->rdy) return;
    switch (cpu->uPC) {
    case 0: /* fetch */
        if (cpu->reset_pending) { cpu->reset_pending = 0; cpu->uPC = 14; return; }
        if (cpu->nmi_pending) { cpu->nmi_pending = 0; cpu->uPC = 1; return; }
        if (cpu->irq_armed && !cpu->effective_i) { cpu->irq_armed = 0; cpu->irq_pending = 0; cpu->uPC = 7; return; }
        cpu->irq_armed = cpu->irq_pending;
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->last_read_addr = cpu->PC;
        cpu->IR = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = cpu_entry[cpu->IR]; return;
    case 1: /* nmi-handler */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 2; return;
    case 2: /* nmi-handler */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), ((cpu->PC >> 8) & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 3; return;
    case 3: /* nmi-handler */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), (cpu->PC & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 4; return;
    case 4: /* nmi-handler */
        cpu->DL = (cpu->P | 0x20) & ~0x10;
        cpu->mem_write(cpu, (0x0100 | cpu->SP), cpu->DL);
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 5; return;
    case 5: /* nmi-handler */
        cpu->P |= 0x04;
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->last_read_addr = 0xFFFA;
        cpu->ADL = cpu->mem_read(cpu, 0xFFFA);
        cpu->uPC = 6; return;
    case 6: /* nmi-handler */
        cpu->last_read_addr = 0xFFFB;
        cpu->ADH = cpu->mem_read(cpu, 0xFFFB);
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 7: /* irq-handler */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 8; return;
    case 8: /* irq-handler */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), ((cpu->PC >> 8) & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 9; return;
    case 9: /* irq-handler */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), (cpu->PC & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 10; return;
    case 10: /* irq-handler */
        cpu->DL = (cpu->P | 0x20) & ~0x10;
        cpu->mem_write(cpu, (0x0100 | cpu->SP), cpu->DL);
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 11; return;
    case 11: /* irq-handler */
        cpu->P |= 0x04;
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->last_read_addr = (cpu->nmi_pending ? 0xFFFA : 0xFFFE);
        cpu->ADL = cpu->mem_read(cpu, (cpu->nmi_pending ? 0xFFFA : 0xFFFE));
        cpu->uPC = 12; return;
    case 12: /* irq-handler */
        cpu->last_read_addr = (cpu->nmi_pending ? (cpu->nmi_pending = 0, 0xFFFB) : 0xFFFF);
        cpu->ADH = cpu->mem_read(cpu, (cpu->nmi_pending ? (cpu->nmi_pending = 0, 0xFFFB) : 0xFFFF));
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 13: /* nmi-handler-next */
        cpu->last_read_addr = 0xFFFB;
        cpu->ADH = cpu->mem_read(cpu, 0xFFFB);
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 14: /* reset-handler */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 15; return;
    case 15: /* reset-handler */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 16; return;
    case 16: /* reset-handler */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 17; return;
    case 17: /* reset-handler */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->P |= 0x04;
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->uPC = 18; return;
    case 18: /* reset-handler */
        cpu->last_read_addr = 0xFFFC;
        cpu->ADL = cpu->mem_read(cpu, 0xFFFC);
        cpu->uPC = 19; return;
    case 19: /* reset-handler */
        cpu->last_read_addr = 0xFFFD;
        cpu->ADH = cpu->mem_read(cpu, 0xFFFD);
        cpu->uPC = 20; return;
    case 20: /* reset-handler */
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 21: /* illegal */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 22: /* lda-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 23: /* lda-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 24; return;
    case 24: /* lda-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 25: /* lda-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 26; return;
    case 26: /* lda-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 27; return;
    case 27: /* lda-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 28: /* lda-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 29; return;
    case 29: /* lda-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 30; return;
    case 30: /* lda-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 31: /* lda-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 32; return;
    case 32: /* lda-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 33; return;
    case 33: /* lda-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 34; return; }
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 34: /* lda-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 35: /* lda-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 36; return;
    case 36: /* lda-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 37; return;
    case 37: /* lda-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 38; return; }
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 38: /* lda-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 39: /* lda-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 40; return;
    case 40: /* lda-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 41; return;
    case 41: /* lda-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 42; return;
    case 42: /* lda-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 43; return;
    case 43: /* lda-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 44: /* lda-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 45; return;
    case 45: /* lda-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 46; return;
    case 46: /* lda-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 47; return;
    case 47: /* lda-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 48; return; }
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 48: /* lda-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 49: /* ldx-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 50: /* ldx-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 51; return;
    case 51: /* ldx-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->X = cpu->DL;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 52: /* ldx-zpy */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 53; return;
    case 53: /* ldx-zpy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 54; return;
    case 54: /* ldx-zpy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->X = cpu->DL;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 55: /* ldx-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 56; return;
    case 56: /* ldx-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 57; return;
    case 57: /* ldx-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->X = cpu->DL;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 58: /* ldx-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 59; return;
    case 59: /* ldx-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 60; return;
    case 60: /* ldx-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 61; return; }
        cpu->X = cpu->DL;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 61: /* ldx-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->X = cpu->DL;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 62: /* ldy-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->Y = cpu->DL;
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 63: /* ldy-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 64; return;
    case 64: /* ldy-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->Y = cpu->DL;
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 65: /* ldy-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 66; return;
    case 66: /* ldy-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 67; return;
    case 67: /* ldy-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->Y = cpu->DL;
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 68: /* ldy-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 69; return;
    case 69: /* ldy-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 70; return;
    case 70: /* ldy-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->Y = cpu->DL;
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 71: /* ldy-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 72; return;
    case 72: /* ldy-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 73; return;
    case 73: /* ldy-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 74; return; }
        cpu->Y = cpu->DL;
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 74: /* ldy-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->Y = cpu->DL;
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 75: /* sta-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 76; return;
    case 76: /* sta-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A);
        cpu->uPC = 0; return;
    case 77: /* sta-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 78; return;
    case 78: /* sta-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 79; return;
    case 79: /* sta-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A);
        cpu->uPC = 0; return;
    case 80: /* sta-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 81; return;
    case 81: /* sta-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 82; return;
    case 82: /* sta-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A);
        cpu->uPC = 0; return;
    case 83: /* sta-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 84; return;
    case 84: /* sta-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 85; return;
    case 85: /* sta-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 86; return;
    case 86: /* sta-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A);
        cpu->uPC = 0; return;
    case 87: /* sta-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 88; return;
    case 88: /* sta-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 89; return;
    case 89: /* sta-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 90; return;
    case 90: /* sta-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A);
        cpu->uPC = 0; return;
    case 91: /* sta-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 92; return;
    case 92: /* sta-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 93; return;
    case 93: /* sta-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 94; return;
    case 94: /* sta-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 95; return;
    case 95: /* sta-izx */
        cpu->ADL = cpu->DL;
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A);
        cpu->uPC = 0; return;
    case 96: /* sta-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 97; return;
    case 97: /* sta-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 98; return;
    case 98: /* sta-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 99; return;
    case 99: /* sta-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 100; return;
    case 100: /* sta-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A);
        cpu->uPC = 0; return;
    case 101: /* stx-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 102; return;
    case 102: /* stx-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->X);
        cpu->uPC = 0; return;
    case 103: /* stx-zpy */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 104; return;
    case 104: /* stx-zpy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 105; return;
    case 105: /* stx-zpy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->X);
        cpu->uPC = 0; return;
    case 106: /* stx-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 107; return;
    case 107: /* stx-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 108; return;
    case 108: /* stx-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->X);
        cpu->uPC = 0; return;
    case 109: /* sty-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 110; return;
    case 110: /* sty-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->Y);
        cpu->uPC = 0; return;
    case 111: /* sty-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 112; return;
    case 112: /* sty-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 113; return;
    case 113: /* sty-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->Y);
        cpu->uPC = 0; return;
    case 114: /* sty-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 115; return;
    case 115: /* sty-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 116; return;
    case 116: /* sty-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->Y);
        cpu->uPC = 0; return;
    case 117: /* tax */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->X = cpu->A;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 118: /* tay */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->Y = cpu->A;
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 119: /* txa */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->A = cpu->X;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 120: /* tya */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->A = cpu->Y;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 121: /* tsx */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->X = cpu->SP;
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 122: /* txs */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->SP = cpu->X;
        cpu->uPC = 0; return;
    case 123: /* pha */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 124; return;
    case 124: /* pha */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), cpu->A);
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 0; return;
    case 125: /* php */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->DL = cpu->P | 0x30;
        cpu->uPC = 126; return;
    case 126: /* php */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), cpu->DL);
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 0; return;
    case 127: /* pla */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 128; return;
    case 128: /* pla */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP + 1) & 0xFF;
        cpu->uPC = 129; return;
    case 129: /* pla */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        cpu->A = cpu->mem_read(cpu, (0x0100 | cpu->SP));
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 130: /* plp */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 131; return;
    case 131: /* plp */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP + 1) & 0xFF;
        cpu->uPC = 132; return;
    case 132: /* plp */
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->last_read_addr = (0x0100 | cpu->SP);
        cpu->P = cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->uPC = 0; return;
    case 133: /* adc-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 134: /* adc-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 135; return;
    case 135: /* adc-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 136: /* adc-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 137; return;
    case 137: /* adc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 138; return;
    case 138: /* adc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 139: /* adc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 140; return;
    case 140: /* adc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 141; return;
    case 141: /* adc-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 142: /* adc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 143; return;
    case 143: /* adc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 144; return;
    case 144: /* adc-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 145; return; }
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 145: /* adc-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 146: /* adc-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 147; return;
    case 147: /* adc-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 148; return;
    case 148: /* adc-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 149; return; }
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 149: /* adc-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 150: /* adc-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 151; return;
    case 151: /* adc-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 152; return;
    case 152: /* adc-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 153; return;
    case 153: /* adc-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 154; return;
    case 154: /* adc-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 155: /* adc-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 156; return;
    case 156: /* adc-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 157; return;
    case 157: /* adc-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 158; return;
    case 158: /* adc-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 159; return; }
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 159: /* adc-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 160: /* sbc-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 161: /* sbc-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 162; return;
    case 162: /* sbc-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 163: /* sbc-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 164; return;
    case 164: /* sbc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 165; return;
    case 165: /* sbc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 166: /* sbc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 167; return;
    case 167: /* sbc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 168; return;
    case 168: /* sbc-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 169: /* sbc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 170; return;
    case 170: /* sbc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 171; return;
    case 171: /* sbc-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 172; return; }
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 172: /* sbc-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 173: /* sbc-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 174; return;
    case 174: /* sbc-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 175; return;
    case 175: /* sbc-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 176; return; }
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 176: /* sbc-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 177: /* sbc-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 178; return;
    case 178: /* sbc-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 179; return;
    case 179: /* sbc-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 180; return;
    case 180: /* sbc-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 181; return;
    case 181: /* sbc-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 182: /* sbc-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 183; return;
    case 183: /* sbc-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 184; return;
    case 184: /* sbc-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 185; return;
    case 185: /* sbc-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 186; return; }
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 186: /* sbc-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 187: /* and-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 188: /* and-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 189; return;
    case 189: /* and-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 190: /* and-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 191; return;
    case 191: /* and-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 192; return;
    case 192: /* and-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 193: /* and-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 194; return;
    case 194: /* and-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 195; return;
    case 195: /* and-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 196: /* and-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 197; return;
    case 197: /* and-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 198; return;
    case 198: /* and-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 199; return; }
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 199: /* and-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 200: /* and-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 201; return;
    case 201: /* and-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 202; return;
    case 202: /* and-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 203; return; }
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 203: /* and-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 204: /* and-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 205; return;
    case 205: /* and-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 206; return;
    case 206: /* and-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 207; return;
    case 207: /* and-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 208; return;
    case 208: /* and-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 209: /* and-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 210; return;
    case 210: /* and-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 211; return;
    case 211: /* and-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 212; return;
    case 212: /* and-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 213; return; }
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 213: /* and-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 214: /* ora-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 215: /* ora-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 216; return;
    case 216: /* ora-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 217: /* ora-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 218; return;
    case 218: /* ora-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 219; return;
    case 219: /* ora-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 220: /* ora-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 221; return;
    case 221: /* ora-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 222; return;
    case 222: /* ora-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 223: /* ora-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 224; return;
    case 224: /* ora-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 225; return;
    case 225: /* ora-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 226; return; }
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 226: /* ora-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 227: /* ora-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 228; return;
    case 228: /* ora-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 229; return;
    case 229: /* ora-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 230; return; }
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 230: /* ora-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 231: /* ora-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 232; return;
    case 232: /* ora-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 233; return;
    case 233: /* ora-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 234; return;
    case 234: /* ora-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 235; return;
    case 235: /* ora-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 236: /* ora-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 237; return;
    case 237: /* ora-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 238; return;
    case 238: /* ora-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 239; return;
    case 239: /* ora-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 240; return; }
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 240: /* ora-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 241: /* eor-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 242: /* eor-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 243; return;
    case 243: /* eor-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 244: /* eor-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 245; return;
    case 245: /* eor-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 246; return;
    case 246: /* eor-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 247: /* eor-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 248; return;
    case 248: /* eor-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 249; return;
    case 249: /* eor-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 250: /* eor-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 251; return;
    case 251: /* eor-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 252; return;
    case 252: /* eor-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 253; return; }
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 253: /* eor-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 254: /* eor-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 255; return;
    case 255: /* eor-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 256; return;
    case 256: /* eor-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 257; return; }
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 257: /* eor-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 258: /* eor-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 259; return;
    case 259: /* eor-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 260; return;
    case 260: /* eor-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 261; return;
    case 261: /* eor-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 262; return;
    case 262: /* eor-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 263: /* eor-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 264; return;
    case 264: /* eor-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 265; return;
    case 265: /* eor-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 266; return;
    case 266: /* eor-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 267; return; }
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 267: /* eor-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 268: /* cmp-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 269: /* cmp-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 270; return;
    case 270: /* cmp-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 271: /* cmp-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 272; return;
    case 272: /* cmp-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 273; return;
    case 273: /* cmp-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 274: /* cmp-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 275; return;
    case 275: /* cmp-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 276; return;
    case 276: /* cmp-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 277: /* cmp-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 278; return;
    case 278: /* cmp-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 279; return;
    case 279: /* cmp-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 280; return; }
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 280: /* cmp-abx when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 281: /* cmp-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 282; return;
    case 282: /* cmp-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 283; return;
    case 283: /* cmp-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 284; return; }
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 284: /* cmp-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 285: /* cmp-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 286; return;
    case 286: /* cmp-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 287; return;
    case 287: /* cmp-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 288; return;
    case 288: /* cmp-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 289; return;
    case 289: /* cmp-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 290: /* cmp-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 291; return;
    case 291: /* cmp-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 292; return;
    case 292: /* cmp-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 293; return;
    case 293: /* cmp-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 294; return; }
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 294: /* cmp-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 295: /* cpx-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t a = cpu->X, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 296: /* cpx-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 297; return;
    case 297: /* cpx-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->X, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 298: /* cpx-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 299; return;
    case 299: /* cpx-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 300; return;
    case 300: /* cpx-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->X, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 301: /* cpy-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint8_t a = cpu->Y, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 302: /* cpy-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 303; return;
    case 303: /* cpy-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->Y, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 304: /* cpy-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 305; return;
    case 305: /* cpy-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 306; return;
    case 306: /* cpy-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->Y, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 307: /* bit-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 308; return;
    case 308: /* bit-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = a & b; cpu->P = (cpu->P & 0x3D) | (r == 0 ? 2 : 0) | (b & 0xC0); }
        cpu->uPC = 0; return;
    case 309: /* bit-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 310; return;
    case 310: /* bit-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 311; return;
    case 311: /* bit-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t a = cpu->A, b = cpu->DL, r = a & b; cpu->P = (cpu->P & 0x3D) | (r == 0 ? 2 : 0) | (b & 0xC0); }
        cpu->uPC = 0; return;
    case 312: /* inc-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 313; return;
    case 313: /* inc-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 314; return;
    case 314: /* inc-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 315; return;
    case 315: /* inc-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 316: /* inc-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 317; return;
    case 317: /* inc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 318; return;
    case 318: /* inc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 319; return;
    case 319: /* inc-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 320; return;
    case 320: /* inc-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 321: /* inc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 322; return;
    case 322: /* inc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 323; return;
    case 323: /* inc-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 324; return;
    case 324: /* inc-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 325; return;
    case 325: /* inc-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 326: /* inc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 327; return;
    case 327: /* inc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 328; return;
    case 328: /* inc-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 329; return;
    case 329: /* inc-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 330; return;
    case 330: /* inc-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 331; return;
    case 331: /* inc-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 332: /* dec-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 333; return;
    case 333: /* dec-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 334; return;
    case 334: /* dec-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 335; return;
    case 335: /* dec-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 336: /* dec-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 337; return;
    case 337: /* dec-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 338; return;
    case 338: /* dec-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 339; return;
    case 339: /* dec-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 340; return;
    case 340: /* dec-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 341: /* dec-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 342; return;
    case 342: /* dec-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 343; return;
    case 343: /* dec-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 344; return;
    case 344: /* dec-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 345; return;
    case 345: /* dec-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 346: /* dec-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 347; return;
    case 347: /* dec-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 348; return;
    case 348: /* dec-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 349; return;
    case 349: /* dec-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 350; return;
    case 350: /* dec-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 351; return;
    case 351: /* dec-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 352: /* inx */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t n = (cpu->X + 1) & 0xFF; cpu->X = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 353: /* iny */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t n = (cpu->Y + 1) & 0xFF; cpu->Y = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 354: /* dex */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t n = (cpu->X - 1) & 0xFF; cpu->X = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        { uint8_t v = cpu->X; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 355: /* dey */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t n = (cpu->Y - 1) & 0xFF; cpu->Y = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        { uint8_t v = cpu->Y; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 356: /* asl-acc */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t o = cpu->A, n = (o << 1) & 0xFF; cpu->A = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 0; return;
    case 357: /* lsr-acc */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t o = cpu->A, n = o >> 1; cpu->A = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 0; return;
    case 358: /* rol-acc */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t o = cpu->A, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->A = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 0; return;
    case 359: /* ror-acc */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        { uint8_t o = cpu->A, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->A = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 0; return;
    case 360: /* asl-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 361; return;
    case 361: /* asl-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 362; return;
    case 362: /* asl-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 363; return;
    case 363: /* asl-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 364: /* asl-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 365; return;
    case 365: /* asl-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 366; return;
    case 366: /* asl-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 367; return;
    case 367: /* asl-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 368; return;
    case 368: /* asl-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 369: /* asl-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 370; return;
    case 370: /* asl-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 371; return;
    case 371: /* asl-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 372; return;
    case 372: /* asl-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 373; return;
    case 373: /* asl-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 374: /* asl-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 375; return;
    case 375: /* asl-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 376; return;
    case 376: /* asl-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 377; return;
    case 377: /* asl-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 378; return;
    case 378: /* asl-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 379; return;
    case 379: /* asl-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 380: /* lsr-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 381; return;
    case 381: /* lsr-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 382; return;
    case 382: /* lsr-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 383; return;
    case 383: /* lsr-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 384: /* lsr-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 385; return;
    case 385: /* lsr-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 386; return;
    case 386: /* lsr-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 387; return;
    case 387: /* lsr-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 388; return;
    case 388: /* lsr-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 389: /* lsr-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 390; return;
    case 390: /* lsr-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 391; return;
    case 391: /* lsr-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 392; return;
    case 392: /* lsr-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 393; return;
    case 393: /* lsr-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 394: /* lsr-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 395; return;
    case 395: /* lsr-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 396; return;
    case 396: /* lsr-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 397; return;
    case 397: /* lsr-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 398; return;
    case 398: /* lsr-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 399; return;
    case 399: /* lsr-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 400: /* rol-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 401; return;
    case 401: /* rol-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 402; return;
    case 402: /* rol-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 403; return;
    case 403: /* rol-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 404: /* rol-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 405; return;
    case 405: /* rol-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 406; return;
    case 406: /* rol-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 407; return;
    case 407: /* rol-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 408; return;
    case 408: /* rol-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 409: /* rol-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 410; return;
    case 410: /* rol-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 411; return;
    case 411: /* rol-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 412; return;
    case 412: /* rol-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 413; return;
    case 413: /* rol-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 414: /* rol-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 415; return;
    case 415: /* rol-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 416; return;
    case 416: /* rol-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 417; return;
    case 417: /* rol-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 418; return;
    case 418: /* rol-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 419; return;
    case 419: /* rol-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 420: /* ror-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 421; return;
    case 421: /* ror-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 422; return;
    case 422: /* ror-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 423; return;
    case 423: /* ror-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 424: /* ror-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 425; return;
    case 425: /* ror-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 426; return;
    case 426: /* ror-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 427; return;
    case 427: /* ror-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 428; return;
    case 428: /* ror-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 429: /* ror-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 430; return;
    case 430: /* ror-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 431; return;
    case 431: /* ror-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 432; return;
    case 432: /* ror-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 433; return;
    case 433: /* ror-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 434: /* ror-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 435; return;
    case 435: /* ror-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 436; return;
    case 436: /* ror-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 437; return;
    case 437: /* ror-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 438; return;
    case 438: /* ror-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 439; return;
    case 439: /* ror-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 440: /* bpl */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if (!((cpu->P & 0x80))) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 441; return; }
        cpu->uPC = 0; return;
    case 441: /* bpl when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 442; return; }
        cpu->uPC = 0; return;
    case 442: /* bpl when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 443: /* bmi */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if ((cpu->P & 0x80)) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 444; return; }
        cpu->uPC = 0; return;
    case 444: /* bmi when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 445; return; }
        cpu->uPC = 0; return;
    case 445: /* bmi when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 446: /* bvc */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if (!((cpu->P & 0x40))) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 447; return; }
        cpu->uPC = 0; return;
    case 447: /* bvc when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 448; return; }
        cpu->uPC = 0; return;
    case 448: /* bvc when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 449: /* bvs */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if ((cpu->P & 0x40)) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 450; return; }
        cpu->uPC = 0; return;
    case 450: /* bvs when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 451; return; }
        cpu->uPC = 0; return;
    case 451: /* bvs when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 452: /* bcc */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if (!((cpu->P & 0x01))) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 453; return; }
        cpu->uPC = 0; return;
    case 453: /* bcc when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 454; return; }
        cpu->uPC = 0; return;
    case 454: /* bcc when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 455: /* bcs */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if ((cpu->P & 0x01)) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 456; return; }
        cpu->uPC = 0; return;
    case 456: /* bcs when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 457; return; }
        cpu->uPC = 0; return;
    case 457: /* bcs when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 458: /* bne */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if (!((cpu->P & 0x02))) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 459; return; }
        cpu->uPC = 0; return;
    case 459: /* bne when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 460; return; }
        cpu->uPC = 0; return;
    case 460: /* bne when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 461: /* beq */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        if ((cpu->P & 0x02)) {
            cpu->branch_taken = 1;
            uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
            cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
        } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
        if (cpu->branch_taken) { cpu->uPC = 462; return; }
        cpu->uPC = 0; return;
    case 462: /* beq when branch-taken */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
        if (cpu->page_cross) { cpu->uPC = 463; return; }
        cpu->uPC = 0; return;
    case 463: /* beq when page-cross */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
        else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
        cpu->uPC = 0; return;
    case 464: /* jmp-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 465; return;
    case 465: /* jmp-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 466: /* jmp-ind */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 467; return;
    case 467: /* jmp-ind */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 468; return;
    case 468: /* jmp-ind */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 469; return;
    case 469: /* jmp-ind */
        { uint8_t n = (cpu->ADL + 1) & 0xFF; cpu->ADL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->DL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 470: /* jsr */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 471; return;
    case 471: /* jsr */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->uPC = 472; return;
    case 472: /* jsr */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), ((cpu->PC >> 8) & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 473; return;
    case 473: /* jsr */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), (cpu->PC & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 474; return;
    case 474: /* jsr */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 475: /* rts */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 476; return;
    case 476: /* rts */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP + 1) & 0xFF;
        cpu->uPC = 477; return;
    case 477: /* rts */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        cpu->ADL = cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP + 1) & 0xFF;
        cpu->uPC = 478; return;
    case 478: /* rts */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        cpu->ADH = cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->uPC = 479; return;
    case 479: /* rts */
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 0; return;
    case 480: /* rti */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 481; return;
    case 481: /* rti */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        (void)cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP + 1) & 0xFF;
        cpu->uPC = 482; return;
    case 482: /* rti */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        cpu->P = cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->SP = (cpu->SP + 1) & 0xFF;
        cpu->uPC = 483; return;
    case 483: /* rti */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        cpu->ADL = cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->SP = (cpu->SP + 1) & 0xFF;
        cpu->uPC = 484; return;
    case 484: /* rti */
        cpu->last_read_addr = (0x0100 | cpu->SP);
        cpu->ADH = cpu->mem_read(cpu, (0x0100 | cpu->SP));
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 485: /* clc */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->P &= ~0x01;
        cpu->uPC = 0; return;
    case 486: /* sec */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->P |= 0x01;
        cpu->uPC = 0; return;
    case 487: /* cli */
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->P &= ~0x04;
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 488: /* sei */
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->P |= 0x04;
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 489: /* cld */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->P &= ~0x08;
        cpu->uPC = 0; return;
    case 490: /* sed */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->P |= 0x08;
        cpu->uPC = 0; return;
    case 491: /* clv */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->P &= ~0x40;
        cpu->uPC = 0; return;
    case 492: /* nop */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 493: /* brk */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 494; return;
    case 494: /* brk */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), ((cpu->PC >> 8) & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 495; return;
    case 495: /* brk */
        cpu->mem_write(cpu, (0x0100 | cpu->SP), (cpu->PC & 0xFF));
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 496; return;
    case 496: /* brk */
        cpu->DL = cpu->nmi_pending ? ((cpu->P | 0x20) & ~0x10) : (cpu->P | 0x30);
        cpu->mem_write(cpu, (0x0100 | cpu->SP), cpu->DL);
        cpu->SP = (cpu->SP - 1) & 0xFF;
        cpu->uPC = 497; return;
    case 497: /* brk */
        cpu->P |= 0x04;
        cpu->effective_i = (cpu->P >> 2) & 1;
        cpu->last_read_addr = (cpu->nmi_pending ? 0xFFFA : 0xFFFE);
        cpu->ADL = cpu->mem_read(cpu, (cpu->nmi_pending ? 0xFFFA : 0xFFFE));
        cpu->uPC = 498; return;
    case 498: /* brk */
        cpu->last_read_addr = (cpu->nmi_pending ? (cpu->nmi_pending = 0, 0xFFFB) : 0xFFFF);
        cpu->ADH = cpu->mem_read(cpu, (cpu->nmi_pending ? (cpu->nmi_pending = 0, 0xFFFB) : 0xFFFF));
        cpu->PC = (cpu->PC & 0xFF00) | (cpu->ADL);
        cpu->PC = (cpu->PC & 0x00FF) | ((cpu->ADH) << 8);
        cpu->uPC = 0; return;
    case 499: /* lax-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 500; return;
    case 500: /* lax-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 501: /* lax-zpy */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 502; return;
    case 502: /* lax-zpy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 503; return;
    case 503: /* lax-zpy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 504: /* lax-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 505; return;
    case 505: /* lax-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 506; return;
    case 506: /* lax-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 507: /* lax-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 508; return;
    case 508: /* lax-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 509; return;
    case 509: /* lax-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 510; return; }
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 510: /* lax-aby when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 511: /* lax-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 512; return;
    case 512: /* lax-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 513; return;
    case 513: /* lax-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 514; return;
    case 514: /* lax-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 515; return;
    case 515: /* lax-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 516: /* lax-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 517; return;
    case 517: /* lax-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 518; return;
    case 518: /* lax-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 519; return;
    case 519: /* lax-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 520; return; }
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 520: /* lax-izy when page-cross */
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 521: /* lax-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->A = cpu->DL;
        cpu->X = cpu->DL;
        { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 522: /* sax-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 523; return;
    case 523: /* sax-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A & cpu->X);
        cpu->uPC = 0; return;
    case 524: /* sax-zpy */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 525; return;
    case 525: /* sax-zpy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 526; return;
    case 526: /* sax-zpy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A & cpu->X);
        cpu->uPC = 0; return;
    case 527: /* sax-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 528; return;
    case 528: /* sax-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 529; return;
    case 529: /* sax-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A & cpu->X);
        cpu->uPC = 0; return;
    case 530: /* sax-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 531; return;
    case 531: /* sax-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 532; return;
    case 532: /* sax-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 533; return;
    case 533: /* sax-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 534; return;
    case 534: /* sax-izx */
        cpu->ADL = cpu->DL;
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->A & cpu->X);
        cpu->uPC = 0; return;
    case 535: /* dcp-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 536; return;
    case 536: /* dcp-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 537; return;
    case 537: /* dcp-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 538; return;
    case 538: /* dcp-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 539: /* dcp-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 540; return;
    case 540: /* dcp-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 541; return;
    case 541: /* dcp-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 542; return;
    case 542: /* dcp-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 543; return;
    case 543: /* dcp-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 544: /* dcp-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 545; return;
    case 545: /* dcp-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 546; return;
    case 546: /* dcp-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 547; return;
    case 547: /* dcp-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 548; return;
    case 548: /* dcp-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 549: /* dcp-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 550; return;
    case 550: /* dcp-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 551; return;
    case 551: /* dcp-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 552; return;
    case 552: /* dcp-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 553; return;
    case 553: /* dcp-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 554; return;
    case 554: /* dcp-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 555: /* dcp-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 556; return;
    case 556: /* dcp-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 557; return;
    case 557: /* dcp-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 558; return;
    case 558: /* dcp-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 559; return;
    case 559: /* dcp-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 560; return;
    case 560: /* dcp-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 561: /* dcp-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 562; return;
    case 562: /* dcp-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 563; return;
    case 563: /* dcp-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 564; return;
    case 564: /* dcp-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 565; return;
    case 565: /* dcp-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 566; return;
    case 566: /* dcp-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 567; return;
    case 567: /* dcp-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 568: /* dcp-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 569; return;
    case 569: /* dcp-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 570; return;
    case 570: /* dcp-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 571; return;
    case 571: /* dcp-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 572; return;
    case 572: /* dcp-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 573; return;
    case 573: /* dcp-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL - 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 574; return;
    case 574: /* dcp-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, r = (a - b) & 0xFF; cpu->P = (cpu->P & 0x7C) | (a >= b ? 1 : 0) | (a == b ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 575: /* isc-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 576; return;
    case 576: /* isc-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 577; return;
    case 577: /* isc-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 578; return;
    case 578: /* isc-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 579: /* isc-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 580; return;
    case 580: /* isc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 581; return;
    case 581: /* isc-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 582; return;
    case 582: /* isc-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 583; return;
    case 583: /* isc-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 584: /* isc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 585; return;
    case 585: /* isc-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 586; return;
    case 586: /* isc-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 587; return;
    case 587: /* isc-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 588; return;
    case 588: /* isc-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 589: /* isc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 590; return;
    case 590: /* isc-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 591; return;
    case 591: /* isc-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 592; return;
    case 592: /* isc-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 593; return;
    case 593: /* isc-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 594; return;
    case 594: /* isc-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 595: /* isc-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 596; return;
    case 596: /* isc-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 597; return;
    case 597: /* isc-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 598; return;
    case 598: /* isc-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 599; return;
    case 599: /* isc-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 600; return;
    case 600: /* isc-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 601: /* isc-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 602; return;
    case 602: /* isc-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 603; return;
    case 603: /* isc-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 604; return;
    case 604: /* isc-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 605; return;
    case 605: /* isc-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 606; return;
    case 606: /* isc-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 607; return;
    case 607: /* isc-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 608: /* isc-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 609; return;
    case 609: /* isc-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 610; return;
    case 610: /* isc-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 611; return;
    case 611: /* isc-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 612; return;
    case 612: /* isc-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 613; return;
    case 613: /* isc-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7D) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 614; return;
    case 614: /* isc-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; int16_t d = a - b - (1 - c); uint8_t r = d & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (d >= 0 ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | (((a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 615: /* slo-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 616; return;
    case 616: /* slo-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 617; return;
    case 617: /* slo-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 618; return;
    case 618: /* slo-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 619: /* slo-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 620; return;
    case 620: /* slo-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 621; return;
    case 621: /* slo-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 622; return;
    case 622: /* slo-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 623; return;
    case 623: /* slo-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 624: /* slo-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 625; return;
    case 625: /* slo-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 626; return;
    case 626: /* slo-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 627; return;
    case 627: /* slo-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 628; return;
    case 628: /* slo-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 629: /* slo-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 630; return;
    case 630: /* slo-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 631; return;
    case 631: /* slo-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 632; return;
    case 632: /* slo-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 633; return;
    case 633: /* slo-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 634; return;
    case 634: /* slo-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 635: /* slo-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 636; return;
    case 636: /* slo-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 637; return;
    case 637: /* slo-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 638; return;
    case 638: /* slo-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 639; return;
    case 639: /* slo-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 640; return;
    case 640: /* slo-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 641: /* slo-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 642; return;
    case 642: /* slo-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 643; return;
    case 643: /* slo-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 644; return;
    case 644: /* slo-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 645; return;
    case 645: /* slo-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 646; return;
    case 646: /* slo-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 647; return;
    case 647: /* slo-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 648: /* slo-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 649; return;
    case 649: /* slo-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 650; return;
    case 650: /* slo-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 651; return;
    case 651: /* slo-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 652; return;
    case 652: /* slo-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 653; return;
    case 653: /* slo-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = (o << 1) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 654; return;
    case 654: /* slo-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A | cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 655: /* rla-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 656; return;
    case 656: /* rla-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 657; return;
    case 657: /* rla-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 658; return;
    case 658: /* rla-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 659: /* rla-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 660; return;
    case 660: /* rla-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 661; return;
    case 661: /* rla-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 662; return;
    case 662: /* rla-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 663; return;
    case 663: /* rla-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 664: /* rla-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 665; return;
    case 665: /* rla-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 666; return;
    case 666: /* rla-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 667; return;
    case 667: /* rla-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 668; return;
    case 668: /* rla-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 669: /* rla-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 670; return;
    case 670: /* rla-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 671; return;
    case 671: /* rla-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 672; return;
    case 672: /* rla-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 673; return;
    case 673: /* rla-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 674; return;
    case 674: /* rla-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 675: /* rla-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 676; return;
    case 676: /* rla-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 677; return;
    case 677: /* rla-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 678; return;
    case 678: /* rla-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 679; return;
    case 679: /* rla-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 680; return;
    case 680: /* rla-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 681: /* rla-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 682; return;
    case 682: /* rla-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 683; return;
    case 683: /* rla-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 684; return;
    case 684: /* rla-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 685; return;
    case 685: /* rla-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 686; return;
    case 686: /* rla-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 687; return;
    case 687: /* rla-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 688: /* rla-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 689; return;
    case 689: /* rla-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 690; return;
    case 690: /* rla-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 691; return;
    case 691: /* rla-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 692; return;
    case 692: /* rla-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 693; return;
    case 693: /* rla-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = ((o << 1) | c) & 0xFF; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | ((o >> 7) & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 694; return;
    case 694: /* rla-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 695: /* sre-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 696; return;
    case 696: /* sre-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 697; return;
    case 697: /* sre-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 698; return;
    case 698: /* sre-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 699: /* sre-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 700; return;
    case 700: /* sre-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 701; return;
    case 701: /* sre-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 702; return;
    case 702: /* sre-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 703; return;
    case 703: /* sre-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 704: /* sre-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 705; return;
    case 705: /* sre-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 706; return;
    case 706: /* sre-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 707; return;
    case 707: /* sre-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 708; return;
    case 708: /* sre-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 709: /* sre-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 710; return;
    case 710: /* sre-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 711; return;
    case 711: /* sre-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 712; return;
    case 712: /* sre-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 713; return;
    case 713: /* sre-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 714; return;
    case 714: /* sre-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 715: /* sre-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 716; return;
    case 716: /* sre-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 717; return;
    case 717: /* sre-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 718; return;
    case 718: /* sre-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 719; return;
    case 719: /* sre-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 720; return;
    case 720: /* sre-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 721: /* sre-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 722; return;
    case 722: /* sre-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 723; return;
    case 723: /* sre-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 724; return;
    case 724: /* sre-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 725; return;
    case 725: /* sre-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 726; return;
    case 726: /* sre-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 727; return;
    case 727: /* sre-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 728: /* sre-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 729; return;
    case 729: /* sre-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 730; return;
    case 730: /* sre-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 731; return;
    case 731: /* sre-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 732; return;
    case 732: /* sre-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 733; return;
    case 733: /* sre-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, n = o >> 1; cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 734; return;
    case 734: /* sre-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t r = cpu->A ^ cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 735: /* rra-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 736; return;
    case 736: /* rra-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 737; return;
    case 737: /* rra-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 738; return;
    case 738: /* rra-zp */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 739: /* rra-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 740; return;
    case 740: /* rra-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 741; return;
    case 741: /* rra-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 742; return;
    case 742: /* rra-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 743; return;
    case 743: /* rra-zpx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 744: /* rra-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 745; return;
    case 745: /* rra-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 746; return;
    case 746: /* rra-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 747; return;
    case 747: /* rra-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 748; return;
    case 748: /* rra-abs */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 749: /* rra-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 750; return;
    case 750: /* rra-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 751; return;
    case 751: /* rra-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 752; return;
    case 752: /* rra-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 753; return;
    case 753: /* rra-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 754; return;
    case 754: /* rra-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 755: /* rra-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 756; return;
    case 756: /* rra-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 757; return;
    case 757: /* rra-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 758; return;
    case 758: /* rra-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 759; return;
    case 759: /* rra-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 760; return;
    case 760: /* rra-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 761: /* rra-izx */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 762; return;
    case 762: /* rra-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 763; return;
    case 763: /* rra-izx */
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 764; return;
    case 764: /* rra-izx */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 765; return;
    case 765: /* rra-izx */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 766; return;
    case 766: /* rra-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 767; return;
    case 767: /* rra-izx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 768: /* rra-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 769; return;
    case 769: /* rra-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 770; return;
    case 770: /* rra-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 771; return;
    case 771: /* rra-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 772; return;
    case 772: /* rra-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 773; return;
    case 773: /* rra-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t o = cpu->DL, c = cpu->P & 1, n = (o >> 1) | (c << 7); cpu->DL = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0) | (n & 0x80); }
        cpu->uPC = 774; return;
    case 774: /* rra-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        { uint8_t a = cpu->A, b = cpu->DL, c = cpu->P & 1; uint16_t s = a + b + c; uint8_t r = s & 0xFF;
          cpu->P = (cpu->P & 0x3C) | (s > 0xFF ? 1 : 0) | (r == 0 ? 2 : 0) | (r & 0x80) | ((~(a^b) & (a^r) & 0x80) ? 0x40 : 0);
        cpu->A = r;
        }
        cpu->uPC = 0; return;
    case 775: /* anc-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 776; return;
    case 776: /* anc-imm */
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->P = (cpu->P & 0xFE) | ((cpu->A >> 7) & 1);
        cpu->uPC = 0; return;
    case 777: /* alr-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 778; return;
    case 778: /* alr-imm */
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        { uint8_t o = cpu->A, n = o >> 1; cpu->A = n; cpu->P = (cpu->P & 0x7C) | (o & 1) | (n == 0 ? 2 : 0); }
        cpu->uPC = 0; return;
    case 779: /* arr-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 780; return;
    case 780: /* arr-imm */
        { uint8_t t = cpu->A & cpu->DL; uint8_t r = (t >> 1) | ((cpu->P & 1) << 7);
          cpu->A = r; cpu->P = (cpu->P & 0x3C) | ((r >> 6) & 1) | (r == 0 ? 2 : 0) | (r & 0x80) | (((r >> 6) ^ (r >> 5)) & 1 ? 0x40 : 0); }
        cpu->uPC = 0; return;
    case 781: /* xaa-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 782; return;
    case 782: /* xaa-imm */
        cpu->A = cpu->X;
        { uint8_t r = cpu->A & cpu->DL; cpu->A = r; cpu->P = (cpu->P & 0x7D) | (r == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 783: /* axs-imm */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 784; return;
    case 784: /* axs-imm */
        { uint8_t t = cpu->A & cpu->X; int16_t r = t - cpu->DL; cpu->X = r & 0xFF;
          cpu->P = (cpu->P & 0x7C) | (r >= 0 ? 1 : 0) | ((r & 0xFF) == 0 ? 2 : 0) | (r & 0x80); }
        cpu->uPC = 0; return;
    case 785: /* stp */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 785; return;
    case 786: /* nop-1 */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 787: /* nop-2 */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 788; return;
    case 788: /* nop-2 */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 789: /* nop-3 */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 790; return;
    case 790: /* nop-3 */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 791; return;
    case 791: /* nop-3 */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 792: /* nop-zp */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 793; return;
    case 793: /* nop-zp */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 794; return;
    case 794: /* nop-zp */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 795: /* nop-zpx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 796; return;
    case 796: /* nop-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        (void)cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 797; return;
    case 797: /* nop-zpx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 798; return;
    case 798: /* nop-zpx */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 799: /* nop-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 800; return;
    case 800: /* nop-abs */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 801; return;
    case 801: /* nop-abs */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 802; return;
    case 802: /* nop-abs */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 803: /* nop-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 804; return;
    case 804: /* nop-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 805; return;
    case 805: /* nop-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 806; return; }
        cpu->uPC = 807; return;
    case 806: /* nop-abx when page-cross */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 807; return;
    case 807: /* nop-abx */
        cpu->last_read_addr = cpu->PC;
        (void)cpu->mem_read(cpu, cpu->PC);
        cpu->uPC = 0; return;
    case 808: /* las-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 809; return;
    case 809: /* las-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 810; return;
    case 810: /* las-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        if (cpu->page_cross) { cpu->uPC = 811; return; }
        cpu->uPC = 812; return;
    case 811: /* las-aby when page-cross */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->ADH = cpu->ADH + (cpu->page_cross ? 1 : 0);
        cpu->page_cross = 0;
        cpu->uPC = 812; return;
    case 812: /* las-aby */
        { uint8_t v = cpu->DL & cpu->SP; cpu->A = cpu->X = cpu->SP = v;
          cpu->P = (cpu->P & 0x7D) | (v == 0 ? 2 : 0) | (v & 0x80); }
        cpu->uPC = 0; return;
    case 813: /* tas-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 814; return;
    case 814: /* tas-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 815; return;
    case 815: /* tas-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->SP = cpu->A & cpu->X; cpu->DL = cpu->SP & h1; if (cpu->page_cross) { cpu->ADH = h1 & cpu->A & cpu->X; cpu->page_cross = 0; } }
        cpu->uPC = 816; return;
    case 816: /* tas-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        cpu->uPC = 0; return;
    case 817: /* sha-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 818; return;
    case 818: /* sha-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 819; return;
    case 819: /* sha-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->DL = cpu->A & cpu->X & h1; if (cpu->page_cross) { cpu->ADH = h1 & cpu->A & cpu->X; cpu->page_cross = 0; } }
        cpu->uPC = 820; return;
    case 820: /* sha-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        cpu->uPC = 0; return;
    case 821: /* sha-izy */
        cpu->last_read_addr = cpu->PC;
        cpu->DL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->ADH = 0;
        cpu->uPC = 822; return;
    case 822: /* sha-izy */
        cpu->ADL = cpu->DL;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        cpu->uPC = 823; return;
    case 823: /* sha-izy */
        cpu->ADL = (cpu->ADL + 1) & 0xFF;
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->ADH = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint16_t s = (uint16_t)cpu->DL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 824; return;
    case 824: /* sha-izy */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->DL = cpu->A & cpu->X & h1; if (cpu->page_cross) { cpu->ADH = h1 & cpu->A & cpu->X; cpu->page_cross = 0; } }
        cpu->uPC = 825; return;
    case 825: /* sha-izy */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        cpu->uPC = 0; return;
    case 826: /* shx-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 827; return;
    case 827: /* shx-aby */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->Y; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 828; return;
    case 828: /* shx-aby */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->DL = cpu->X & h1; if (cpu->page_cross) { cpu->ADH = h1 & cpu->X; cpu->page_cross = 0; } }
        cpu->uPC = 829; return;
    case 829: /* shx-aby */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        cpu->uPC = 0; return;
    case 830: /* shy-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADL = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        cpu->uPC = 831; return;
    case 831: /* shy-abx */
        cpu->last_read_addr = cpu->PC;
        cpu->ADH = cpu->mem_read(cpu, cpu->PC);
        cpu->PC = (cpu->PC + 1) & 0xFFFF;
        { uint16_t s = (uint16_t)cpu->ADL + (uint16_t)cpu->X; cpu->ADL = s & 0xFF; cpu->page_cross = (s > 0xFF); }
        cpu->uPC = 832; return;
    case 832: /* shy-abx */
        cpu->last_read_addr = ((uint16_t)cpu->ADH << 8) | cpu->ADL;
        cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
        { uint8_t h1 = cpu->ignore_h ? 0xFF : (cpu->ADH + 1); cpu->ignore_h = 0; cpu->DL = cpu->Y & h1; if (cpu->page_cross) { cpu->ADH = h1 & cpu->Y; cpu->page_cross = 0; } }
        cpu->uPC = 833; return;
    case 833: /* shy-abx */
        cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
        cpu->uPC = 0; return;
    default: cpu->uPC = 0; return;
    }
}

uint16_t cpu_get_next_read_addr(CPU *cpu) {
    switch (cpu->uPC) {
    case 0: return cpu->PC;
    case 1: return cpu->PC;
    case 5: return 0xFFFA;
    case 6: return 0xFFFB;
    case 7: return cpu->PC;
    case 11: return (cpu->nmi_pending ? 0xFFFA : 0xFFFE);
    case 12: return (cpu->nmi_pending ? 0xFFFB : 0xFFFF);
    case 13: return 0xFFFB;
    case 14: return cpu->PC;
    case 15: return (0x0100 | cpu->SP);
    case 16: return (0x0100 | cpu->SP);
    case 17: return (0x0100 | cpu->SP);
    case 18: return 0xFFFC;
    case 19: return 0xFFFD;
    case 21: return cpu->PC;
    case 22: return cpu->PC;
    case 23: return cpu->PC;
    case 24: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 25: return cpu->PC;
    case 26: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 27: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 28: return cpu->PC;
    case 29: return cpu->PC;
    case 30: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 31: return cpu->PC;
    case 32: return cpu->PC;
    case 33: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 34: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 35: return cpu->PC;
    case 36: return cpu->PC;
    case 37: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 38: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 39: return cpu->PC;
    case 40: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 41: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 42: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 43: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 44: return cpu->PC;
    case 45: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 46: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 47: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 48: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 49: return cpu->PC;
    case 50: return cpu->PC;
    case 51: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 52: return cpu->PC;
    case 53: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 54: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 55: return cpu->PC;
    case 56: return cpu->PC;
    case 57: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 58: return cpu->PC;
    case 59: return cpu->PC;
    case 60: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 61: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 62: return cpu->PC;
    case 63: return cpu->PC;
    case 64: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 65: return cpu->PC;
    case 66: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 67: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 68: return cpu->PC;
    case 69: return cpu->PC;
    case 70: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 71: return cpu->PC;
    case 72: return cpu->PC;
    case 73: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 74: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 75: return cpu->PC;
    case 77: return cpu->PC;
    case 78: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 80: return cpu->PC;
    case 81: return cpu->PC;
    case 83: return cpu->PC;
    case 84: return cpu->PC;
    case 85: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 87: return cpu->PC;
    case 88: return cpu->PC;
    case 89: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 91: return cpu->PC;
    case 92: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 93: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 94: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 96: return cpu->PC;
    case 97: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 98: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 99: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 101: return cpu->PC;
    case 103: return cpu->PC;
    case 104: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 106: return cpu->PC;
    case 107: return cpu->PC;
    case 109: return cpu->PC;
    case 111: return cpu->PC;
    case 112: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 114: return cpu->PC;
    case 115: return cpu->PC;
    case 117: return cpu->PC;
    case 118: return cpu->PC;
    case 119: return cpu->PC;
    case 120: return cpu->PC;
    case 121: return cpu->PC;
    case 122: return cpu->PC;
    case 123: return cpu->PC;
    case 125: return cpu->PC;
    case 127: return cpu->PC;
    case 128: return (0x0100 | cpu->SP);
    case 129: return (0x0100 | cpu->SP);
    case 130: return cpu->PC;
    case 131: return (0x0100 | cpu->SP);
    case 132: return (0x0100 | cpu->SP);
    case 133: return cpu->PC;
    case 134: return cpu->PC;
    case 135: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 136: return cpu->PC;
    case 137: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 138: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 139: return cpu->PC;
    case 140: return cpu->PC;
    case 141: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 142: return cpu->PC;
    case 143: return cpu->PC;
    case 144: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 145: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 146: return cpu->PC;
    case 147: return cpu->PC;
    case 148: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 149: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 150: return cpu->PC;
    case 151: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 152: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 153: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 154: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 155: return cpu->PC;
    case 156: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 157: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 158: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 159: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 160: return cpu->PC;
    case 161: return cpu->PC;
    case 162: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 163: return cpu->PC;
    case 164: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 165: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 166: return cpu->PC;
    case 167: return cpu->PC;
    case 168: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 169: return cpu->PC;
    case 170: return cpu->PC;
    case 171: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 172: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 173: return cpu->PC;
    case 174: return cpu->PC;
    case 175: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 176: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 177: return cpu->PC;
    case 178: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 179: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 180: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 181: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 182: return cpu->PC;
    case 183: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 184: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 185: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 186: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 187: return cpu->PC;
    case 188: return cpu->PC;
    case 189: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 190: return cpu->PC;
    case 191: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 192: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 193: return cpu->PC;
    case 194: return cpu->PC;
    case 195: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 196: return cpu->PC;
    case 197: return cpu->PC;
    case 198: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 199: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 200: return cpu->PC;
    case 201: return cpu->PC;
    case 202: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 203: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 204: return cpu->PC;
    case 205: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 206: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 207: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 208: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 209: return cpu->PC;
    case 210: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 211: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 212: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 213: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 214: return cpu->PC;
    case 215: return cpu->PC;
    case 216: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 217: return cpu->PC;
    case 218: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 219: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 220: return cpu->PC;
    case 221: return cpu->PC;
    case 222: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 223: return cpu->PC;
    case 224: return cpu->PC;
    case 225: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 226: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 227: return cpu->PC;
    case 228: return cpu->PC;
    case 229: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 230: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 231: return cpu->PC;
    case 232: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 233: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 234: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 235: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 236: return cpu->PC;
    case 237: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 238: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 239: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 240: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 241: return cpu->PC;
    case 242: return cpu->PC;
    case 243: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 244: return cpu->PC;
    case 245: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 246: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 247: return cpu->PC;
    case 248: return cpu->PC;
    case 249: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 250: return cpu->PC;
    case 251: return cpu->PC;
    case 252: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 253: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 254: return cpu->PC;
    case 255: return cpu->PC;
    case 256: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 257: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 258: return cpu->PC;
    case 259: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 260: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 261: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 262: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 263: return cpu->PC;
    case 264: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 265: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 266: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 267: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 268: return cpu->PC;
    case 269: return cpu->PC;
    case 270: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 271: return cpu->PC;
    case 272: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 273: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 274: return cpu->PC;
    case 275: return cpu->PC;
    case 276: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 277: return cpu->PC;
    case 278: return cpu->PC;
    case 279: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 280: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 281: return cpu->PC;
    case 282: return cpu->PC;
    case 283: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 284: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 285: return cpu->PC;
    case 286: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 287: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 288: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 289: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 290: return cpu->PC;
    case 291: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 292: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 293: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 294: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 295: return cpu->PC;
    case 296: return cpu->PC;
    case 297: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 298: return cpu->PC;
    case 299: return cpu->PC;
    case 300: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 301: return cpu->PC;
    case 302: return cpu->PC;
    case 303: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 304: return cpu->PC;
    case 305: return cpu->PC;
    case 306: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 307: return cpu->PC;
    case 308: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 309: return cpu->PC;
    case 310: return cpu->PC;
    case 311: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 312: return cpu->PC;
    case 313: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 316: return cpu->PC;
    case 317: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 318: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 321: return cpu->PC;
    case 322: return cpu->PC;
    case 323: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 326: return cpu->PC;
    case 327: return cpu->PC;
    case 328: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 329: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 332: return cpu->PC;
    case 333: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 336: return cpu->PC;
    case 337: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 338: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 341: return cpu->PC;
    case 342: return cpu->PC;
    case 343: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 346: return cpu->PC;
    case 347: return cpu->PC;
    case 348: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 349: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 352: return cpu->PC;
    case 353: return cpu->PC;
    case 354: return cpu->PC;
    case 355: return cpu->PC;
    case 356: return cpu->PC;
    case 357: return cpu->PC;
    case 358: return cpu->PC;
    case 359: return cpu->PC;
    case 360: return cpu->PC;
    case 361: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 364: return cpu->PC;
    case 365: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 366: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 369: return cpu->PC;
    case 370: return cpu->PC;
    case 371: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 374: return cpu->PC;
    case 375: return cpu->PC;
    case 376: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 377: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 380: return cpu->PC;
    case 381: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 384: return cpu->PC;
    case 385: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 386: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 389: return cpu->PC;
    case 390: return cpu->PC;
    case 391: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 394: return cpu->PC;
    case 395: return cpu->PC;
    case 396: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 397: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 400: return cpu->PC;
    case 401: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 404: return cpu->PC;
    case 405: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 406: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 409: return cpu->PC;
    case 410: return cpu->PC;
    case 411: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 414: return cpu->PC;
    case 415: return cpu->PC;
    case 416: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 417: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 420: return cpu->PC;
    case 421: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 424: return cpu->PC;
    case 425: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 426: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 429: return cpu->PC;
    case 430: return cpu->PC;
    case 431: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 434: return cpu->PC;
    case 435: return cpu->PC;
    case 436: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 437: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 440: return cpu->PC;
    case 441: return cpu->PC;
    case 442: return cpu->PC;
    case 443: return cpu->PC;
    case 444: return cpu->PC;
    case 445: return cpu->PC;
    case 446: return cpu->PC;
    case 447: return cpu->PC;
    case 448: return cpu->PC;
    case 449: return cpu->PC;
    case 450: return cpu->PC;
    case 451: return cpu->PC;
    case 452: return cpu->PC;
    case 453: return cpu->PC;
    case 454: return cpu->PC;
    case 455: return cpu->PC;
    case 456: return cpu->PC;
    case 457: return cpu->PC;
    case 458: return cpu->PC;
    case 459: return cpu->PC;
    case 460: return cpu->PC;
    case 461: return cpu->PC;
    case 462: return cpu->PC;
    case 463: return cpu->PC;
    case 464: return cpu->PC;
    case 465: return cpu->PC;
    case 466: return cpu->PC;
    case 467: return cpu->PC;
    case 468: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 469: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 470: return cpu->PC;
    case 471: return (0x0100 | cpu->SP);
    case 474: return cpu->PC;
    case 475: return cpu->PC;
    case 476: return (0x0100 | cpu->SP);
    case 477: return (0x0100 | cpu->SP);
    case 478: return (0x0100 | cpu->SP);
    case 480: return cpu->PC;
    case 481: return (0x0100 | cpu->SP);
    case 482: return (0x0100 | cpu->SP);
    case 483: return (0x0100 | cpu->SP);
    case 484: return (0x0100 | cpu->SP);
    case 485: return cpu->PC;
    case 486: return cpu->PC;
    case 487: return cpu->PC;
    case 488: return cpu->PC;
    case 489: return cpu->PC;
    case 490: return cpu->PC;
    case 491: return cpu->PC;
    case 492: return cpu->PC;
    case 493: return cpu->PC;
    case 497: return (cpu->nmi_pending ? 0xFFFA : 0xFFFE);
    case 498: return (cpu->nmi_pending ? 0xFFFB : 0xFFFF);
    case 499: return cpu->PC;
    case 500: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 501: return cpu->PC;
    case 502: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 503: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 504: return cpu->PC;
    case 505: return cpu->PC;
    case 506: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 507: return cpu->PC;
    case 508: return cpu->PC;
    case 509: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 510: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 511: return cpu->PC;
    case 512: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 513: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 514: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 515: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 516: return cpu->PC;
    case 517: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 518: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 519: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 520: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 521: return cpu->PC;
    case 522: return cpu->PC;
    case 524: return cpu->PC;
    case 525: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 527: return cpu->PC;
    case 528: return cpu->PC;
    case 530: return cpu->PC;
    case 531: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 532: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 533: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 535: return cpu->PC;
    case 536: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 539: return cpu->PC;
    case 540: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 541: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 544: return cpu->PC;
    case 545: return cpu->PC;
    case 546: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 549: return cpu->PC;
    case 550: return cpu->PC;
    case 551: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 552: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 555: return cpu->PC;
    case 556: return cpu->PC;
    case 557: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 558: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 561: return cpu->PC;
    case 562: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 563: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 564: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 565: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 568: return cpu->PC;
    case 569: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 570: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 571: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 572: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 575: return cpu->PC;
    case 576: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 579: return cpu->PC;
    case 580: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 581: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 584: return cpu->PC;
    case 585: return cpu->PC;
    case 586: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 589: return cpu->PC;
    case 590: return cpu->PC;
    case 591: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 592: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 595: return cpu->PC;
    case 596: return cpu->PC;
    case 597: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 598: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 601: return cpu->PC;
    case 602: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 603: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 604: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 605: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 608: return cpu->PC;
    case 609: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 610: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 611: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 612: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 615: return cpu->PC;
    case 616: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 619: return cpu->PC;
    case 620: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 621: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 624: return cpu->PC;
    case 625: return cpu->PC;
    case 626: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 629: return cpu->PC;
    case 630: return cpu->PC;
    case 631: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 632: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 635: return cpu->PC;
    case 636: return cpu->PC;
    case 637: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 638: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 641: return cpu->PC;
    case 642: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 643: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 644: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 645: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 648: return cpu->PC;
    case 649: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 650: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 651: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 652: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 655: return cpu->PC;
    case 656: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 659: return cpu->PC;
    case 660: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 661: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 664: return cpu->PC;
    case 665: return cpu->PC;
    case 666: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 669: return cpu->PC;
    case 670: return cpu->PC;
    case 671: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 672: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 675: return cpu->PC;
    case 676: return cpu->PC;
    case 677: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 678: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 681: return cpu->PC;
    case 682: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 683: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 684: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 685: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 688: return cpu->PC;
    case 689: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 690: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 691: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 692: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 695: return cpu->PC;
    case 696: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 699: return cpu->PC;
    case 700: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 701: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 704: return cpu->PC;
    case 705: return cpu->PC;
    case 706: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 709: return cpu->PC;
    case 710: return cpu->PC;
    case 711: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 712: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 715: return cpu->PC;
    case 716: return cpu->PC;
    case 717: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 718: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 721: return cpu->PC;
    case 722: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 723: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 724: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 725: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 728: return cpu->PC;
    case 729: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 730: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 731: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 732: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 735: return cpu->PC;
    case 736: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 739: return cpu->PC;
    case 740: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 741: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 744: return cpu->PC;
    case 745: return cpu->PC;
    case 746: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 749: return cpu->PC;
    case 750: return cpu->PC;
    case 751: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 752: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 755: return cpu->PC;
    case 756: return cpu->PC;
    case 757: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 758: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 761: return cpu->PC;
    case 762: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 763: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 764: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 765: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 768: return cpu->PC;
    case 769: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 770: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 771: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 772: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 775: return cpu->PC;
    case 777: return cpu->PC;
    case 779: return cpu->PC;
    case 781: return cpu->PC;
    case 783: return cpu->PC;
    case 785: return cpu->PC;
    case 786: return cpu->PC;
    case 787: return cpu->PC;
    case 788: return cpu->PC;
    case 789: return cpu->PC;
    case 790: return cpu->PC;
    case 791: return cpu->PC;
    case 792: return cpu->PC;
    case 793: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 794: return cpu->PC;
    case 795: return cpu->PC;
    case 796: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 797: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 798: return cpu->PC;
    case 799: return cpu->PC;
    case 800: return cpu->PC;
    case 801: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 802: return cpu->PC;
    case 803: return cpu->PC;
    case 804: return cpu->PC;
    case 805: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 806: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 807: return cpu->PC;
    case 808: return cpu->PC;
    case 809: return cpu->PC;
    case 810: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 811: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 813: return cpu->PC;
    case 814: return cpu->PC;
    case 815: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 817: return cpu->PC;
    case 818: return cpu->PC;
    case 819: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 821: return cpu->PC;
    case 822: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 823: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 824: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 826: return cpu->PC;
    case 827: return cpu->PC;
    case 828: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    case 830: return cpu->PC;
    case 831: return cpu->PC;
    case 832: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    default: return cpu->last_read_addr;
    }
}

bool cpu_next_is_write(CPU *cpu) {
    switch (cpu->uPC) {
    case 2: return true;
    case 3: return true;
    case 4: return true;
    case 8: return true;
    case 9: return true;
    case 10: return true;
    case 76: return true;
    case 79: return true;
    case 82: return true;
    case 86: return true;
    case 90: return true;
    case 95: return true;
    case 100: return true;
    case 102: return true;
    case 105: return true;
    case 108: return true;
    case 110: return true;
    case 113: return true;
    case 116: return true;
    case 124: return true;
    case 126: return true;
    case 314: return true;
    case 315: return true;
    case 319: return true;
    case 320: return true;
    case 324: return true;
    case 325: return true;
    case 330: return true;
    case 331: return true;
    case 334: return true;
    case 335: return true;
    case 339: return true;
    case 340: return true;
    case 344: return true;
    case 345: return true;
    case 350: return true;
    case 351: return true;
    case 362: return true;
    case 363: return true;
    case 367: return true;
    case 368: return true;
    case 372: return true;
    case 373: return true;
    case 378: return true;
    case 379: return true;
    case 382: return true;
    case 383: return true;
    case 387: return true;
    case 388: return true;
    case 392: return true;
    case 393: return true;
    case 398: return true;
    case 399: return true;
    case 402: return true;
    case 403: return true;
    case 407: return true;
    case 408: return true;
    case 412: return true;
    case 413: return true;
    case 418: return true;
    case 419: return true;
    case 422: return true;
    case 423: return true;
    case 427: return true;
    case 428: return true;
    case 432: return true;
    case 433: return true;
    case 438: return true;
    case 439: return true;
    case 472: return true;
    case 473: return true;
    case 494: return true;
    case 495: return true;
    case 496: return true;
    case 537: return true;
    case 538: return true;
    case 542: return true;
    case 543: return true;
    case 547: return true;
    case 548: return true;
    case 553: return true;
    case 554: return true;
    case 559: return true;
    case 560: return true;
    case 566: return true;
    case 567: return true;
    case 573: return true;
    case 574: return true;
    case 577: return true;
    case 578: return true;
    case 582: return true;
    case 583: return true;
    case 587: return true;
    case 588: return true;
    case 593: return true;
    case 594: return true;
    case 599: return true;
    case 600: return true;
    case 606: return true;
    case 607: return true;
    case 613: return true;
    case 614: return true;
    case 617: return true;
    case 618: return true;
    case 622: return true;
    case 623: return true;
    case 627: return true;
    case 628: return true;
    case 633: return true;
    case 634: return true;
    case 639: return true;
    case 640: return true;
    case 646: return true;
    case 647: return true;
    case 653: return true;
    case 654: return true;
    case 657: return true;
    case 658: return true;
    case 662: return true;
    case 663: return true;
    case 667: return true;
    case 668: return true;
    case 673: return true;
    case 674: return true;
    case 679: return true;
    case 680: return true;
    case 686: return true;
    case 687: return true;
    case 693: return true;
    case 694: return true;
    case 697: return true;
    case 698: return true;
    case 702: return true;
    case 703: return true;
    case 707: return true;
    case 708: return true;
    case 713: return true;
    case 714: return true;
    case 719: return true;
    case 720: return true;
    case 726: return true;
    case 727: return true;
    case 733: return true;
    case 734: return true;
    case 737: return true;
    case 738: return true;
    case 742: return true;
    case 743: return true;
    case 747: return true;
    case 748: return true;
    case 753: return true;
    case 754: return true;
    case 759: return true;
    case 760: return true;
    case 766: return true;
    case 767: return true;
    case 773: return true;
    case 774: return true;
    case 816: return true;
    case 820: return true;
    case 825: return true;
    case 829: return true;
    case 833: return true;
    default: return false;
    }
}

bool cpu_next_is_sha_dummy_read(CPU *cpu) {
    switch (cpu->uPC) {
    case 815: return true;
    case 819: return true;
    case 824: return true;
    case 828: return true;
    case 832: return true;
    default: return false;
    }
}

const uint16_t cpu_entry[256] = {
    493,231,785,641,792,215,360,615,125,214,356,775,799,220,369,624,
    440,236,785,648,795,217,364,619,485,227,786,635,803,223,374,629,
    470,204,785,681,307,188,400,655,130,187,358,775,309,193,409,664,
    443,209,785,688,795,190,404,659,486,200,786,675,803,196,414,669,
    480,258,785,721,792,242,380,695,123,241,357,777,464,247,389,704,
    446,263,785,728,795,244,384,699,487,254,786,715,803,250,394,709,
    475,150,785,761,792,134,420,735,127,133,359,779,466,139,429,744,
    449,155,785,768,795,136,424,739,488,146,786,755,803,142,434,749,
    787,91,787,530,109,75,101,522,355,787,119,781,114,80,106,527,
    452,96,785,821,111,77,103,524,120,87,122,813,830,83,826,817,
    62,39,49,511,63,23,50,499,118,22,117,521,68,28,55,504,
    455,44,785,516,65,25,52,501,491,35,121,808,71,31,58,507,
    301,285,787,561,302,269,332,535,353,268,354,783,304,274,341,544,
    458,290,785,568,795,271,336,539,489,281,786,555,803,277,346,549,
    295,177,787,601,296,161,312,575,352,160,492,160,298,166,321,584,
    461,182,785,608,795,163,316,579,490,173,786,595,803,169,326,589
};
