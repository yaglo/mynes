#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/mapper.h"

/* Sixteen 8 KB PRG banks hold their index. */
static uint8_t prg[16 * 0x2000];

static int failures;
#define CHECK(x) \
    do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void reg(Mapper *m, uint8_t r, uint8_t val) {
    mapper_cpu_write(m, 0x8000, r);
    mapper_cpu_write(m, 0xA000, val);
}

int main(void) {
    for (unsigned i = 0; i < 16; ++i) memset(prg + i * 0x2000, (int)i, 0x2000);

    Mapper m;
    mapper_init(&m, 69, prg, sizeof(prg), NULL, 0, 0);
    CHECK(m.has_chr_ram);
    reg(&m, 9, 3);
    reg(&m, 10, 4);
    reg(&m, 11, 5);
    reg(&m, 12, 1);
    reg(&m, 14, 0x34);
    reg(&m, 15, 0x12);
    CHECK(mapper_cpu_read(&m, 0x8000) == 3);
    CHECK(mapper_cpu_read(&m, 0xA000) == 4);
    CHECK(mapper_cpu_read(&m, 0xC000) == 5);
    CHECK(mapper_cpu_read(&m, 0xE000) == 15);

    /* The registers used to live in the CHR RAM array, so filling pattern
     * memory rewrote the bank, mirroring and IRQ state. */
    for (uint16_t a = 0; a < 0x2000; ++a)
        mapper_ppu_write(&m, a, 0xFF);
    CHECK(mapper_ppu_read(&m, 0x0000) == 0xFF);
    CHECK(mapper_cpu_read(&m, 0x8000) == 3);
    CHECK(mapper_cpu_read(&m, 0xA000) == 4);
    CHECK(mapper_cpu_read(&m, 0xC000) == 5);
    CHECK(mapper_get_mirroring(&m) == 0);
    CHECK(m.ext.fme7.irq_counter == 0x1234);
    CHECK(!m.ext.fme7.irq_counting && !m.irq_pending);

    /* The IRQ counter decrements once per CPU cycle and fires on the
     * wrap from $0000 to $FFFF. */
    reg(&m, 14, 2);
    reg(&m, 15, 0);
    reg(&m, 13, 0x81);
    mapper_cpu_clock(&m);
    mapper_cpu_clock(&m);
    CHECK(m.ext.fme7.irq_counter == 0 && !m.irq_pending);
    mapper_cpu_clock(&m);
    CHECK(m.ext.fme7.irq_counter == 0xFFFF && m.irq_pending);
    reg(&m, 13, 0x80);   /* acknowledge, keep counting without IRQ */
    CHECK(!m.irq_pending);
    mapper_cpu_clock(&m);
    CHECK(m.ext.fme7.irq_counter == 0xFFFE && !m.irq_pending);
    reg(&m, 13, 0x01);   /* IRQ enabled but the counter holds */
    for (int i = 0; i < 4; ++i) mapper_cpu_clock(&m);
    CHECK(m.ext.fme7.irq_counter == 0xFFFE && !m.irq_pending);
    reg(&m, 13, 0x00);   /* counting stopped */
    mapper_cpu_clock(&m);
    CHECK(m.ext.fme7.irq_counter == 0xFFFE);

    /* With the IRQ disabled the wrap raises nothing; any $D write
     * acknowledges a pending IRQ, even one that leaves both bits set. */
    reg(&m, 14, 0);
    reg(&m, 15, 0);
    reg(&m, 13, 0x80);
    mapper_cpu_clock(&m);
    CHECK(m.ext.fme7.irq_counter == 0xFFFF && !m.irq_pending);
    reg(&m, 14, 0);
    reg(&m, 15, 0);
    reg(&m, 13, 0x81);
    mapper_cpu_clock(&m);
    CHECK(m.irq_pending);
    reg(&m, 13, 0x81);
    CHECK(!m.irq_pending);

    printf("Mapper 69 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
