#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Four 32 KB PRG banks hold 0x10 + index; four 8 KB CHR banks hold
 * 0xC0 + index. */
static uint8_t prg[4 * 0x8000];
static uint8_t chr[4 * 0x2000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void) {
    for (unsigned i = 0; i < 4; ++i) memset(prg + i * 0x8000, 0x10 + (int)i, 0x8000);
    for (unsigned i = 0; i < 4; ++i) memset(chr + i * 0x2000, 0xC0 + (int)i, 0x2000);

    Mapper m;
    mapper_init(&m, 66, prg, sizeof(prg), chr, sizeof(chr), 0);
    CHECK(mapper_supported(66));
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x10);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC0);

    /* Bits 4-5 select PRG, bits 0-1 CHR. */
    mapper_cpu_write(&m, 0x8000, 0x23);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x12);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 0x12);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC3);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0xC3);

    /* The other bits are not decoded. */
    mapper_cpu_write(&m, 0xFFFF, 0x5D);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x11);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC1);

    /* Mirroring is hard-wired and CHR ROM ignores writes. */
    CHECK(mapper_get_mirroring(&m) == 0);
    mapper_ppu_write(&m, 0x0000, 0x55);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC1);

    printf("Mapper 66 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
