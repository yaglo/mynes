#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Two 16 KB PRG banks and four 8 KB CHR banks hold their index. */
static uint8_t prg[2 * 0x4000];
static uint8_t chr[4 * 0x2000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void) {
    for (unsigned i = 0; i < 2; ++i) memset(prg + i * 0x4000, (int)i, 0x4000);
    for (unsigned i = 0; i < 4; ++i) memset(chr + i * 0x2000, 0x10 + (int)i, 0x2000);

    Mapper m;
    mapper_init(&m, 3, prg, sizeof(prg), chr, sizeof(chr), 0);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0);
    CHECK(mapper_cpu_read(&m, 0xC000) == 1);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0x10);
    mapper_cpu_write(&m, 0x8000, 2);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0x12);
    /* CHR ROM ignores writes. */
    mapper_ppu_write(&m, 0x0000, 0xEE);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0x12);

    /* A header without CHR ROM gets 8 KB of CHR RAM instead of a read
     * modulo a zero CHR size. */
    mapper_init(&m, 3, prg, sizeof(prg), NULL, 0, 0);
    CHECK(m.has_chr_ram);
    mapper_ppu_write(&m, 0x0123, 0xA5);
    mapper_ppu_write(&m, 0x1FFF, 0x5A);
    CHECK(mapper_ppu_read(&m, 0x0123) == 0xA5);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0x5A);
    mapper_cpu_write(&m, 0x8000, 3);
    CHECK(mapper_ppu_read(&m, 0x0123) == 0xA5);

    printf("Mapper 3 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
