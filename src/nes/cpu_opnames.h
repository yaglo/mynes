/*
 * 6502 Opcode Tables
 *
 * Mnemonic names, addressing modes, and instruction sizes for all 256 opcodes.
 * Includes official and unofficial (marked with *) instructions.
 */

#ifndef NES_CPU_OPNAMES_H
#define NES_CPU_OPNAMES_H

#include <stdint.h>
#include <stddef.h>

/* Addressing modes */
typedef enum {
    AM_IMP, /* Implied / Accumulator (1 byte) */
    AM_IMM, /* Immediate #$xx (2 bytes) */
    AM_ZP,  /* Zero Page $xx (2 bytes) */
    AM_ZPX, /* Zero Page,X $xx,X (2 bytes) */
    AM_ZPY, /* Zero Page,Y $xx,Y (2 bytes) */
    AM_ABS, /* Absolute $xxxx (3 bytes) */
    AM_ABX, /* Absolute,X $xxxx,X (3 bytes) */
    AM_ABY, /* Absolute,Y $xxxx,Y (3 bytes) */
    AM_IND, /* Indirect ($xxxx) (3 bytes) */
    AM_IZX, /* Indexed Indirect ($xx,X) (2 bytes) */
    AM_IZY, /* Indirect Indexed ($xx),Y (2 bytes) */
    AM_REL, /* Relative $xx (2 bytes) */
} CPUAddressMode;

/* Opcode mnemonic names (NULL = undocumented KIL/JAM) */
static const char *cpu_op_name[256] = {
    /*       x0     x1     x2     x3     x4     x5     x6     x7     x8     x9     xA     xB     xC     xD     xE     xF */
    /*0x*/ "BRK", "ORA",  NULL, "*SLO","*NOP", "ORA", "ASL", "*SLO", "PHP", "ORA", "ASL", "*ANC","*NOP", "ORA", "ASL", "*SLO",
    /*1x*/ "BPL", "ORA",  NULL, "*SLO","*NOP", "ORA", "ASL", "*SLO", "CLC", "ORA", "*NOP","*SLO","*NOP", "ORA", "ASL", "*SLO",
    /*2x*/ "JSR", "AND",  NULL, "*RLA", "BIT", "AND", "ROL", "*RLA", "PLP", "AND", "ROL", "*ANC", "BIT", "AND", "ROL", "*RLA",
    /*3x*/ "BMI", "AND",  NULL, "*RLA","*NOP", "AND", "ROL", "*RLA", "SEC", "AND", "*NOP","*RLA","*NOP", "AND", "ROL", "*RLA",
    /*4x*/ "RTI", "EOR",  NULL, "*SRE","*NOP", "EOR", "LSR", "*SRE", "PHA", "EOR", "LSR", "*ALR", "JMP", "EOR", "LSR", "*SRE",
    /*5x*/ "BVC", "EOR",  NULL, "*SRE","*NOP", "EOR", "LSR", "*SRE", "CLI", "EOR", "*NOP","*SRE","*NOP", "EOR", "LSR", "*SRE",
    /*6x*/ "RTS", "ADC",  NULL, "*RRA","*NOP", "ADC", "ROR", "*RRA", "PLA", "ADC", "ROR", "*ARR", "JMP", "ADC", "ROR", "*RRA",
    /*7x*/ "BVS", "ADC",  NULL, "*RRA","*NOP", "ADC", "ROR", "*RRA", "SEI", "ADC", "*NOP","*RRA","*NOP", "ADC", "ROR", "*RRA",
    /*8x*/"*NOP", "STA", "*NOP","*SAX", "STY", "STA", "STX", "*SAX", "DEY", "*NOP", "TXA", "*XAA", "STY", "STA", "STX", "*SAX",
    /*9x*/ "BCC", "STA",  NULL, "*SHA", "STY", "STA", "STX", "*SAX", "TYA", "STA", "TXS", "*TAS","*SHY", "STA", "*SHX","*SHA",
    /*Ax*/ "LDY", "LDA", "LDX", "*LAX", "LDY", "LDA", "LDX", "*LAX", "TAY", "LDA", "TAX", "*ATX", "LDY", "LDA", "LDX", "*LAX",
    /*Bx*/ "BCS", "LDA",  NULL, "*LAX", "LDY", "LDA", "LDX", "*LAX", "CLV", "LDA", "TSX", "*LAS", "LDY", "LDA", "LDX", "*LAX",
    /*Cx*/ "CPY", "CMP", "*NOP","*DCP", "CPY", "CMP", "DEC", "*DCP", "INY", "CMP", "DEX", "*AXS", "CPY", "CMP", "DEC", "*DCP",
    /*Dx*/ "BNE", "CMP",  NULL, "*DCP","*NOP", "CMP", "DEC", "*DCP", "CLD", "CMP", "*NOP","*DCP","*NOP", "CMP", "DEC", "*DCP",
    /*Ex*/ "CPX", "SBC", "*NOP","*ISB", "CPX", "SBC", "INC", "*ISB", "INX", "SBC", "NOP", "*SBC", "CPX", "SBC", "INC", "*ISB",
    /*Fx*/ "BEQ", "SBC",  NULL, "*ISB","*NOP", "SBC", "INC", "*ISB", "SED", "SBC", "*NOP","*ISB","*NOP", "SBC", "INC", "*ISB",
};

/* Addressing mode for each opcode */
static const CPUAddressMode cpu_op_mode[256] = {
    /*       x0      x1      x2      x3      x4      x5      x6      x7      x8      x9      xA      xB      xC      xD      xE      xF */
    /*0x*/ AM_IMP, AM_IZX, AM_IMP, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_ABS, AM_ABS, AM_ABS, AM_ABS,
    /*1x*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPX, AM_ZPX, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABX, AM_ABX,
    /*2x*/ AM_ABS, AM_IZX, AM_IMP, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_ABS, AM_ABS, AM_ABS, AM_ABS,
    /*3x*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPX, AM_ZPX, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABX, AM_ABX,
    /*4x*/ AM_IMP, AM_IZX, AM_IMP, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_ABS, AM_ABS, AM_ABS, AM_ABS,
    /*5x*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPX, AM_ZPX, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABX, AM_ABX,
    /*6x*/ AM_IMP, AM_IZX, AM_IMP, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_IND, AM_ABS, AM_ABS, AM_ABS,
    /*7x*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPX, AM_ZPX, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABX, AM_ABX,
    /*8x*/ AM_IMM, AM_IZX, AM_IMM, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_ABS, AM_ABS, AM_ABS, AM_ABS,
    /*9x*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPY, AM_ZPY, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABY, AM_ABY,
    /*Ax*/ AM_IMM, AM_IZX, AM_IMM, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_ABS, AM_ABS, AM_ABS, AM_ABS,
    /*Bx*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPY, AM_ZPY, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABY, AM_ABY,
    /*Cx*/ AM_IMM, AM_IZX, AM_IMM, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_ABS, AM_ABS, AM_ABS, AM_ABS,
    /*Dx*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPX, AM_ZPX, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABX, AM_ABX,
    /*Ex*/ AM_IMM, AM_IZX, AM_IMM, AM_IZX, AM_ZP,  AM_ZP,  AM_ZP,  AM_ZP,  AM_IMP, AM_IMM, AM_IMP, AM_IMM, AM_ABS, AM_ABS, AM_ABS, AM_ABS,
    /*Fx*/ AM_REL, AM_IZY, AM_IMP, AM_IZY, AM_ZPX, AM_ZPX, AM_ZPX, AM_ZPX, AM_IMP, AM_ABY, AM_IMP, AM_ABY, AM_ABX, AM_ABX, AM_ABX, AM_ABX,
};

/* Instruction size in bytes (1, 2, or 3) */
static const uint8_t cpu_op_size[256] = {
    /*       x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF */
    /*0x*/   1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*1x*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    /*2x*/   3, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*3x*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    /*4x*/   1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*5x*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    /*6x*/   1, 2, 1, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*7x*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    /*8x*/   2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*9x*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    /*Ax*/   2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*Bx*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    /*Cx*/   2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*Dx*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
    /*Ex*/   2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 1, 2, 3, 3, 3, 3,
    /*Fx*/   2, 2, 1, 2, 2, 2, 2, 2, 1, 3, 1, 3, 3, 3, 3, 3,
};

#endif /* NES_CPU_OPNAMES_H */
