#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Sixteen 8 KB PRG banks hold their index; sixty-four 1 KB CHR banks hold
 * 0x40 + index. */
static uint8_t prg[16 * 0x2000];
static uint8_t chr[64 * 0x400];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void set_reg(Mapper *m, uint8_t reg, uint8_t val) {
    mapper_cpu_write(m, 0x8000, reg);
    mapper_cpu_write(m, 0x8001, val);
}

int main(void) {
    for (unsigned i = 0; i < 16; ++i) memset(prg + i * 0x2000, (int)i, 0x2000);
    for (unsigned i = 0; i < 64; ++i) memset(chr + i * 0x400, 0x40 + (int)i, 0x400);

    Mapper m;
    mapper_init(&m, 206, prg, sizeof(prg), chr, sizeof(chr), 1);
    CHECK(mapper_supported(206));

    /* The last two PRG banks are fixed at $C000/$E000; R6/R7 fill the rest. */
    CHECK(mapper_cpu_read(&m, 0xC000) == 14);
    CHECK(mapper_cpu_read(&m, 0xE000) == 15);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 15);
    set_reg(&m, 6, 3);
    set_reg(&m, 7, 9);
    CHECK(mapper_cpu_read(&m, 0x8000) == 3);
    CHECK(mapper_cpu_read(&m, 0x9FFF) == 3);
    CHECK(mapper_cpu_read(&m, 0xA000) == 9);
    CHECK(mapper_cpu_read(&m, 0xBFFF) == 9);

    /* R0/R1 are 2 KB banks (low bit ignored) at $0000/$0800, R2-R5 1 KB
     * banks at $1000-$1C00. */
    set_reg(&m, 0, 0x0B);
    set_reg(&m, 1, 0x02);
    set_reg(&m, 2, 0x07);
    set_reg(&m, 3, 0x11);
    set_reg(&m, 4, 0x22);
    set_reg(&m, 5, 0x3F);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0x4A);
    CHECK(mapper_ppu_read(&m, 0x0400) == 0x4B);
    CHECK(mapper_ppu_read(&m, 0x07FF) == 0x4B);
    CHECK(mapper_ppu_read(&m, 0x0800) == 0x42);
    CHECK(mapper_ppu_read(&m, 0x0FFF) == 0x43);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0x47);
    CHECK(mapper_ppu_read(&m, 0x1400) == 0x51);
    CHECK(mapper_ppu_read(&m, 0x1800) == 0x62);
    CHECK(mapper_ppu_read(&m, 0x1C00) == 0x7F);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0x7F);

    /* CHR values are six bits wide; MMC3 would treat bits 6-7 as bank bits. */
    set_reg(&m, 2, 0xC5);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0x45);

    /* MMC3's PRG/CHR mode bits in the bank-select value do nothing. */
    mapper_cpu_write(&m, 0x8000, 0xC6);
    mapper_cpu_write(&m, 0x8001, 4);
    CHECK(mapper_cpu_read(&m, 0x8000) == 4);
    CHECK(mapper_cpu_read(&m, 0xC000) == 14);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0x4A);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0x45);

    /* Only A0 is decoded, so the register pair repeats up to $FFFF and the
     * MMC3 mirroring/IRQ addresses are just more copies of it. */
    mapper_cpu_write(&m, 0xE000, 7);
    mapper_cpu_write(&m, 0xFFFF, 12);
    CHECK(mapper_cpu_read(&m, 0xA000) == 12);
    mapper_cpu_write(&m, 0xA000, 1);
    mapper_cpu_write(&m, 0xA001, 0x80);
    CHECK(mapper_get_mirroring(&m) == 1);
    CHECK(mapper_ppu_read(&m, 0x0800) == 0x40);
    mapper_cpu_write(&m, 0xC000, 1);
    mapper_cpu_write(&m, 0xC001, 1);
    mapper_cpu_write(&m, 0xE001, 1);
    for (int i = 0; i < 300; ++i) {
        mapper_ppu_address(&m, 0x0000);
        mapper_ppu_address(&m, 0x1000);
        mapper_notify_scanline(&m);
    }
    CHECK(!m.irq_pending);

    /* No PRG RAM. */
    mapper_cpu_write(&m, 0x6000, 0x55);
    CHECK(mapper_cpu_read(&m, 0x6000) == 0);

    printf("Mapper 206 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
