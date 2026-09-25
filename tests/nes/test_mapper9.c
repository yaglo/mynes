#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Sixteen 8 KB PRG banks hold their index; thirty-two 4 KB CHR banks hold
 * 0xC0 + index, so a single read identifies the mapped bank. */
static uint8_t prg[16 * 0x2000];
static uint8_t chr[32 * 0x1000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void) {
    for (unsigned i = 0; i < 16; ++i) memset(prg + i * 0x2000, (int)i, 0x2000);
    for (unsigned i = 0; i < 32; ++i) memset(chr + i * 0x1000, 0xC0 + (int)i, 0x1000);

    Mapper m;
    mapper_init(&m, 9, prg, sizeof(prg), chr, sizeof(chr), 1);
    CHECK(mapper_supported(9));

    /* Power-on: bank 0 switchable, the last three banks fixed above it. */
    CHECK(mapper_cpu_read(&m, 0x8000) == 0);
    CHECK(mapper_cpu_read(&m, 0xA000) == 13);
    CHECK(mapper_cpu_read(&m, 0xC000) == 14);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 15);

    /* $A000 switches only the 8 KB window at $8000. */
    mapper_cpu_write(&m, 0xA000, 5);
    CHECK(mapper_cpu_read(&m, 0x8000) == 5);
    CHECK(mapper_cpu_read(&m, 0x9FFF) == 5);
    CHECK(mapper_cpu_read(&m, 0xA000) == 13);

    /* $FD/$FE banks for each 4 KB half; both latches start at $FE. */
    mapper_cpu_write(&m, 0xB000, 2);
    mapper_cpu_write(&m, 0xC000, 3);
    mapper_cpu_write(&m, 0xD000, 4);
    mapper_cpu_write(&m, 0xE000, 6);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC3);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC6);

    /* Latch 0 responds to the exact address $0FD8; the triggering fetch
     * still returns the old bank and the switch shows on the next one. */
    CHECK(mapper_ppu_read(&m, 0x0FD8) == 0xC3);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC2);
    /* Other rows of the $FD/$FE tiles do nothing, unlike MMC4. */
    CHECK(mapper_ppu_read(&m, 0x0FE9) == 0xC2);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC2);
    CHECK(mapper_ppu_read(&m, 0x0FE8) == 0xC2);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC3);
    CHECK(mapper_ppu_read(&m, 0x0FD9) == 0xC3);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC3);

    /* Latch 1 responds to the whole rows $1FD8-$1FDF and $1FE8-$1FEF. */
    CHECK(mapper_ppu_read(&m, 0x1FDA) == 0xC6);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC4);
    CHECK(mapper_ppu_read(&m, 0x1FEF) == 0xC4);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC6);
    CHECK(mapper_ppu_read(&m, 0x1FD0) == 0xC6);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC6);
    /* Each latch only watches its own pattern table. */
    CHECK(mapper_ppu_read(&m, 0x1FD8) == 0xC6);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC3);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC4);

    /* A debugger peek at a trigger address does not flip the latch. */
    CHECK(mapper_ppu_peek(&m, 0x0FD8) == 0xC3);
    CHECK(mapper_ppu_peek(&m, 0x1FE8) == 0xC4);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC3);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC4);

    /* A register write for the currently latched bank applies at once. */
    mapper_cpu_write(&m, 0xD000, 9);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0xC9);

    /* $F000 bit 0: 0 = vertical, 1 = horizontal. */
    mapper_cpu_write(&m, 0xF000, 1);
    CHECK(mapper_get_mirroring(&m) == 0);
    mapper_cpu_write(&m, 0xFFFF, 0);
    CHECK(mapper_get_mirroring(&m) == 1);

    /* Reset returns to bank 0 and the $FE latches. */
    mapper_reset(&m);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC0);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC0);

    printf("Mapper 9 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
