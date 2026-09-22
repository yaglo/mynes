#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Four 32 KB PRG banks hold (index << 4) | 0x0F, so the ROM byte at any
 * write address passes the PRG bits and masks the CHR bits the bus conflict
 * has to clear. Sixteen 8 KB CHR banks hold 0xC0 + index. */
static uint8_t prg[4 * 0x8000];
static uint8_t chr[16 * 0x2000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void) {
    for (unsigned i = 0; i < 4; ++i) memset(prg + i * 0x8000, (int)(i << 4) | 0x0F, 0x8000);
    for (unsigned i = 0; i < 16; ++i) memset(chr + i * 0x2000, 0xC0 + (int)i, 0x2000);

    Mapper m;
    mapper_init(&m, 11, prg, sizeof(prg), chr, sizeof(chr), 0);
    CHECK(mapper_supported(11));
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x0F);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 0x0F);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC0);

    /* Bank 0's ROM byte is $0F: the PRG bits pass, the CHR bits are lost. */
    mapper_cpu_write(&m, 0x8000, 0x32);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x2F);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 0x2F);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0xC0);

    /* Bank 2's ROM byte is $2F: without the conflict this would pick CHR 3. */
    mapper_cpu_write(&m, 0xC000, 0x31);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x1F);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC2);
    CHECK(mapper_ppu_read(&m, 0x1FFF) == 0xC2);

    /* Bank 1's ROM byte is $1F: bits 2-3 of the value are not decoded. */
    mapper_cpu_write(&m, 0xFFFF, 0xFF);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0x3F);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC1);

    /* Mirroring is hard-wired and CHR ROM ignores writes. */
    CHECK(mapper_get_mirroring(&m) == 0);
    mapper_ppu_write(&m, 0x0000, 0x55);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xC1);

    printf("Mapper 11 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
