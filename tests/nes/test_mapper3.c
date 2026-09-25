#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nes/nes.h"
#include "nes/debug.h"

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

    /* The debugger sees nametables through the PPU's mirroring: with
     * vertical mirroring $2C00 is $2400, which lives at vram $2400. */
    static NES nes;
    nes_init(&nes);
    nes_load_mapper(&nes, 3, prg, sizeof(prg), chr, sizeof(chr), 1);
    ppu_write(&nes.ppu, 0x2400, 0x77);
    ppu_write(&nes.ppu, 0x2000, 0x66);
    CHECK(debug_read_ppu_vram(&nes, 0x2C00) == 0x77);
    CHECK(debug_read_ppu_vram(&nes, 0x2800) == 0x66);
    CHECK(debug_read_ppu_vram(&nes, 0x3400) == 0x77);

    printf("Mapper 3 tests: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
