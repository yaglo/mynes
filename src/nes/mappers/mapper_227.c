/*
 * Mapper 227: address-latched multicart (including 1200-in-1)
 *
 * The value written is ignored.  PRG banking and nametable mirroring are
 * selected entirely by the address of a write in $8000-$FFFF.
 */

#include "mapper_ops.h"

static void mapper227_select(Mapper *m, uint16_t addr) {
    /* A8 supplies the high outer-bank bit; A6..A2 supply the other five. */
    uint8_t bank = (uint8_t)(((addr >> 2) & 0x1F) |
                             ((addr & 0x0100) >> 3));
    bool s = (addr & 0x0001) != 0;
    bool l = (addr & 0x0200) != 0;
    bool nrom_mode = (addr & 0x0080) != 0;

    if (nrom_mode) {
        if (s) {
            /* NROM-256: select an aligned pair of 16 KiB banks. */
            m->prg_bank0 = bank & 0xFE;
            m->prg_bank1 = m->prg_bank0 + 1;
        } else {
            /* NROM-128: mirror one 16 KiB bank in both CPU windows. */
            m->prg_bank0 = bank;
            m->prg_bank1 = bank;
        }
    } else {
        /* UNROM-like modes. S forces the switchable bank to be even. */
        m->prg_bank0 = s ? (bank & 0x3E) : bank;
        m->prg_bank1 = l ? (bank | 0x07) : (bank & 0x38);
    }

    /* A1: 0 = vertical, 1 = horizontal. */
    m->mirroring = (addr & 0x0002) ? 0 : 1;
}

static void mapper227_init(Mapper *m) {
    m->has_chr_ram = true;
    mapper227_select(m, 0);
}

static uint8_t mapper227_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000 && m->prg_rom_size != 0) {
        uint32_t bank = (addr < 0xC000) ? m->prg_bank0 : m->prg_bank1;
        uint32_t offset = bank * 0x4000u + (addr & 0x3FFF);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    return 0;
}

static void mapper227_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    (void)val;
    if (addr >= 0x8000)
        mapper227_select(m, addr);
}

static uint8_t mapper227_ppu_read(Mapper *m, uint16_t addr) {
    return (addr < 0x2000) ? m->chr_ram[addr] : 0;
}

static void mapper227_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000)
        m->chr_ram[addr] = val;
}

const MapperOps mapper227_ops = {
    .init = mapper227_init,
    .cpu_read = mapper227_cpu_read,
    .cpu_write = mapper227_cpu_write,
    .ppu_read = mapper227_ppu_read,
    .ppu_write = mapper227_ppu_write,
};
