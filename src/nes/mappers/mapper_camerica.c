/*
 * Mapper 71: Camerica/Codemasters (BF9093, BF9097)
 *
 * UxROM-like: writes to $C000-$FFFF select the 16 KB bank at $8000 and the
 * last bank is fixed at $C000. The BF9097 board (Fire Hawk) adds a
 * single-screen mirroring latch at $8000-$9FFF. The other games never write
 * there, so it is decoded unconditionally instead of being gated on a
 * submapper the iNES 1.0 header cannot carry.
 */

#include "mapper_ops.h"

static void mapper71_init(Mapper *m) {
    m->prg_bank0 = 0;
    m->prg_bank1 = (m->prg_banks > 0) ? (m->prg_banks - 1) : 0;
}

static uint8_t mapper71_cpu_read(Mapper *m, uint16_t addr) {
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

static void mapper71_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0xC000) {
        m->prg_bank0 = val & 0x0F;
    } else if (addr >= 0x8000 && addr < 0xA000) {
        m->mirroring = (val & 0x10) ? 3 : 2;  /* 2=single-low, 3=single-high */
    } else if (addr >= 0x6000 && addr < 0x8000) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t mapper71_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        if (m->has_chr_ram) {
            return m->chr_ram[addr];
        }
        return m->chr_rom[addr % m->chr_rom_size];
    }
    return 0;
}

static void mapper71_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[addr] = val;
    }
}

const MapperOps mapper71_ops = {
    .init = mapper71_init,
    .cpu_read = mapper71_cpu_read,
    .cpu_write = mapper71_cpu_write,
    .ppu_read = mapper71_ppu_read,
    .ppu_write = mapper71_ppu_write,
};
