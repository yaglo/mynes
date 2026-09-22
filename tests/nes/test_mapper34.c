#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Four 32 KB PRG banks hold 0x10 + index; sixteen 4 KB CHR banks hold
 * 0xC0 + index. */
static uint8_t prg[4 * 0x8000];
static uint8_t chr[16 * 0x1000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

/* CHR RAM selects the BNROM interpretation. */
static void test_bnrom(void) {
    Mapper m;
    mapper_init(&m, 34, prg, sizeof(prg), NULL, 0, 1);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x10);

    mapper_cpu_write(&m, 0x8000, 2);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x12);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 0x12);
    mapper_cpu_write(&m, 0xFFFF, 1);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x11);

    /* The NINA-001 register addresses are plain PRG RAM on BNROM. */
    mapper_cpu_write(&m, 0x7FFD, 3);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x11);
    CHECK(mapper_cpu_read(&m, 0x7FFD) == 3);

    mapper_ppu_write(&m, 0x1234, 0xA5);
    CHECK(mapper_ppu_read(&m, 0x1234) == 0xA5);
    CHECK(mapper_get_mirroring(&m) == 1);
}

/* CHR ROM selects the NINA-001 interpretation. */
static void test_nina001(void) {
    Mapper m;
    mapper_init(&m, 34, prg, 2 * 0x8000, chr, sizeof(chr), 0);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x10);
    /* Power-on CHR is the linear first 8 KB. */
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC0);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC1);

    /* BNROM's register is not decoded. */
    mapper_cpu_write(&m, 0x8000, 1);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x10);

    /* $7FFD selects PRG with bit 0 only. */
    mapper_cpu_write(&m, 0x7FFD, 0x03);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x11);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 0x11);

    /* $7FFE/$7FFF select the two 4 KB CHR banks. */
    mapper_cpu_write(&m, 0x7FFE, 5);
    mapper_cpu_write(&m, 0x7FFF, 9);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC5);
    CHECK(mapper_ppu_read(&m, 0x0FFF) == 0xC5);
    CHECK(mapper_ppu_read(&m, 0x1000) == 0xC9);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0xC9);

    /* The registers stay readable as RAM, like the rest of $6000-$7FFF. */
    CHECK(mapper_cpu_read(&m, 0x7FFD) == 0x03);
    CHECK(mapper_cpu_read(&m, 0x7FFE) == 5);
    mapper_cpu_write(&m, 0x6000, 0x42);
    CHECK(mapper_cpu_read(&m, 0x6000) == 0x42);

    mapper_ppu_write(&m, 0x0000, 0x55);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC5);
    CHECK(mapper_get_mirroring(&m) == 0);
}

int main(void) {
    for (unsigned i = 0; i < 4; ++i) memset(prg + i * 0x8000, 0x10 + (int)i, 0x8000);
    for (unsigned i = 0; i < 16; ++i) memset(chr + i * 0x1000, 0xC0 + (int)i, 0x1000);

    CHECK(mapper_supported(34));
    test_bnrom();
    test_nina001();

    printf("Mapper 34 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
