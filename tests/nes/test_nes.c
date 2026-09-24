#include <stdio.h>
#include <string.h>
#include "nes/nes.h"
#include "nes/rom.h"

static NES nes;

/* Simple test ROM in PRG space */
static uint8_t test_rom[0x8000];

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

static void setup(void) {
    memset(test_rom, 0, sizeof(test_rom));
    nes_init(&nes);
    nes_load_prg(&nes, test_rom, sizeof(test_rom));
}

/* Write a program to ROM starting at given address */
static void write_program(uint16_t addr, uint8_t *prog, size_t len) {
    /* addr is CPU address ($8000+), convert to ROM offset */
    uint16_t offset = addr - 0x8000;
    memcpy(&test_rom[offset], prog, len);
}

/* Set reset vector */
static void set_reset_vector(uint16_t addr) {
    /* Reset vector at $FFFC-$FFFD (ROM offset $7FFC-$7FFD) */
    test_rom[0x7FFC] = addr & 0xFF;
    test_rom[0x7FFD] = (addr >> 8) & 0xFF;
}

/* Set NMI vector */
static void set_nmi_vector(uint16_t addr) {
    /* NMI vector at $FFFA-$FFFB */
    test_rom[0x7FFA] = addr & 0xFF;
    test_rom[0x7FFB] = (addr >> 8) & 0xFF;
}

/* Run for N CPU cycles */
static void run_cycles(int n) {
    for (int i = 0; i < n; i++) {
        nes_step(&nes);
    }
}

/* ============================================================================
 * Tests
 * ============================================================================ */

int test_ram_access(void) {
    setup();

    /* Program: LDA #$42, STA $00, LDA $00, STA $01, NOP */
    uint8_t prog[] = { 0xA9, 0x42, 0x85, 0x00, 0xA5, 0x00, 0x85, 0x01, 0xEA };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);
    run_cycles(50); /* Run reset + program */

    if (nes.ram[0x00] == 0x42 && nes.ram[0x01] == 0x42) {
        printf("TEST ram_access: PASS (ram[0]=%02X ram[1]=%02X)\n",
               nes.ram[0x00], nes.ram[0x01]);
        return 1;
    } else {
        printf("TEST ram_access: FAIL (ram[0]=%02X ram[1]=%02X)\n",
               nes.ram[0x00], nes.ram[0x01]);
        return 0;
    }
}

int test_ppu_register_access(void) {
    setup();

    /* Program: LDA #$80, STA $2000 (enable NMI), LDA $2002, NOP */
    uint8_t prog[] = { 0xA9, 0x80, 0x8D, 0x00, 0x20, 0xAD, 0x02, 0x20, 0xEA };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);
    run_cycles(50);

    if (nes.ppu.ctrl == 0x80) {
        printf("TEST ppu_register_access: PASS (PPUCTRL=%02X)\n", nes.ppu.ctrl);
        return 1;
    } else {
        printf("TEST ppu_register_access: FAIL (PPUCTRL=%02X expected 80)\n", nes.ppu.ctrl);
        return 0;
    }
}

int test_ppu_scroll_write(void) {
    setup();

    /* Program: LDA #$10, STA $2005, LDA #$20, STA $2005, NOP */
    /* Sets X scroll to $10, Y scroll to $20 */
    uint8_t prog[] = {
        0xA9, 0x10, 0x8D, 0x05, 0x20,  /* LDA #$10, STA $2005 */
        0xA9, 0x20, 0x8D, 0x05, 0x20,  /* LDA #$20, STA $2005 */
        0xEA
    };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);
    run_cycles(60);

    /* X scroll: coarse = $10 >> 3 = 2, fine = $10 & 7 = 0 */
    /* Y scroll: coarse = $20 >> 3 = 4, fine = $20 & 7 = 0 */
    uint8_t fine_x = nes.ppu.x;
    uint16_t coarse_x = nes.ppu.t & 0x1F;
    uint16_t coarse_y = (nes.ppu.t >> 5) & 0x1F;
    uint16_t fine_y = (nes.ppu.t >> 12) & 0x07;

    if (coarse_x == 2 && fine_x == 0 && coarse_y == 4 && fine_y == 0) {
        printf("TEST ppu_scroll_write: PASS (coarse_x=%d fine_x=%d coarse_y=%d fine_y=%d)\n",
               coarse_x, fine_x, coarse_y, fine_y);
        return 1;
    } else {
        printf("TEST ppu_scroll_write: FAIL (coarse_x=%d fine_x=%d coarse_y=%d fine_y=%d)\n",
               coarse_x, fine_x, coarse_y, fine_y);
        return 0;
    }
}

int test_pal_clock_ratio(void) {
    setup();
    uint8_t prog[] = {0x4C, 0x00, 0x80};
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);
    nes_set_region(&nes, NES_REGION_PAL);
    nes_reset(&nes);
    run_cycles(1000);
    int pass = nes.master_tick == 16000 &&
        nes.ppu.next_dot_master_tick == 16000 &&
        nes.ppu.scanline * 341 + nes.ppu.dot == 3200;
    /* Five complete PAL frames are exactly 166237.5 CPU cycles;
     * the rounded end is at cycle 166238, with 532762 PPU dots. */
    run_cycles(166238 - 1000);
    pass &= nes.ppu.frame == 5 && nes.ppu.scanline == 0 && nes.ppu.dot == 2;
    pass &= nes.apu.cpu_clock == APU_CPU_CLOCK_PAL;
    printf("TEST pal_clock_ratio: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_timing_ratio(void) {
    setup();

    /* Program: infinite loop */
    uint8_t prog[] = { 0x4C, 0x00, 0x80 }; /* JMP $8000 */
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);

    /* Run reset sequence */
    run_cycles(10);

    /* Record starting positions */
    uint64_t cpu_start = nes.cpu.cycles;

    /* Run for a bunch of cycles */
    run_cycles(1000);

    uint64_t cpu_elapsed = nes.cpu.cycles - cpu_start;

    /* PPU should have run 3x as many dots */
    /* We can check frame count or just verify PPU advanced */

    if (cpu_elapsed == 1000) {
        printf("TEST timing_ratio: PASS (1000 CPU cycles executed)\n");
        return 1;
    } else {
        printf("TEST timing_ratio: FAIL (expected 1000 CPU cycles, got %llu)\n",
               (unsigned long long)cpu_elapsed);
        return 0;
    }
}

int test_nmi_fires(void) {
    setup();

    /* Main program: enable NMI, loop forever */
    uint8_t main_prog[] = {
        0xA9, 0x80, 0x8D, 0x00, 0x20,  /* LDA #$80, STA $2000 (enable NMI) */
        0xA9, 0x00,                     /* LDA #$00 */
        0x4C, 0x07, 0x80               /* JMP $8007 (loop) */
    };
    write_program(0x8000, main_prog, sizeof(main_prog));

    /* NMI handler: LDA #$55, store to RAM, RTI */
    uint8_t nmi_handler[] = {
        0xA9, 0x55,                     /* LDA #$55 */
        0x85, 0x10,                     /* STA $10 */
        0x40                            /* RTI */
    };
    write_program(0x9000, nmi_handler, sizeof(nmi_handler));

    set_reset_vector(0x8000);
    set_nmi_vector(0x9000);

    nes_reset(&nes);

    /* Run until NMI should have fired (after vblank starts) */
    /* VBlank starts at scanline 241, dot 1 */
    /* That's about 241 * 341 = 82181 PPU dots = ~27394 CPU cycles */
    /* Plus reset sequence */
    run_cycles(30000);

    if (nes.ram[0x10] == 0x55) {
        printf("TEST nmi_fires: PASS (NMI handler executed, ram[10]=%02X)\n", nes.ram[0x10]);
        return 1;
    } else {
        printf("TEST nmi_fires: FAIL (NMI handler not executed, ram[10]=%02X)\n", nes.ram[0x10]);
        return 0;
    }
}

int test_controller_read(void) {
    setup();

    /* Set controller state */
    nes_set_controller(&nes, 0, BTN_A | BTN_START);

    /* Program: strobe controller, read 8 bits */
    uint8_t prog[] = {
        0xA9, 0x01, 0x8D, 0x16, 0x40,  /* LDA #$01, STA $4016 (strobe high) */
        0xA9, 0x00, 0x8D, 0x16, 0x40,  /* LDA #$00, STA $4016 (strobe low) */
        0xAD, 0x16, 0x40,              /* LDA $4016 (read A) */
        0x85, 0x00,                     /* STA $00 */
        0xAD, 0x16, 0x40,              /* LDA $4016 (read B) */
        0x85, 0x01,                     /* STA $01 */
        0xAD, 0x16, 0x40,              /* LDA $4016 (read Select) */
        0x85, 0x02,                     /* STA $02 */
        0xAD, 0x16, 0x40,              /* LDA $4016 (read Start) */
        0x85, 0x03,                     /* STA $03 */
        0xEA
    };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);
    run_cycles(100);

    /* A and Start should be 1, B and Select should be 0 */
    /* Note: buttons are read MSB first (A=bit7, B=bit6, etc) */
    if ((nes.ram[0] & 1) == 1 &&    /* A pressed */
        (nes.ram[1] & 1) == 0 &&    /* B not pressed */
        (nes.ram[2] & 1) == 0 &&    /* Select not pressed */
        (nes.ram[3] & 1) == 1) {    /* Start pressed */
        printf("TEST controller_read: PASS (A=%d B=%d Sel=%d Start=%d)\n",
               nes.ram[0] & 1, nes.ram[1] & 1, nes.ram[2] & 1, nes.ram[3] & 1);
        return 1;
    } else {
        printf("TEST controller_read: FAIL (A=%d B=%d Sel=%d Start=%d)\n",
               nes.ram[0] & 1, nes.ram[1] & 1, nes.ram[2] & 1, nes.ram[3] & 1);
        return 0;
    }
}

int test_oam_dma(void) {
    setup();

    /* Fill RAM $0200-$02FF with test pattern AFTER setup clears RAM */
    for (int i = 0; i < 256; i++) {
        nes.ram[0x200 + i] = (uint8_t)i;
    }

    /* Program: trigger OAM DMA from page $02 */
    uint8_t prog[] = {
        0xA9, 0x02, 0x8D, 0x14, 0x40,  /* LDA #$02, STA $4014 */
        0xEA, 0xEA, 0xEA, 0xEA, 0xEA   /* NOPs for DMA to complete */
    };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);

    /* Re-fill RAM after reset clears it */
    for (int i = 0; i < 256; i++) {
        nes.ram[0x200 + i] = (uint8_t)i;
    }

    run_cycles(600); /* DMA takes ~513 cycles */

    /* Check OAM was copied */
    int match = 1;
    int first_mismatch = -1;
    for (int i = 0; i < 256; i++) {
        if (nes.ppu.oam[i] != (uint8_t)i) {
            if (first_mismatch < 0) first_mismatch = i;
            match = 0;
        }
    }

    if (match) {
        printf("TEST oam_dma: PASS (256 bytes transferred)\n");
        return 1;
    } else {
        printf("TEST oam_dma: FAIL (first mismatch at %d: got %02X expected %02X)\n",
               first_mismatch, nes.ppu.oam[first_mismatch], (uint8_t)first_mismatch);
        return 0;
    }
}

int test_full_frame(void) {
    setup();

    /* Simple program: enable rendering, loop forever */
    uint8_t prog[] = {
        0xA9, 0x1E, 0x8D, 0x01, 0x20,  /* LDA #$1E, STA $2001 (enable rendering) */
        0x4C, 0x05, 0x80               /* JMP $8005 (loop) */
    };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);

    /* Run one complete frame */
    nes_run_frame(&nes);

    if (nes.ppu.frame_complete && nes.ppu.frame >= 1) {
        printf("TEST full_frame: PASS (frame=%llu)\n", (unsigned long long)nes.ppu.frame);
        return 1;
    } else {
        printf("TEST full_frame: FAIL (frame_complete=%d frame=%llu)\n",
               nes.ppu.frame_complete, (unsigned long long)nes.ppu.frame);
        return 0;
    }
}

int test_vblank_flag_read(void) {
    setup();

    /* Program: wait for vblank by polling $2002 */
    uint8_t prog[] = {
        /* Wait for vblank */
        0xAD, 0x02, 0x20,              /* wait: LDA $2002 */
        0x10, 0xFB,                     /* BPL wait (loop until bit 7 set) */
        0xA9, 0x01,                     /* LDA #$01 */
        0x85, 0x00,                     /* STA $00 (flag that we saw vblank) */
        0x4C, 0x0A, 0x80               /* JMP $800A (loop) */
    };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);

    /* Run until well into vblank */
    run_cycles(35000);

    if (nes.ram[0] == 0x01) {
        printf("TEST vblank_flag_read: PASS (vblank detected)\n");
        return 1;
    } else {
        printf("TEST vblank_flag_read: FAIL (vblank not detected, ram[0]=%02X)\n", nes.ram[0]);
        return 0;
    }
}

int test_dmc_load_alignment(void) {
    bool pass = true;
    for (unsigned put = 0; put < 2; put++) {
        setup();
        nes.apu.put_cycle = put;
        nes.apu.dmc.sample_address = 0x8000;
        nes.apu.dmc.sample_length = 1;
        apu_write(&nes.apu, 0x4015, 0x10);
        unsigned delay = put ? 3 : 4;
        for (unsigned cycle = 1; cycle <= delay; cycle++) {
            apu_step(&nes.apu);
            pass &= apu_dmc_needs_sample(&nes.apu) == (cycle == delay);
        }
        pass &= !nes.apu.put_cycle; /* Load halts on a get. */
    }
    printf("TEST dmc_load_alignment: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_dmc_aborted_halt(void) {
    setup();
    nes.cpu.reset_pending = false;
    nes.cpu.PC = 0x8000;
    nes.apu.dmc.bytes_remaining = 1;
    nes.apu.dmc.sample_buffer_empty = false;
    nes.apu.dmc.bits_remaining = 1;
    nes.apu.dmc.timer = 2;
    apu_write(&nes.apu, 0x4015, 0);
    unsigned halted = 0;
    for (unsigned cycle = 0; cycle < 5; cycle++) {
        apu_step(&nes.apu);
        halted += nes_dma_step(&nes);
    }
    bool pass = halted == 1 && !nes.dma.dmc_active;

    /* A write cannot be halted; this pulse expires instead of retrying. */
    uint8_t prog[] = { 0x85, 0x10 }; /* STA $10 */
    write_program(0x8000, prog, sizeof(prog));
    nes.cpu.rdy = true;
    cpu_step(&nes.cpu);
    cpu_step(&nes.cpu);
    pass &= cpu_next_is_write(&nes.cpu);
    nes.apu.dmc_abort_pending = true;
    pass &= !nes_dma_step(&nes) && !nes.apu.dmc_abort_pending;
    printf("TEST dmc_aborted_halt: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_dmc_stop_during_request(void) {
    setup();
    nes.cpu.reset_pending = false;
    nes.cpu.PC = 0x8000;
    nes.apu.put_cycle = false;
    nes.apu.dmc.current_address = 0x8010;
    nes.apu.dmc.bytes_remaining = 1;
    nes.apu.dmc.sample_length = 1;
    nes.apu.dmc.loop_flag = true;
    test_rom[0x10] = 0xA5;
    apu_write(&nes.apu, 0x4015, 0);
    unsigned halted = 0;
    for (unsigned cycle = 0; cycle < 5; cycle++) {
        apu_step(&nes.apu);
        halted += nes_dma_step(&nes);
    }
    /* The pending fetch survives, but cannot restart a stopped loop. */
    bool pass = halted == 4 && nes.apu.dmc.sample_buffer == 0xA5 &&
                nes.apu.dmc.bytes_remaining == 0 && !apu_dmc_needs_sample(&nes.apu);
    printf("TEST dmc_stop_during_request: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

static unsigned traced_instructions;
static bool trace_matches;
static uint64_t last_trace_cycle;

static void check_instruction_trace(uint16_t pc, uint8_t opcode, uint64_t cycles) {
    trace_matches &= pc >= 0x8000 && pc <= 0x8003;
    trace_matches &= opcode == test_rom[pc & 0x7FFF];
    trace_matches &= cycles > last_trace_cycle;
    last_trace_cycle = cycles;
    traced_instructions++;
}

int test_trace_callback_toggle(void) {
    setup();
    uint8_t program[] = {0xEA, 0x4C, 0x00, 0x80}; /* NOP; JMP $8000 */
    write_program(0x8000, program, sizeof(program));
    set_reset_vector(0x8000);
    nes_reset(&nes);
    traced_instructions = 0;
    last_trace_cycle = 0;
    trace_matches = true;
    debug_hooks.on_cpu_step = NULL;
    run_cycles(31);
    debug_hooks.on_cpu_step = check_instruction_trace;
    run_cycles(50);
    bool pass = traced_instructions > 0 && trace_matches;
    unsigned count = traced_instructions;
    debug_hooks.on_cpu_step = NULL;
    run_cycles(31);
    pass &= traced_instructions == count;
    debug_hooks.on_cpu_step = check_instruction_trace;
    run_cycles(50);
    pass &= traced_instructions > count && trace_matches;
    debug_hooks.on_cpu_step = NULL;
    printf("TEST trace_callback_toggle: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

/* Boots a 32 KB PRG, 8 KB CHR cartridge from header bytes, as the frontends do. */
static void boot_cartridge(uint8_t flags6) {
    static uint8_t data[INES_HEADER_SIZE + 2 * INES_PRG_BANK_SIZE + INES_CHR_BANK_SIZE];
    static ROM rom;
    nes_rom_free(&rom);
    memcpy(data, "NES\x1A\x02\x01", 6);
    data[6] = flags6;
    if (nes_rom_load_data(&rom, data, sizeof(data)) != ROM_OK) return;
    nes_init(&nes);
    nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size,
                    rom.chr_rom, rom.chr_size, rom.mirroring);
    nes_reset(&nes);
}

/* Each of $2000/$2400/$2800/$2C00, and its $3000 mirror, reads back the
 * byte written to that table alone. */
static bool four_tables_distinct(void) {
    bool pass = true;
    for (uint16_t t = 0; t < 4; t++) {
        pass &= ppu_read(&nes.ppu, 0x2000 + t * 0x400 + 0x21) == 0x10 + t;
        pass &= ppu_read(&nes.ppu, 0x3000 + t * 0x400 + 0x21) == 0x10 + t;
    }
    return pass;
}

/* Flags 6 bit 3 (Rad Racer II's TVROM board) adds 2 KB of cartridge VRAM,
 * so the four nametables are separate memory. The board leaves the MMC3's
 * mirroring output unconnected: a $A000 write must not fold them back. */
int test_four_screen_nametables(void) {
    boot_cartridge(0x48);   /* mapper 4, four-screen */
    for (uint16_t t = 0; t < 4; t++)
        ppu_write(&nes.ppu, 0x2000 + t * 0x400 + 0x21, 0x10 + t);
    bool pass = nes.ppu.mirroring == 4 && four_tables_distinct();

    for (uint8_t val = 0; val < 2; val++) {   /* $A000: 0 = vertical, 1 = horizontal */
        nes_cpu_write(&nes.cpu, 0xA000, val);
        pass &= nes.ppu.mirroring == 4 && four_tables_distinct();
        run_cycles(10);
        pass &= nes.ppu.mirroring == 4 && four_tables_distinct();
    }

    /* Without bit 3 the same write does fold them: $2400 is $2000. */
    boot_cartridge(0x40);
    nes_cpu_write(&nes.cpu, 0xA000, 0x01);
    run_cycles(10);
    for (uint16_t t = 0; t < 4; t++)
        ppu_write(&nes.ppu, 0x2000 + t * 0x400 + 0x21, 0x10 + t);
    pass &= nes.ppu.mirroring == 0 && !four_tables_distinct() &&
            ppu_read(&nes.ppu, 0x2021) == 0x11;

    /* No MMC2 board has four-screen RAM, yet the common Punch-Out!! (U)
     * dump sets bit 3 (flags 6 = $99). The game switches the MMC2 between
     * horizontal and vertical, and that register must keep working. */
    boot_cartridge(0x98);
    nes_cpu_write(&nes.cpu, 0xF000, 0x00);
    run_cycles(10);
    pass &= nes.mapper.number == 9 && nes.ppu.mirroring == 1;

    printf("TEST four_screen_nametables: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int test_reset_from_kil(void) {
    setup();

    /* INC $10, KIL */
    uint8_t prog[] = { 0xE6, 0x10, 0x02 };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);
    run_cycles(50);
    bool pass = nes.ram[0x10] == 1 && nes.cpu.PC == 0x8003;

    /* Reset must escape the jam and drop any DMA the CPU was stuck behind. */
    nes.oam_dma_pending = true;
    nes_reset(&nes);
    run_cycles(50);
    pass &= !nes.cpu.reset_pending && !nes.oam_dma_pending && !nes.dma.oam_active;
    pass &= nes.ram[0x10] == 1 && nes.cpu.PC == 0x8003;

    printf("TEST reset_from_kil: %s (PC=%04X ram[10]=%02X)\n",
           pass ? "PASS" : "FAIL", nes.cpu.PC, nes.ram[0x10]);
    return pass;
}

int test_controller_strobe_held(void) {
    setup();

    /* Strobe high, spin while the buttons change, strobe low, read A. */
    uint8_t prog[] = {
        0xA9, 0x01, 0x8D, 0x16, 0x40,  /* LDA #$01, STA $4016 */
        0xA2, 0x20, 0xCA, 0xD0, 0xFD,  /* LDX #$20, loop: DEX, BNE loop */
        0xA9, 0x00, 0x8D, 0x16, 0x40,  /* LDA #$00, STA $4016 */
        0xAD, 0x16, 0x40, 0x85, 0x00,  /* LDA $4016, STA $00 */
        0x4C, 0x14, 0x80               /* JMP * */
    };
    write_program(0x8000, prog, sizeof(prog));
    set_reset_vector(0x8000);

    nes_reset(&nes);
    run_cycles(20);                     /* strobe is high, A released */
    nes_set_controller(&nes, 0, BTN_A);
    run_cycles(300);

    /* While strobe is high the shift register keeps reloading, so the press
     * made after the strobe went high is what gets read. */
    bool pass = (nes.ram[0] & 1) == 1;
    printf("TEST controller_strobe_held: %s (A=%d)\n", pass ? "PASS" : "FAIL",
           nes.ram[0] & 1);
    return pass;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(void) {
    printf("=== NES Integration Tests ===\n\n");

    int passed = 0;
    int total = 0;

    total++; passed += test_dmc_load_alignment();
    total++; passed += test_dmc_aborted_halt();
    total++; passed += test_dmc_stop_during_request();
    total++; passed += test_ram_access();
    total++; passed += test_ppu_register_access();
    total++; passed += test_ppu_scroll_write();
    total++; passed += test_timing_ratio();
    total++; passed += test_pal_clock_ratio();
    total++; passed += test_nmi_fires();
    total++; passed += test_controller_read();
    total++; passed += test_oam_dma();
    total++; passed += test_full_frame();
    total++; passed += test_vblank_flag_read();
    total++; passed += test_trace_callback_toggle();
    total++; passed += test_four_screen_nametables();
    total++; passed += test_reset_from_kil();
    total++; passed += test_controller_strobe_held();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
