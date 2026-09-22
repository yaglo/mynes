/*
 * Mapper 66: GxROM (GNROM/MHROM)
 *
 * One latch at $8000-$FFFF: bits 4-5 pick the 32 KB PRG bank, bits 0-1 the
 * 8 KB CHR bank. Mirroring is hard-wired on the board.
 */

#include "mapper_ops.h"

static void mapper66_init(Mapper *m) {
    m->prg_bank0 = 0;
    m->chr_bank0 = 0;
}

static uint8_t mapper66_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t offset = (addr & 0x7FFF) + (m->prg_bank0 * 0x8000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper66_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        m->prg_bank0 = (val >> 4) & 0x03;
        m->chr_bank0 = val & 0x03;
    } else if (addr >= 0x6000) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t mapper66_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        if (m->has_chr_ram) {
            return m->chr_ram[addr];
        }
        uint32_t offset = addr + (m->chr_bank0 * 0x2000);
        return m->chr_rom[offset % m->chr_rom_size];
    }
    return 0;
}

static void mapper66_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[addr] = val;
    }
}

const MapperOps mapper66_ops = {
    .init = mapper66_init,
    .cpu_read = mapper66_cpu_read,
    .cpu_write = mapper66_cpu_write,
    .ppu_read = mapper66_ppu_read,
    .ppu_write = mapper66_ppu_write,
};
