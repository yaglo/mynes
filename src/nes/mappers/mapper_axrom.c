/*
 * Mapper 7: AxROM
 */

#include "mapper_ops.h"

static void mapper7_init(Mapper *m) {
    m->prg_bank0 = 0;       /* 32KB bank number */
    m->mirroring = 2;       /* Single-screen low */
}

static uint8_t mapper7_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        /* 32KB bank switching */
        uint32_t offset = (addr & 0x7FFF) + (m->prg_bank0 * 0x8000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper7_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        /* Bits 0-2: PRG bank select */
        m->prg_bank0 = val & 0x07;
        /* Bit 4: Single-screen mirroring select (0=low, 1=high) */
        m->mirroring = (val & 0x10) ? 3 : 2;  /* 2=single-low, 3=single-high */
    } else if (addr >= 0x6000) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

/* The boards carry CHR RAM, but a header that declares CHR ROM gets it. */
static uint8_t mapper7_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        if (m->has_chr_ram)
            return m->chr_ram[addr];
        return m->chr_rom[addr % m->chr_rom_size];
    }
    return 0;
}

static void mapper7_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[addr] = val;
    }
}

const MapperOps mapper7_ops = {
    .init = mapper7_init,
    .cpu_read = mapper7_cpu_read,
    .cpu_write = mapper7_cpu_write,
    .ppu_read = mapper7_ppu_read,
    .ppu_write = mapper7_ppu_write,
};
