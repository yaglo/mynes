#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/nes.h"

static uint8_t prg[8 * 0x4000];
static uint8_t chr[32 * 0x1000];

static void write_register(Mapper *m, uint16_t addr, uint8_t val) {
    for (int bit = 0; bit < 5; ++bit)
        mapper_cpu_write(m, addr, (val >> bit) & 1);
}

int main(void) {
    Mapper m;
    NES nes;
    memset(prg, 0, sizeof(prg));
    memset(chr, 0, sizeof(chr));
    mapper_init(&m, 1, prg, sizeof(prg), chr, sizeof(chr), 0);

    write_register(&m, 0x8000, 0x1E); /* 4KB CHR, fixed-last PRG, vertical */
    write_register(&m, 0xA000, 28);
    write_register(&m, 0xC000, 24);
    write_register(&m, 0xE000, 5);

    mapper_cpu_write(&m, 0xE000, 0x80);

    int pass = m.mmc1_shift == 0x10 && m.mmc1_shift_count == 0 &&
               m.mmc1_control == 0x1E && m.mmc1_chr_mode &&
               m.chr_bank0 == 28 && m.chr_bank1 == 24 &&
               m.prg_mode == 3 && m.prg_bank0 == 5 && m.prg_bank1 == 7 &&
               mapper_get_mirroring(&m) == 1;

    if (!pass) {
        printf("FAIL ctrl=%02X shift=%02X/%u chrmode=%u chr=%u/%u "
               "prgmode=%u prg=%u/%u mirror=%u\n",
               m.mmc1_control, m.mmc1_shift, m.mmc1_shift_count,
               m.mmc1_chr_mode, m.chr_bank0, m.chr_bank1, m.prg_mode,
               m.prg_bank0, m.prg_bank1, mapper_get_mirroring(&m));
    }

    /* Reset while in fixed-first mode must restore the raw PRG register as
     * the switchable low bank, rather than losing it with the old mapping. */
    write_register(&m, 0x8000, 0x08);
    write_register(&m, 0xE000, 3);
    mapper_cpu_write(&m, 0xA000, 0x80);
    if (m.prg_mode != 3 || m.prg_bank0 != 3 || m.prg_bank1 != 7)
        pass = 0;

    /* An RMW instruction writes on two adjacent cycles; MMC1 accepts only
     * the first serial-data write. */
    mapper_reset(&m);
    memset(&nes, 0, sizeof(nes));
    m.nes = &nes;
    nes.cpu.cycles = 100;
    mapper_cpu_write(&m, 0x8000, 0);
    uint8_t shift = m.mmc1_shift;
    nes.cpu.cycles = 101;
    mapper_cpu_write(&m, 0x8000, 1);
    if (m.mmc1_shift_count != 1 || m.mmc1_shift != shift)
        pass = 0;
    nes.cpu.cycles = 103;
    mapper_cpu_write(&m, 0x8000, 1);
    if (m.mmc1_shift_count != 2)
        pass = 0;

    /* SUROM: 512 KB of PRG, CHR bank bit 4 selects the 256 KB half for
     * both windows, including the fixed banks. */
    static uint8_t surom[32 * 0x4000];
    for (unsigned i = 0; i < 32; ++i) memset(surom + i * 0x4000, (int)i, 0x4000);
    mapper_init(&m, 1, surom, sizeof(surom), NULL, 0, 0);
    if (mapper_cpu_read(&m, 0x8000) != 0 || mapper_cpu_read(&m, 0xC000) != 15)
        pass = 0;
    write_register(&m, 0xE000, 3);
    write_register(&m, 0xA000, 0x10);
    if (mapper_cpu_read(&m, 0x8000) != 19 || mapper_cpu_read(&m, 0xC000) != 31)
        pass = 0;
    write_register(&m, 0x8000, 0x08);   /* fixed first bank */
    if (mapper_cpu_read(&m, 0x8000) != 16 || mapper_cpu_read(&m, 0xC000) != 19)
        pass = 0;
    write_register(&m, 0x8000, 0x00);   /* 32 KB */
    write_register(&m, 0xA000, 0x00);
    if (mapper_cpu_read(&m, 0x8000) != 2 || mapper_cpu_read(&m, 0xC000) != 3)
        pass = 0;

    printf("MMC1 tests: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
