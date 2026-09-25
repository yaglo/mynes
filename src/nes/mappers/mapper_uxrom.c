/*
 * Mapper 2: UxROM
 */

#include "mapper_ops.h"

static void mapper2_init(Mapper *m) {
    m->prg_bank0 = 0;
    m->prg_bank1 = (m->prg_banks > 0) ? (m->prg_banks - 1) : 0;  /* Fixed to last bank */
}

static uint8_t mapper2_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t bank = (addr < 0xC000) ? m->prg_bank0 : m->prg_bank1;
        uint32_t offset = (addr & 0x3FFF) + (bank * 0x4000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper2_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        /* A mask of prg_banks - 1 is only right for power-of-two sizes. */
        m->prg_bank0 = m->prg_banks ? val % m->prg_banks : 0;
    } else if (addr >= 0x6000) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

/* The boards carry CHR RAM, but a header that declares CHR ROM gets it. */
static uint8_t mapper2_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        if (m->has_chr_ram)
            return m->chr_ram[addr];
        return m->chr_rom[addr % m->chr_rom_size];
    }
    return 0;
}

static void mapper2_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[addr] = val;
    }
}

const MapperOps mapper2_ops = {
    .init = mapper2_init,
    .cpu_read = mapper2_cpu_read,
    .cpu_write = mapper2_cpu_write,
    .ppu_read = mapper2_ppu_read,
    .ppu_write = mapper2_ppu_write,
};
