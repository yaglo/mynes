#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Six 16 KB PRG banks (not a power of two) hold their index. */
static uint8_t prg[6 * 0x4000];
static uint8_t chr[0x2000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void) {
    for (unsigned i = 0; i < 6; ++i) memset(prg + i * 0x4000, (int)i, 0x4000);
    for (unsigned i = 0; i < sizeof(chr); ++i) chr[i] = (uint8_t)(i * 7);

    Mapper m;
    mapper_init(&m, 2, prg, sizeof(prg), NULL, 0, 0);
    CHECK(mapper_cpu_read(&m, 0x8000) == 0);
    CHECK(mapper_cpu_read(&m, 0xC000) == 5);
    /* With a mask of prg_banks - 1 (5) bank 4 read as 4 but 2 read as 0. */
    for (uint8_t bank = 0; bank < 6; ++bank) {
        mapper_cpu_write(&m, 0x8000, bank);
        CHECK(mapper_cpu_read(&m, 0x8000) == bank);
        CHECK(mapper_cpu_read(&m, 0xC000) == 5);
    }
    mapper_cpu_write(&m, 0x8000, 7);
    CHECK(mapper_cpu_read(&m, 0x8000) == 1);

    /* CHR RAM without CHR ROM in the header. */
    CHECK(m.has_chr_ram);
    mapper_ppu_write(&m, 0x0456, 0x99);
    CHECK(mapper_ppu_read(&m, 0x0456) == 0x99);

    /* A header that declares CHR ROM gets it, read-only. */
    mapper_init(&m, 2, prg, sizeof(prg), chr, sizeof(chr), 0);
    CHECK(!m.has_chr_ram);
    CHECK(mapper_ppu_read(&m, 0x0456) == chr[0x456]);
    mapper_ppu_write(&m, 0x0456, (uint8_t)~chr[0x456]);
    CHECK(mapper_ppu_read(&m, 0x0456) == chr[0x456]);

    /* AxROM likewise. */
    mapper_init(&m, 7, prg, 4 * 0x4000, chr, sizeof(chr), 0);
    CHECK(mapper_ppu_read(&m, 0x1234) == chr[0x1234]);

    printf("Mapper 2 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
