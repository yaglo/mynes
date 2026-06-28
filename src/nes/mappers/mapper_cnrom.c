/*
 * Mapper 3: CNROM
 */

#include "mapper_ops.h"

static void mapper3_init(Mapper *m) {
    m->prg_bank0 = 0;
    m->prg_bank1 = (m->prg_banks > 1) ? 1 : 0;
    m->chr_bank0 = 0;
}

static uint8_t mapper3_cpu_read(Mapper *m, uint16_t addr) {
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

static void mapper3_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        m->chr_bank0 = val & 0x03;  /* 2-bit bank select */
    } else if (addr >= 0x6000) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t mapper3_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        uint32_t offset = addr + (m->chr_bank0 * 0x2000);
        return m->chr_rom[offset % m->chr_rom_size];
    }
    return 0;
}

static void mapper3_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    (void)m; (void)addr; (void)val; /* CHR ROM is read-only */
}

const MapperOps mapper3_ops = {
    .init = mapper3_init,
    .cpu_read = mapper3_cpu_read,
    .cpu_write = mapper3_cpu_write,
    .ppu_read = mapper3_ppu_read,
    .ppu_write = mapper3_ppu_write,
};
