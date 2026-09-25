#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

static uint8_t prg[64 * 0x4000];

static int expect_mapping(Mapper *m, uint16_t reg, uint8_t low,
                          uint8_t high, uint8_t mirroring) {
    mapper_cpu_write(m, reg, 0xFF);
    if (mapper_cpu_read(m, 0x8000) != low ||
        mapper_cpu_read(m, 0xC000) != high ||
        mapper_get_mirroring(m) != mirroring) {
        printf("FAIL reg=%04X got banks %u/%u mirror %u, expected %u/%u/%u\n",
               reg, mapper_cpu_read(m, 0x8000), mapper_cpu_read(m, 0xC000),
               mapper_get_mirroring(m), low, high, mirroring);
        return 0;
    }
    return 1;
}

int main(void) {
    for (unsigned bank = 0; bank < 64; ++bank)
        memset(prg + bank * 0x4000, (int)bank, 0x4000);

    Mapper m;
    mapper_init(&m, 227, prg, sizeof(prg), NULL, 0, 0);

    int pass = mapper_supported(227);
    /* Reset: switchable bank 0 and fixed inner bank 0, vertical. */
    pass &= expect_mapping(&m, 0x8000, 0, 0, 1);
    /* UNROM: bank 5 in the low window, bank 7 in the high window. */
    pass &= expect_mapping(&m, 0x8214, 5, 7, 1);
    /* NROM-128: bank 37 mirrored, with horizontal mirroring. */
    pass &= expect_mapping(&m, 0x8196, 37, 37, 0);
    /* NROM-256: address-selected bank is aligned to a 32 KiB pair. */
    pass &= expect_mapping(&m, 0x8197, 36, 37, 0);
    /* S=1 UNROM mode forces an even low bank; L=0 fixes high to group base. */
    pass &= expect_mapping(&m, 0x8015, 4, 0, 1);

    mapper_ppu_write(&m, 0x1234, 0xA5);
    if (mapper_ppu_read(&m, 0x1234) != 0xA5) {
        printf("FAIL CHR RAM read/write\n");
        pass = 0;
    }

    /* A header that declares CHR ROM gets it, read-only. */
    static uint8_t chr[0x2000];
    chr[0x1234] = 0x3C;
    mapper_init(&m, 227, prg, sizeof(prg), chr, sizeof(chr), 0);
    mapper_ppu_write(&m, 0x1234, 0xA5);
    if (mapper_ppu_read(&m, 0x1234) != 0x3C) {
        printf("FAIL CHR ROM read\n");
        pass = 0;
    }

    printf("Mapper 227 tests: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
