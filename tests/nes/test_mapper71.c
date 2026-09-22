#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Sixteen 16 KB PRG banks hold their index. */
static uint8_t prg[16 * 0x4000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

int main(void) {
    for (unsigned i = 0; i < 16; ++i) memset(prg + i * 0x4000, (int)i, 0x4000);

    Mapper m;
    mapper_init(&m, 71, prg, sizeof(prg), NULL, 0, 0);
    CHECK(mapper_supported(71));
    CHECK(mapper_cpu_read(&m, 0x8000) == 0);
    CHECK(mapper_cpu_read(&m, 0xC000) == 15);

    /* $C000-$FFFF switches the bank at $8000; the last bank stays fixed. */
    mapper_cpu_write(&m, 0xC000, 5);
    CHECK(mapper_cpu_read(&m, 0x8000) == 5);
    CHECK(mapper_cpu_read(&m, 0xBFFF) == 5);
    CHECK(mapper_cpu_read(&m, 0xC000) == 15);
    CHECK(mapper_cpu_read(&m, 0xFFFF) == 15);
    mapper_cpu_write(&m, 0xFFFF, 0x19);
    CHECK(mapper_cpu_read(&m, 0x8000) == 9);

    /* Fire Hawk's single-screen latch at $8000-$9FFF, bit 4. */
    CHECK(mapper_get_mirroring(&m) == 0);
    mapper_cpu_write(&m, 0x8000, 0x10);
    CHECK(mapper_get_mirroring(&m) == 3);
    mapper_cpu_write(&m, 0x9FFF, 0x00);
    CHECK(mapper_get_mirroring(&m) == 2);

    /* Neither register is decoded at $A000-$BFFF. */
    mapper_cpu_write(&m, 0xA000, 0x13);
    CHECK(mapper_cpu_read(&m, 0x8000) == 9);
    CHECK(mapper_get_mirroring(&m) == 2);

    /* Each register leaves the other alone. */
    mapper_cpu_write(&m, 0x8000, 0x17);
    CHECK(mapper_cpu_read(&m, 0x8000) == 9);
    CHECK(mapper_get_mirroring(&m) == 3);
    mapper_cpu_write(&m, 0xC000, 0x02);
    CHECK(mapper_cpu_read(&m, 0x8000) == 2);
    CHECK(mapper_get_mirroring(&m) == 3);

    mapper_ppu_write(&m, 0x0123, 0xA5);
    CHECK(mapper_ppu_read(&m, 0x0123) == 0xA5);

    printf("Mapper 71 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
