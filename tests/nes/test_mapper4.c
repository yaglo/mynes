#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* 2 MB, the largest MMC3 image: 256 8 KB PRG banks, each filled with its
 * index. 128 16 KB banks is where a count kept in eight bits wraps to 0. */
static uint8_t prg[256 * 0x2000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void bank(Mapper *m, uint8_t select, uint8_t reg, uint8_t val) {
    mapper_cpu_write(m, 0x8000, (uint8_t)(select | reg));
    mapper_cpu_write(m, 0x8001, val);
}

int main(void) {
    for (unsigned i = 0; i < 256; ++i) memset(prg + i * 0x2000, (int)i, 0x2000);

    Mapper m;
    mapper_init(&m, 4, prg, sizeof(prg), NULL, 0, 0);
    CHECK(m.prg_banks == 128);

    /* PRG mode 0: R6 at $8000, R7 at $A000, the second-to-last bank at
     * $C000 and the last at $E000. */
    bank(&m, 0x00, 6, 200);
    bank(&m, 0x00, 7, 201);
    CHECK(mapper_cpu_read(&m, 0x8000) == 200);
    CHECK(mapper_cpu_read(&m, 0xA000) == 201);
    CHECK(mapper_cpu_read(&m, 0xC000) == 254);
    CHECK(mapper_cpu_read(&m, 0xE000) == 255);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 255);

    /* PRG mode 1 swaps $8000 and $C000. */
    bank(&m, 0x40, 6, 250);
    CHECK(mapper_cpu_read(&m, 0x8000) == 254);
    CHECK(mapper_cpu_read(&m, 0xA000) == 201);
    CHECK(mapper_cpu_read(&m, 0xC000) == 250);
    CHECK(mapper_cpu_read(&m, 0xE000) == 255);

    /* R7 reaches the top bank rather than wrapping through a short count. */
    bank(&m, 0x40, 7, 255);
    CHECK(mapper_cpu_read(&m, 0xA000) == 255);

    printf("Mapper 4 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
