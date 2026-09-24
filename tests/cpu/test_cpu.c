#include <stdio.h>
#include <string.h>
#include "cpu_gen.h"

static uint8_t memory[65536];

static uint8_t mem_read(CPU *cpu, uint16_t addr) {
    (void)cpu;
    return memory[addr];
}

static void mem_write(CPU *cpu, uint16_t addr, uint8_t val) {
    (void)cpu;
    memory[addr] = val;
}

static void run_until_nop(CPU *cpu, int max_cycles, int trace) {
    int nop_started = 0;
    for (int i = 0; i < max_cycles; i++) {
        if (trace) {
            printf("cyc=%3llu uPC=%3d PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X IR=%02X\n",
                   (unsigned long long)cpu->cycles, cpu->uPC, cpu->PC,
                   cpu->A, cpu->X, cpu->Y, cpu->SP, cpu->P, cpu->IR);
        }
        /* Track when NOP (0xEA) is fetched */
        if (cpu->IR == 0xEA && cpu->uPC != 0) nop_started = 1;
        cpu_step(cpu);
        /* Stop when NOP completes (back to fetch state after NOP) */
        if (nop_started && cpu->uPC == 0) break;
    }
}

int test_lda_sta(void) {
    CPU cpu;
    /* Initialization must clear interrupt latches even on reused storage. */
    memset(&cpu, 0xFF, sizeof(cpu));
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* LDA #$42, STA $10, LDX $10, NOP */
    uint8_t prog[] = { 0xA9, 0x42, 0x85, 0x10, 0xA6, 0x10, 0xEA };
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    if (cpu.A == 0x42 && cpu.X == 0x42 && memory[0x10] == 0x42) {
        printf("TEST lda_sta: PASS (A=%02X X=%02X mem[10]=%02X)\n", cpu.A, cpu.X, memory[0x10]);
        return 1;
    } else {
        printf("TEST lda_sta: FAIL (A=%02X X=%02X mem[10]=%02X)\n", cpu.A, cpu.X, memory[0x10]);
        return 0;
    }
}

int test_arithmetic(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* CLC, LDA #$50, ADC #$30 -> A=$80, SBC #$10 -> A=$6F (with carry) */
    uint8_t prog[] = { 0x18, 0xA9, 0x50, 0x69, 0x30, 0xEA };
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    if (cpu.A == 0x80 && (cpu.P & 0x80)) {  /* N flag set */
        printf("TEST arithmetic: PASS (A=%02X P=%02X)\n", cpu.A, cpu.P);
        return 1;
    } else {
        printf("TEST arithmetic: FAIL (A=%02X P=%02X)\n", cpu.A, cpu.P);
        return 0;
    }
}

int test_branch(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* LDA #$05, TAX, loop: DEX, BNE loop, NOP */
    uint8_t prog[] = { 0xA9, 0x05, 0xAA, 0xCA, 0xD0, 0xFD, 0xEA };
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 200, 0);

    if (cpu.X == 0x00 && (cpu.P & 0x02)) {  /* Z flag set */
        printf("TEST branch: PASS (X=%02X P=%02X cycles=%llu)\n",
               cpu.X, cpu.P, (unsigned long long)cpu.cycles);
        return 1;
    } else {
        printf("TEST branch: FAIL (X=%02X P=%02X cycles=%llu)\n",
               cpu.X, cpu.P, (unsigned long long)cpu.cycles);
        return 0;
    }
}

int test_stack(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* LDA #$55, PHA, LDA #$00, PLA, NOP */
    uint8_t prog[] = { 0xA9, 0x55, 0x48, 0xA9, 0x00, 0x68, 0xEA };
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    if (cpu.A == 0x55) {
        printf("TEST stack: PASS (A=%02X SP=%02X)\n", cpu.A, cpu.SP);
        return 1;
    } else {
        printf("TEST stack: FAIL (A=%02X SP=%02X)\n", cpu.A, cpu.SP);
        return 0;
    }
}

int test_jsr_rts(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* JSR $0210, NOP ; At $0210: LDA #$77, RTS */
    uint8_t prog[] = { 0x20, 0x10, 0x02, 0xEA };  /* JSR $0210, NOP */
    uint8_t sub[] = { 0xA9, 0x77, 0x60 };  /* LDA #$77, RTS */
    memcpy(&memory[0x200], prog, sizeof(prog));
    memcpy(&memory[0x210], sub, sizeof(sub));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    if (cpu.A == 0x77 && cpu.PC == 0x204) {
        printf("TEST jsr_rts: PASS (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 1;
    } else {
        printf("TEST jsr_rts: FAIL (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 0;
    }
}

int test_shift(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* LDA #$81, ASL A, ROL A, NOP */
    uint8_t prog[] = { 0xA9, 0x81, 0x0A, 0x2A, 0xEA };
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    /* $81 ASL -> $02, C=1; $02 ROL -> $05 (includes carry) */
    if (cpu.A == 0x05) {
        printf("TEST shift: PASS (A=%02X P=%02X)\n", cpu.A, cpu.P);
        return 1;
    } else {
        printf("TEST shift: FAIL (A=%02X P=%02X expected A=05)\n", cpu.A, cpu.P);
        return 0;
    }
}

int test_lax_unofficial(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* Store value at $10, then LAX $10 (unofficial: load A and X) */
    memory[0x10] = 0x33;
    uint8_t prog[] = { 0xA7, 0x10, 0xEA };  /* LAX $10, NOP */
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    if (cpu.A == 0x33 && cpu.X == 0x33) {
        printf("TEST lax_unofficial: PASS (A=%02X X=%02X)\n", cpu.A, cpu.X);
        return 1;
    } else {
        printf("TEST lax_unofficial: FAIL (A=%02X X=%02X)\n", cpu.A, cpu.X);
        return 0;
    }
}

int test_dcp_unofficial(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* LDA #$10, store $10 at $20, DCP $20 (dec then cmp) */
    memory[0x20] = 0x10;
    uint8_t prog[] = { 0xA9, 0x10, 0xC7, 0x20, 0xEA };  /* LDA #$10, DCP $20, NOP */
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    /* DCP: $10 - 1 = $0F, then CMP A($10) with $0F -> C=1, Z=0 */
    if (memory[0x20] == 0x0F && (cpu.P & 0x01) && !(cpu.P & 0x02)) {
        printf("TEST dcp_unofficial: PASS (mem[20]=%02X P=%02X)\n", memory[0x20], cpu.P);
        return 1;
    } else {
        printf("TEST dcp_unofficial: FAIL (mem[20]=%02X P=%02X)\n", memory[0x20], cpu.P);
        return 0;
    }
}

int test_indexed_indirect(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* Setup: $30,$31 = $0050 (pointer), $0050 = $99 */
    memory[0x30] = 0x50;
    memory[0x31] = 0x00;
    memory[0x50] = 0x99;

    /* LDX #$20, LDA ($10,X), NOP  ; $10 + $20 = $30 -> pointer to $0050 */
    uint8_t prog[] = { 0xA2, 0x20, 0xA1, 0x10, 0xEA };
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    if (cpu.A == 0x99) {
        printf("TEST indexed_indirect: PASS (A=%02X)\n", cpu.A);
        return 1;
    } else {
        printf("TEST indexed_indirect: FAIL (A=%02X expected 99)\n", cpu.A);
        return 0;
    }
}

int test_indirect_indexed(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* Setup: $20,$21 = $0100 (base), $0105 = $77 */
    memory[0x20] = 0x00;
    memory[0x21] = 0x01;
    memory[0x105] = 0x77;

    /* LDY #$05, LDA ($20),Y, NOP  ; $0100 + Y($05) = $0105 */
    uint8_t prog[] = { 0xA0, 0x05, 0xB1, 0x20, 0xEA };
    memcpy(&memory[0x200], prog, sizeof(prog));
    cpu.PC = 0x200;
    cpu.uPC = 0;

    run_until_nop(&cpu, 100, 0);

    if (cpu.A == 0x77) {
        printf("TEST indirect_indexed: PASS (A=%02X)\n", cpu.A);
        return 1;
    } else {
        printf("TEST indirect_indexed: FAIL (A=%02X expected 77)\n", cpu.A);
        return 0;
    }
}

int test_nmi(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* Main program: loop forever at $200 */
    uint8_t prog[] = { 0xA9, 0x00, 0x4C, 0x02, 0x02 };  /* LDA #$00, JMP $0202 */
    memcpy(&memory[0x200], prog, sizeof(prog));

    /* NMI handler at $0300: LDA #$55, RTI */
    uint8_t nmi_handler[] = { 0xA9, 0x55, 0x40 };
    memcpy(&memory[0x300], nmi_handler, sizeof(nmi_handler));

    /* Set NMI vector */
    memory[0xFFFA] = 0x00;  /* Low byte */
    memory[0xFFFB] = 0x03;  /* High byte -> $0300 */

    cpu.PC = 0x200;
    cpu.uPC = 0;

    /* Run a few instructions, trigger NMI, continue */
    for (int i = 0; i < 20; i++) cpu_step(&cpu);  /* Execute initial code */

    cpu.nmi_pending = 1;  /* Trigger NMI */

    /* Run until RTI completes and we're back in main loop */
    for (int i = 0; i < 50; i++) cpu_step(&cpu);

    /* A should be $55 from NMI handler, and we should be back at main loop */
    if (cpu.A == 0x55) {
        printf("TEST nmi: PASS (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 1;
    } else {
        printf("TEST nmi: FAIL (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 0;
    }
}

int test_irq(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* Main program: CLI, loop forever */
    uint8_t prog[] = { 0x58, 0xA9, 0x00, 0x4C, 0x03, 0x02 };  /* CLI, LDA #$00, JMP $0203 */
    memcpy(&memory[0x200], prog, sizeof(prog));

    /* IRQ handler at $0400: LDA #$77, RTI */
    uint8_t irq_handler[] = { 0xA9, 0x77, 0x40 };
    memcpy(&memory[0x400], irq_handler, sizeof(irq_handler));

    /* Set IRQ vector */
    memory[0xFFFE] = 0x00;  /* Low byte */
    memory[0xFFFF] = 0x04;  /* High byte -> $0400 */

    cpu.PC = 0x200;
    cpu.uPC = 0;

    /* Run CLI and a few more instructions */
    for (int i = 0; i < 15; i++) cpu_step(&cpu);

    cpu.irq_pending = 1;  /* Trigger IRQ (should work since I flag is clear) */

    /* Run until IRQ handler executes */
    for (int i = 0; i < 50; i++) cpu_step(&cpu);

    if (cpu.A == 0x77) {
        printf("TEST irq: PASS (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 1;
    } else {
        printf("TEST irq: FAIL (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 0;
    }
}

int test_irq_masked(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* Main program: SEI, LDA #$42, loop forever (IRQ disabled) */
    uint8_t prog[] = { 0x78, 0xA9, 0x42, 0x4C, 0x03, 0x02 };  /* SEI, LDA #$42, JMP $0203 */
    memcpy(&memory[0x200], prog, sizeof(prog));

    /* IRQ handler at $0400: LDA #$99, RTI */
    uint8_t irq_handler[] = { 0xA9, 0x99, 0x40 };
    memcpy(&memory[0x400], irq_handler, sizeof(irq_handler));

    /* Set IRQ vector */
    memory[0xFFFE] = 0x00;
    memory[0xFFFF] = 0x04;

    cpu.PC = 0x200;
    cpu.uPC = 0;

    /* Run program */
    for (int i = 0; i < 20; i++) cpu_step(&cpu);

    cpu.irq_pending = 1;  /* Trigger IRQ (should be ignored due to SEI) */

    for (int i = 0; i < 30; i++) cpu_step(&cpu);

    /* A should still be $42, NOT $99, because IRQ is masked */
    if (cpu.A == 0x42) {
        printf("TEST irq_masked: PASS (A=%02X - IRQ correctly ignored)\n", cpu.A);
        return 1;
    } else {
        printf("TEST irq_masked: FAIL (A=%02X - IRQ should have been masked)\n", cpu.A);
        return 0;
    }
}

int test_reset(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* Program at reset vector $0500: LDA #$AA, NOP, NOP, NOP, NOP, NOP */
    uint8_t prog[] = { 0xA9, 0xAA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA, 0xEA };
    memcpy(&memory[0x500], prog, sizeof(prog));

    /* Set reset vector */
    memory[0xFFFC] = 0x00;
    memory[0xFFFD] = 0x05;  /* -> $0500 */

    /* Start somewhere else */
    cpu.PC = 0x200;
    cpu.uPC = 0;
    cpu.A = 0x00;

    /* Trigger reset */
    cpu.reset_pending = 1;

    /* Run reset sequence + program */
    for (int i = 0; i < 20; i++) cpu_step(&cpu);

    if (cpu.A == 0xAA && cpu.PC >= 0x500) {
        printf("TEST reset: PASS (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 1;
    } else {
        printf("TEST reset: FAIL (A=%02X PC=%04X)\n", cpu.A, cpu.PC);
        return 0;
    }
}

int test_jmp_ind_flags(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = mem_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));

    /* JMP ($02FF): the pointer increment wraps within the page, reading
     * $02FF then $0200 (which holds the $6C opcode). The increment is
     * internal address math and must not touch N/Z. */
    uint8_t prog[] = { 0x6C, 0xFF, 0x02 };
    memcpy(&memory[0x200], prog, sizeof(prog));
    memory[0x2FF] = 0x00;
    memory[0x6C00] = 0xEA;
    cpu.PC = 0x200;
    cpu.uPC = 0;
    cpu.P = 0x24;

    run_until_nop(&cpu, 100, 0);

    if (cpu.PC == 0x6C01 && cpu.P == 0x24) {
        printf("TEST jmp_ind_flags: PASS (PC=%04X P=%02X)\n", cpu.PC, cpu.P);
        return 1;
    } else {
        printf("TEST jmp_ind_flags: FAIL (PC=%04X P=%02X)\n", cpu.PC, cpu.P);
        return 0;
    }
}

static uint16_t read_log[16];
static int read_count;

static uint8_t logged_read(CPU *cpu, uint16_t addr) {
    (void)cpu;
    if (read_count < 16) read_log[read_count++] = addr;
    return memory[addr];
}

/* Run one instruction from $0200 and return how many bus reads it made,
 * or -1 if it has not finished within 20 cycles. */
static int trace_reads(CPU *cpu, const uint8_t *prog, size_t len) {
    memcpy(&memory[0x200], prog, len);
    cpu->PC = 0x200;
    cpu->uPC = 0;
    read_count = 0;
    int cycles = 0;
    do {
        if (++cycles > 20) return -1;
        cpu_step(cpu);
    } while (cpu->uPC != 0);
    return read_count;
}

int test_indexed_page_cross_reads(void) {
    CPU cpu;
    cpu_init(&cpu);
    cpu.mem_read = logged_read;
    cpu.mem_write = mem_write;
    memset(memory, 0, sizeof(memory));
    int ok = 1;

    /* LAS $10F0,Y with Y=$20 reads $1010 (uncorrected) then $1110, and
     * loads A/X/SP from the corrected read only. */
    memory[0x1010] = 0x00;
    memory[0x1110] = 0xF3;
    cpu.Y = 0x20;
    cpu.SP = 0x7F;
    const uint8_t las[] = { 0xBB, 0xF0, 0x10 };
    int n = trace_reads(&cpu, las, sizeof(las));
    if (n != 5 || read_log[3] != 0x1010 || read_log[4] != 0x1110 ||
        cpu.A != 0x73 || cpu.X != 0x73 || cpu.SP != 0x73) {
        printf("TEST page_cross_reads: FAIL LAS (reads=%d %04X %04X A=%02X SP=%02X)\n",
               n, read_log[3], read_log[4], cpu.A, cpu.SP);
        ok = 0;
    }

    /* NOP $10F0,X with X=$20: the same two reads. */
    cpu.X = 0x20;
    const uint8_t nop[] = { 0x1C, 0xF0, 0x10 };
    n = trace_reads(&cpu, nop, sizeof(nop));
    if (n != 5 || read_log[3] != 0x1010 || read_log[4] != 0x1110) {
        printf("TEST page_cross_reads: FAIL NOP (reads=%d %04X %04X)\n",
               n, read_log[3], read_log[4]);
        ok = 0;
    }

    if (ok) printf("TEST page_cross_reads: PASS\n");
    return ok;
}

/* One CPU cycle as the DMA logic sees it before the cycle runs, and the
 * bus accesses the cycle then made. */
typedef struct {
    int op;
    uint8_t xy;
    uint16_t upc;
    bool predicted_write;
    uint16_t peek;
    int reads, writes;
    uint16_t first_read;
} BusCycle;

static int write_count;

static void logged_write(CPU *cpu, uint16_t addr, uint8_t val) {
    (void)cpu;
    write_count++;
    memory[addr] = val;
}

/* Step every opcode cycle by cycle, with indexes that cross pages and
 * without, and hand each cycle to check(). Returns 0 if any check fails
 * or an instruction runs past 20 cycles; *cycles_out counts the cycles. */
static int for_each_bus_cycle(const char *name, int (*check)(const BusCycle *),
                              int *cycles_out) {
    static const struct { uint8_t xy, p; } setups[] = {
        { 0x00, 0x24 },  /* no page cross, branches on clear flags taken */
        { 0x20, 0x24 },  /* abs,X/abs,Y/(zp),Y cross; (zp,X) pointer moves */
        { 0x20, 0xE7 },  /* same, branches on set flags taken */
        { 0x0F, 0xE7 },  /* (zp,X) pointer high byte wraps within page 0 */
    };
    CPU cpu;
    int ok = 1, total = 0;

    for (int op = 0; op < 256; op++) {
        if (cpu_entry[op] == cpu_entry[0x02]) continue;  /* STP jams */
        for (size_t s = 0; s < sizeof(setups) / sizeof(setups[0]); s++) {
            cpu_init(&cpu);
            cpu.mem_read = logged_read;
            cpu.mem_write = logged_write;
            memset(memory, 0, sizeof(memory));
            /* Operand $10F0: abs $10F0, zp $F0, JMP ($10F0), branch -16
             * from $0202 into page 1. (zp),Y reads its pointer from
             * $F0/$F1 = $10F0; (zp,X) from $F0+X. */
            memory[0x200] = (uint8_t)op;
            memory[0x201] = 0xF0;
            memory[0x202] = 0x10;
            memory[0xF0] = 0xF0;
            memory[0xF1] = 0x10;
            memory[0xFF] = 0x34;
            memory[0x00] = 0x12;
            memory[0x10F0] = 0x80;
            memory[0x10F1] = 0x40;
            cpu.PC = 0x200;
            cpu.uPC = 0;
            cpu.X = cpu.Y = setups[s].xy;
            cpu.P = setups[s].p;
            cpu.SP = 0xF0;

            int cycles = 0;
            do {
                if (++cycles > 20) {
                    printf("TEST %s: FAIL op %02X did not finish in 20 cycles\n", name, op);
                    ok = 0;
                    break;
                }
                BusCycle c = { op, setups[s].xy, cpu.uPC, cpu_next_is_write(&cpu),
                               cpu_get_next_read_addr(&cpu), 0, 0, 0 };
                read_count = write_count = 0;
                cpu_step(&cpu);
                c.reads = read_count;
                c.writes = write_count;
                c.first_read = read_log[0];
                total++;
                if (!check(&c)) ok = 0;
            } while (cpu.uPC != 0);
        }
    }
    *cycles_out = total;
    return ok;
}

/* A DMA that halts the CPU repeats the read the CPU was about to make, at
 * the address cpu_get_next_read_addr() reports. */
static int check_next_read_addr(const BusCycle *c) {
    if (c->reads == 0 || c->first_read == c->peek) return 1;
    printf("TEST next_read_addr: FAIL op %02X X=Y=%02X uPC %d peek %04X, read %04X\n",
           c->op, c->xy, c->upc, c->peek, c->first_read);
    return 0;
}

int test_next_read_addr(void) {
    int cycles;
    int ok = for_each_bus_cycle("next_read_addr", check_next_read_addr, &cycles);
    if (ok) printf("TEST next_read_addr: PASS (%d cycles)\n", cycles);
    return ok;
}

/* DMA cannot halt the CPU on a write cycle, so cpu_next_is_write() must
 * name exactly the cycles that write. Every cycle is one bus access. */
static int check_next_is_write(const BusCycle *c) {
    if (c->reads + c->writes == 1 && c->predicted_write == (c->writes == 1)) return 1;
    printf("TEST next_is_write: FAIL op %02X X=Y=%02X uPC %d predicted %s, "
           "made %d reads %d writes\n", c->op, c->xy, c->upc,
           c->predicted_write ? "write" : "read", c->reads, c->writes);
    return 0;
}

int test_next_is_write(void) {
    int cycles;
    int ok = for_each_bus_cycle("next_is_write", check_next_is_write, &cycles);
    if (ok) printf("TEST next_is_write: PASS (%d cycles)\n", cycles);
    return ok;
}

int main(int argc, char **argv) {
    int trace = (argc > 1 && strcmp(argv[1], "-trace") == 0);
    (void)trace;

    printf("=== NES 6502 CPU Tests ===\n\n");

    int passed = 0;
    int total = 0;

    total++; passed += test_lda_sta();
    total++; passed += test_arithmetic();
    total++; passed += test_branch();
    total++; passed += test_stack();
    total++; passed += test_jsr_rts();
    total++; passed += test_shift();
    total++; passed += test_lax_unofficial();
    total++; passed += test_dcp_unofficial();
    total++; passed += test_indexed_indirect();
    total++; passed += test_indirect_indexed();
    total++; passed += test_nmi();
    total++; passed += test_irq();
    total++; passed += test_irq_masked();
    total++; passed += test_reset();
    total++; passed += test_jmp_ind_flags();
    total++; passed += test_indexed_page_cross_reads();
    total++; passed += test_next_read_addr();
    total++; passed += test_next_is_write();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
