/*
 * CPU Cycle Accuracy Test
 *
 * Verifies that cpu_step() advances exactly one cycle per call.
 * Also tests that each instruction takes the correct number of cycles.
 */

#include <stdio.h>
#include <string.h>
#include "cpu_gen.h"

static CPU cpu;
static uint8_t memory[0x10000];

static uint8_t test_read(CPU *c, uint16_t addr) {
    (void)c;
    return memory[addr];
}

static void test_write(CPU *c, uint16_t addr, uint8_t val) {
    (void)c;
    memory[addr] = val;
}

/* Run instruction until complete (uPC returns to 0) */
static int run_instruction(void) {
    int cycles = 0;
    uint64_t start_cycles = cpu.cycles;

    /* First call fetches the instruction */
    do {
        cpu_step(&cpu);
        cycles++;
        if (cycles > 20) {
            printf("ERROR: Instruction took more than 20 cycles!\n");
            return -1;
        }
    } while (cpu.uPC != 0);

    /* Verify internal cycle counter matches */
    int counted = (int)(cpu.cycles - start_cycles);
    if (counted != cycles) {
        printf("ERROR: cpu.cycles incremented by %d but we counted %d steps\n",
               counted, cycles);
        return -1;
    }

    return cycles;
}

/* Test structure */
typedef struct {
    const char *name;
    uint8_t opcode;
    int base_cycles;
    int setup_mode;  /* 0=none, 1=page cross, 2=branch taken, 3=branch+page */
} CycleTest;

/* Setup modes */
#define MODE_NONE       0
#define MODE_PAGE_CROSS 1
#define MODE_BRANCH     2
#define MODE_BRANCH_PG  3

/* Test cases - instruction name, opcode, expected cycles, setup mode */
static const CycleTest tests[] = {
    /* Implied */
    {"NOP", 0xEA, 2, MODE_NONE},
    {"TAX", 0xAA, 2, MODE_NONE},
    {"TAY", 0xA8, 2, MODE_NONE},
    {"TXA", 0x8A, 2, MODE_NONE},
    {"TYA", 0x98, 2, MODE_NONE},
    {"INX", 0xE8, 2, MODE_NONE},
    {"INY", 0xC8, 2, MODE_NONE},
    {"DEX", 0xCA, 2, MODE_NONE},
    {"DEY", 0x88, 2, MODE_NONE},
    {"TSX", 0xBA, 2, MODE_NONE},
    {"TXS", 0x9A, 2, MODE_NONE},
    {"CLC", 0x18, 2, MODE_NONE},
    {"SEC", 0x38, 2, MODE_NONE},
    {"CLI", 0x58, 2, MODE_NONE},
    {"SEI", 0x78, 2, MODE_NONE},
    {"CLV", 0xB8, 2, MODE_NONE},
    {"CLD", 0xD8, 2, MODE_NONE},
    {"SED", 0xF8, 2, MODE_NONE},

    /* Immediate */
    {"LDA #", 0xA9, 2, MODE_NONE},
    {"LDX #", 0xA2, 2, MODE_NONE},
    {"LDY #", 0xA0, 2, MODE_NONE},
    {"ADC #", 0x69, 2, MODE_NONE},
    {"SBC #", 0xE9, 2, MODE_NONE},
    {"AND #", 0x29, 2, MODE_NONE},
    {"ORA #", 0x09, 2, MODE_NONE},
    {"EOR #", 0x49, 2, MODE_NONE},
    {"CMP #", 0xC9, 2, MODE_NONE},
    {"CPX #", 0xE0, 2, MODE_NONE},
    {"CPY #", 0xC0, 2, MODE_NONE},

    /* Zero Page */
    {"LDA zp", 0xA5, 3, MODE_NONE},
    {"LDX zp", 0xA6, 3, MODE_NONE},
    {"LDY zp", 0xA4, 3, MODE_NONE},
    {"STA zp", 0x85, 3, MODE_NONE},
    {"STX zp", 0x86, 3, MODE_NONE},
    {"STY zp", 0x84, 3, MODE_NONE},
    {"ADC zp", 0x65, 3, MODE_NONE},
    {"SBC zp", 0xE5, 3, MODE_NONE},
    {"AND zp", 0x25, 3, MODE_NONE},
    {"ORA zp", 0x05, 3, MODE_NONE},
    {"EOR zp", 0x45, 3, MODE_NONE},
    {"CMP zp", 0xC5, 3, MODE_NONE},
    {"CPX zp", 0xE4, 3, MODE_NONE},
    {"CPY zp", 0xC4, 3, MODE_NONE},
    {"BIT zp", 0x24, 3, MODE_NONE},
    {"INC zp", 0xE6, 5, MODE_NONE},
    {"DEC zp", 0xC6, 5, MODE_NONE},
    {"ASL zp", 0x06, 5, MODE_NONE},
    {"LSR zp", 0x46, 5, MODE_NONE},
    {"ROL zp", 0x26, 5, MODE_NONE},
    {"ROR zp", 0x66, 5, MODE_NONE},

    /* Zero Page,X */
    {"LDA zp,X", 0xB5, 4, MODE_NONE},
    {"LDY zp,X", 0xB4, 4, MODE_NONE},
    {"STA zp,X", 0x95, 4, MODE_NONE},
    {"STY zp,X", 0x94, 4, MODE_NONE},
    {"ADC zp,X", 0x75, 4, MODE_NONE},
    {"SBC zp,X", 0xF5, 4, MODE_NONE},
    {"AND zp,X", 0x35, 4, MODE_NONE},
    {"ORA zp,X", 0x15, 4, MODE_NONE},
    {"EOR zp,X", 0x55, 4, MODE_NONE},
    {"CMP zp,X", 0xD5, 4, MODE_NONE},
    {"INC zp,X", 0xF6, 6, MODE_NONE},
    {"DEC zp,X", 0xD6, 6, MODE_NONE},
    {"ASL zp,X", 0x16, 6, MODE_NONE},
    {"LSR zp,X", 0x56, 6, MODE_NONE},
    {"ROL zp,X", 0x36, 6, MODE_NONE},
    {"ROR zp,X", 0x76, 6, MODE_NONE},

    /* Zero Page,Y */
    {"LDX zp,Y", 0xB6, 4, MODE_NONE},
    {"STX zp,Y", 0x96, 4, MODE_NONE},

    /* Absolute */
    {"LDA abs", 0xAD, 4, MODE_NONE},
    {"LDX abs", 0xAE, 4, MODE_NONE},
    {"LDY abs", 0xAC, 4, MODE_NONE},
    {"STA abs", 0x8D, 4, MODE_NONE},
    {"STX abs", 0x8E, 4, MODE_NONE},
    {"STY abs", 0x8C, 4, MODE_NONE},
    {"ADC abs", 0x6D, 4, MODE_NONE},
    {"SBC abs", 0xED, 4, MODE_NONE},
    {"AND abs", 0x2D, 4, MODE_NONE},
    {"ORA abs", 0x0D, 4, MODE_NONE},
    {"EOR abs", 0x4D, 4, MODE_NONE},
    {"CMP abs", 0xCD, 4, MODE_NONE},
    {"CPX abs", 0xEC, 4, MODE_NONE},
    {"CPY abs", 0xCC, 4, MODE_NONE},
    {"BIT abs", 0x2C, 4, MODE_NONE},
    {"INC abs", 0xEE, 6, MODE_NONE},
    {"DEC abs", 0xCE, 6, MODE_NONE},
    {"ASL abs", 0x0E, 6, MODE_NONE},
    {"LSR abs", 0x4E, 6, MODE_NONE},
    {"ROL abs", 0x2E, 6, MODE_NONE},
    {"ROR abs", 0x6E, 6, MODE_NONE},
    {"JMP abs", 0x4C, 3, MODE_NONE},

    /* Absolute,X (no page cross) */
    {"LDA abs,X", 0xBD, 4, MODE_NONE},
    {"LDY abs,X", 0xBC, 4, MODE_NONE},
    {"ADC abs,X", 0x7D, 4, MODE_NONE},
    {"SBC abs,X", 0xFD, 4, MODE_NONE},
    {"AND abs,X", 0x3D, 4, MODE_NONE},
    {"ORA abs,X", 0x1D, 4, MODE_NONE},
    {"EOR abs,X", 0x5D, 4, MODE_NONE},
    {"CMP abs,X", 0xDD, 4, MODE_NONE},

    /* Absolute,X (page cross = +1 cycle for reads) */
    {"LDA abs,X pg", 0xBD, 5, MODE_PAGE_CROSS},

    /* Absolute,X stores always take 5 cycles */
    {"STA abs,X", 0x9D, 5, MODE_NONE},
    {"INC abs,X", 0xFE, 7, MODE_NONE},
    {"DEC abs,X", 0xDE, 7, MODE_NONE},
    {"ASL abs,X", 0x1E, 7, MODE_NONE},
    {"LSR abs,X", 0x5E, 7, MODE_NONE},
    {"ROL abs,X", 0x3E, 7, MODE_NONE},
    {"ROR abs,X", 0x7E, 7, MODE_NONE},

    /* Absolute,Y (no page cross) */
    {"LDA abs,Y", 0xB9, 4, MODE_NONE},
    {"LDX abs,Y", 0xBE, 4, MODE_NONE},
    {"ADC abs,Y", 0x79, 4, MODE_NONE},
    {"SBC abs,Y", 0xF9, 4, MODE_NONE},
    {"AND abs,Y", 0x39, 4, MODE_NONE},
    {"ORA abs,Y", 0x19, 4, MODE_NONE},
    {"EOR abs,Y", 0x59, 4, MODE_NONE},
    {"CMP abs,Y", 0xD9, 4, MODE_NONE},

    /* Absolute,Y stores always take 5 cycles */
    {"STA abs,Y", 0x99, 5, MODE_NONE},

    /* Indexed Indirect (X) */
    {"LDA (zp,X)", 0xA1, 6, MODE_NONE},
    {"STA (zp,X)", 0x81, 6, MODE_NONE},
    {"ADC (zp,X)", 0x61, 6, MODE_NONE},
    {"SBC (zp,X)", 0xE1, 6, MODE_NONE},
    {"AND (zp,X)", 0x21, 6, MODE_NONE},
    {"ORA (zp,X)", 0x01, 6, MODE_NONE},
    {"EOR (zp,X)", 0x41, 6, MODE_NONE},
    {"CMP (zp,X)", 0xC1, 6, MODE_NONE},

    /* Indirect Indexed (Y) - no page cross */
    {"LDA (zp),Y", 0xB1, 5, MODE_NONE},
    {"ADC (zp),Y", 0x71, 5, MODE_NONE},
    {"SBC (zp),Y", 0xF1, 5, MODE_NONE},
    {"AND (zp),Y", 0x31, 5, MODE_NONE},
    {"ORA (zp),Y", 0x11, 5, MODE_NONE},
    {"EOR (zp),Y", 0x51, 5, MODE_NONE},
    {"CMP (zp),Y", 0xD1, 5, MODE_NONE},

    /* Indirect Indexed (Y) stores always take 6 cycles */
    {"STA (zp),Y", 0x91, 6, MODE_NONE},

    /* Indirect */
    {"JMP (abs)", 0x6C, 5, MODE_NONE},

    /* Stack */
    {"PHA", 0x48, 3, MODE_NONE},
    {"PHP", 0x08, 3, MODE_NONE},
    {"PLA", 0x68, 4, MODE_NONE},
    {"PLP", 0x28, 4, MODE_NONE},

    /* Subroutine */
    {"JSR", 0x20, 6, MODE_NONE},
    {"RTS", 0x60, 6, MODE_NONE},
    {"RTI", 0x40, 6, MODE_NONE},

    /* Branches (not taken) - must set flags to NOT match condition */
    {"BCC nt", 0x90, 2, MODE_NONE},  /* C=1 so not taken */
    {"BCS nt", 0xB0, 2, MODE_NONE},  /* C=0 so not taken */
    {"BEQ nt", 0xF0, 2, MODE_NONE},  /* Z=0 so not taken */
    {"BNE nt", 0xD0, 2, MODE_NONE},  /* Z=1 so not taken */
    {"BPL nt", 0x10, 2, MODE_NONE},  /* N=1 so not taken */
    {"BMI nt", 0x30, 2, MODE_NONE},  /* N=0 so not taken */
    {"BVC nt", 0x50, 2, MODE_NONE},  /* V=1 so not taken */
    {"BVS nt", 0x70, 2, MODE_NONE},  /* V=0 so not taken */

    /* Branches (taken, same page) */
    {"BNE tk", 0xD0, 3, MODE_BRANCH},

    /* Branches (taken, page cross) */
    {"BNE pg", 0xD0, 4, MODE_BRANCH_PG},

    /* Accumulator */
    {"ASL A", 0x0A, 2, MODE_NONE},
    {"LSR A", 0x4A, 2, MODE_NONE},
    {"ROL A", 0x2A, 2, MODE_NONE},
    {"ROR A", 0x6A, 2, MODE_NONE},

    /* BRK */
    {"BRK", 0x00, 7, MODE_NONE},

    /* JMP */
    {"JMP abs", 0x4C, 3, MODE_NONE},
    {"JMP ind", 0x6C, 5, MODE_NONE},

    /* Unofficial: same timing as the official ops sharing the mode. Every
     * opcode of each microcode state is listed, so a mapping slip in the
     * opcode table shows up here. */
    {"NOP #", 0x80, 2, MODE_NONE},
    {"NOP #", 0x82, 2, MODE_NONE},
    {"NOP #", 0x89, 2, MODE_NONE},
    {"NOP #", 0xC2, 2, MODE_NONE},
    {"NOP #", 0xE2, 2, MODE_NONE},
    {"NOP zp", 0x04, 3, MODE_NONE},
    {"NOP zp", 0x44, 3, MODE_NONE},
    {"NOP zp", 0x64, 3, MODE_NONE},
    {"NOP zp,X", 0x14, 4, MODE_NONE},
    {"NOP zp,X", 0x34, 4, MODE_NONE},
    {"NOP zp,X", 0x54, 4, MODE_NONE},
    {"NOP zp,X", 0x74, 4, MODE_NONE},
    {"NOP zp,X", 0xD4, 4, MODE_NONE},
    {"NOP zp,X", 0xF4, 4, MODE_NONE},
    {"NOP abs", 0x0C, 4, MODE_NONE},
    {"NOP abs,X", 0x1C, 4, MODE_NONE},
    {"NOP abs,X pg", 0x1C, 5, MODE_PAGE_CROSS},
    {"NOP abs,X", 0x3C, 4, MODE_NONE},
    {"NOP abs,X pg", 0x3C, 5, MODE_PAGE_CROSS},
    {"NOP abs,X", 0x5C, 4, MODE_NONE},
    {"NOP abs,X pg", 0x5C, 5, MODE_PAGE_CROSS},
    {"NOP abs,X", 0x7C, 4, MODE_NONE},
    {"NOP abs,X pg", 0x7C, 5, MODE_PAGE_CROSS},
    {"NOP abs,X", 0xDC, 4, MODE_NONE},
    {"NOP abs,X pg", 0xDC, 5, MODE_PAGE_CROSS},
    {"NOP abs,X", 0xFC, 4, MODE_NONE},
    {"NOP abs,X pg", 0xFC, 5, MODE_PAGE_CROSS},
    {"ANC #", 0x0B, 2, MODE_NONE},
    {"ANC #", 0x2B, 2, MODE_NONE},
    {"ALR #", 0x4B, 2, MODE_NONE},
    {"ARR #", 0x6B, 2, MODE_NONE},
    {"XAA #", 0x8B, 2, MODE_NONE},
    {"AXS #", 0xCB, 2, MODE_NONE},
    {"LAS abs,Y", 0xBB, 4, MODE_NONE},
    {"LAS abs,Y pg", 0xBB, 5, MODE_PAGE_CROSS},

    {NULL, 0, 0, 0}
};

static void setup_test(const CycleTest *test) {
    memset(memory, 0xEA, sizeof(memory));  /* Fill with NOP */

    /* Reset CPU state */
    cpu_init(&cpu);
    cpu.mem_read = test_read;
    cpu.mem_write = test_write;
    cpu.PC = 0x8000;
    cpu.SP = 0xFD;
    cpu.P = 0x24;  /* Default flags */

    /* Setup reset vector */
    memory[0xFFFC] = 0x00;
    memory[0xFFFD] = 0x80;

    /* Setup IRQ/NMI vectors for BRK/RTI */
    memory[0xFFFE] = 0x00;
    memory[0xFFFF] = 0x90;

    /* Put instruction at PC */
    memory[0x8000] = test->opcode;
    memory[0x8001] = 0x50;  /* operand 1 */
    memory[0x8002] = 0x80;  /* operand 2 (high byte for absolute) */

    /* Setup zero page pointer for indirect modes */
    memory[0x50] = 0x00;
    memory[0x51] = 0x81;  /* Points to $8100 */

    switch (test->setup_mode) {
    case MODE_NONE:
        cpu.X = 0;
        cpu.Y = 0;
        /* Set flags so branches are NOT taken by default */
        if (test->opcode == 0x90) cpu.P |= 0x01;  /* BCC: set C so not taken */
        if (test->opcode == 0xD0) cpu.P |= 0x02;  /* BNE: set Z so not taken */
        if (test->opcode == 0x10) cpu.P |= 0x80;  /* BPL: set N so not taken */
        if (test->opcode == 0x50) cpu.P |= 0x40;  /* BVC: set V so not taken */
        break;

    case MODE_PAGE_CROSS:
        /* Set X/Y to cause page cross: $8050 + $C0 = $8110 (crosses page) */
        cpu.X = 0xC0;
        cpu.Y = 0xC0;
        break;

    case MODE_BRANCH:
        /* Set flags to take the branch */
        if (test->opcode == 0xD0) cpu.P &= ~0x02;  /* BNE: clear Z */
        if (test->opcode == 0xF0) cpu.P |= 0x02;   /* BEQ: set Z */
        if (test->opcode == 0x90) cpu.P &= ~0x01;  /* BCC: clear C */
        if (test->opcode == 0xB0) cpu.P |= 0x01;   /* BCS: set C */
        if (test->opcode == 0x10) cpu.P &= ~0x80;  /* BPL: clear N */
        if (test->opcode == 0x30) cpu.P |= 0x80;   /* BMI: set N */
        if (test->opcode == 0x50) cpu.P &= ~0x40;  /* BVC: clear V */
        if (test->opcode == 0x70) cpu.P |= 0x40;   /* BVS: set V */
        memory[0x8001] = 0x10;  /* Branch offset: forward 16 bytes */
        break;

    case MODE_BRANCH_PG:
        /* Branch that crosses page */
        cpu.PC = 0x80F0;  /* Near end of page */
        memory[0x80F0] = test->opcode;
        memory[0x80F1] = 0x20;  /* Branch forward 32 bytes -> $8112 (crosses) */
        /* Set flags to take the branch */
        if (test->opcode == 0xD0) cpu.P &= ~0x02;
        break;
    }

    /* For JSR, set up return address */
    if (test->opcode == 0x20) {
        memory[0x8001] = 0x00;
        memory[0x8002] = 0x90;  /* JSR $9000 */
    }

    /* For RTS, push return address - 1 */
    if (test->opcode == 0x60) {
        memory[0x01FD] = 0x90;  /* High byte */
        memory[0x01FC] = 0xFF;  /* Low byte - 1 */
        cpu.SP = 0xFB;
    }

    /* For RTI, push flags and return address */
    if (test->opcode == 0x40) {
        memory[0x01FD] = 0x90;  /* High byte */
        memory[0x01FC] = 0x00;  /* Low byte */
        memory[0x01FB] = 0x24;  /* Flags */
        cpu.SP = 0xFA;
    }

    /* For PLA/PLP, push a value */
    if (test->opcode == 0x68 || test->opcode == 0x28) {
        memory[0x01FD] = 0x42;
        cpu.SP = 0xFC;
    }
}

int main(void) {
    int passed = 0;
    int failed = 0;

    printf("=== CPU Cycle Accuracy Tests ===\n\n");

    for (const CycleTest *test = tests; test->name != NULL; test++) {
        setup_test(test);

        int cycles = run_instruction();

        if (cycles == test->base_cycles) {
            printf("  PASS: %-15s opcode=%02X  cycles=%d\n",
                   test->name, test->opcode, cycles);
            passed++;
        } else {
            printf("  FAIL: %-15s opcode=%02X  expected=%d  got=%d\n",
                   test->name, test->opcode, test->base_cycles, cycles);
            failed++;
        }
    }

    printf("\n=== Summary ===\n");
    printf("Passed: %d\n", passed);
    printf("Failed: %d\n", failed);

    /* Test that cpu_step increments cycles exactly once per call */
    printf("\n=== Testing cpu_step cycle increment ===\n");
    cpu_init(&cpu);
    cpu.mem_read = test_read;
    cpu.mem_write = test_write;
    cpu.PC = 0x8000;
    memory[0x8000] = 0xEA;  /* NOP */
    memory[0x8001] = 0xEA;  /* NOP */

    uint64_t before = cpu.cycles;
    cpu_step(&cpu);
    uint64_t after = cpu.cycles;

    if (after - before == 1) {
        printf("  PASS: cpu_step increments cycles by 1\n");
    } else {
        printf("  FAIL: cpu_step incremented cycles by %llu (expected 1)\n",
               (unsigned long long)(after - before));
        failed++;
    }

    /* Run 10 more steps and verify cycle count */
    before = cpu.cycles;
    for (int i = 0; i < 10; i++) {
        cpu_step(&cpu);
    }
    after = cpu.cycles;

    if (after - before == 10) {
        printf("  PASS: 10 cpu_step calls increment cycles by 10\n");
    } else {
        printf("  FAIL: 10 cpu_step calls incremented cycles by %llu\n",
               (unsigned long long)(after - before));
        failed++;
    }

    printf("\n=== Final Result: %s ===\n", failed == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED");

    return failed > 0 ? 1 : 0;
}
