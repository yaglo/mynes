/*
 * Mapper 10: MMC4/FxROM
 */

#include "mapper_ops.h"

static void mapper10_update_chr(Mapper *m) {
    uint8_t total_4k = m->has_chr_ram ? (sizeof(m->chr_ram) / 0x1000)
                                      : (m->chr_rom_size / 0x1000);
    if (total_4k == 0) total_4k = 1;
    uint8_t bank0 = (m->mmc4_latch0 == 0) ? m->mmc4_chr_bank0_fd : m->mmc4_chr_bank0_fe;
    uint8_t bank1 = (m->mmc4_latch1 == 0) ? m->mmc4_chr_bank1_fd : m->mmc4_chr_bank1_fe;
    m->mmc4_chr0 = bank0 % total_4k;
    m->mmc4_chr1 = bank1 % total_4k;
}

static void mapper10_check_latch(Mapper *m, uint16_t addr) {
    if (addr >= 0x2000) return;
    uint16_t tile = addr & 0x0FF8;
    bool changed = false;
    if (addr < 0x1000) {
        if (tile == 0x0FD8 && m->mmc4_latch0 != 0) { m->mmc4_latch0 = 0; changed = true; }
        else if (tile == 0x0FE8 && m->mmc4_latch0 != 1) { m->mmc4_latch0 = 1; changed = true; }
    } else {
        if (tile == 0x0FD8 && m->mmc4_latch1 != 0) { m->mmc4_latch1 = 0; changed = true; }
        else if (tile == 0x0FE8 && m->mmc4_latch1 != 1) { m->mmc4_latch1 = 1; changed = true; }
    }
    if (changed) {
        mapper10_update_chr(m);
    }
}

static void mapper10_init(Mapper *m) {
    m->mmc4_prg_bank = 0;
    m->mmc4_chr_bank0_fd = 0;
    m->mmc4_chr_bank0_fe = 0;
    m->mmc4_chr_bank1_fd = 0;
    m->mmc4_chr_bank1_fe = 0;
    m->mmc4_latch0 = 1; /* Start latched to $FE */
    m->mmc4_latch1 = 1;
    m->prg_bank0 = 0;
    m->prg_bank1 = (m->prg_banks > 0) ? (m->prg_banks - 1) : 0;
    mapper10_update_chr(m);
}

static uint8_t mapper10_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t bank = (addr < 0xC000) ? m->mmc4_prg_bank : m->prg_bank1;
        uint32_t offset = (addr & 0x3FFF) + (bank * 0x4000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper10_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0xA000) {
        uint8_t reg = (addr >> 12) & 0x07;
        switch (reg) {
        case 2:  /* $A000-$AFFF: PRG bank */
            m->mmc4_prg_bank = val & 0x0F;
            m->mmc4_prg_bank %= (m->prg_banks ? m->prg_banks : 1);
            break;
        case 3:  /* $B000-$BFFF: CHR bank 0, latch $FD */
            m->mmc4_chr_bank0_fd = val & 0x1F;
            mapper10_update_chr(m);
            break;
        case 4:  /* $C000-$CFFF: CHR bank 0, latch $FE */
            m->mmc4_chr_bank0_fe = val & 0x1F;
            mapper10_update_chr(m);
            break;
        case 5:  /* $D000-$DFFF: CHR bank 1, latch $FD */
            m->mmc4_chr_bank1_fd = val & 0x1F;
            mapper10_update_chr(m);
            break;
        case 6:  /* $E000-$EFFF: CHR bank 1, latch $FE */
            m->mmc4_chr_bank1_fe = val & 0x1F;
            mapper10_update_chr(m);
            break;
        case 7:  /* $F000-$FFFF: Mirroring */
            m->mirroring = (val & 0x01) ? 0 : 1; /* 0=vertical, 1=horizontal */
            break;
        default:
            break;
        }
    } else if (addr >= 0x6000 && addr < 0x8000) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t mapper10_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        uint32_t chr_addr = (addr < 0x1000)
            ? (addr & 0x0FFF) + (m->mmc4_chr0 * 0x1000)
            : (addr & 0x0FFF) + (m->mmc4_chr1 * 0x1000);
        uint8_t val = m->has_chr_ram
            ? m->chr_ram[chr_addr % sizeof(m->chr_ram)]
            : m->chr_rom[chr_addr % m->chr_rom_size];
        mapper10_check_latch(m, addr);
        return val;
    }
    return 0;
}

static void mapper10_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        uint32_t chr_addr = (addr < 0x1000)
            ? (addr & 0x0FFF) + (m->mmc4_chr0 * 0x1000)
            : (addr & 0x0FFF) + (m->mmc4_chr1 * 0x1000);
        m->chr_ram[chr_addr % sizeof(m->chr_ram)] = val;
        mapper10_check_latch(m, addr);
    }
}

const MapperOps mapper10_ops = {
    .init = mapper10_init,
    .cpu_read = mapper10_cpu_read,
    .cpu_write = mapper10_cpu_write,
    .ppu_read = mapper10_ppu_read,
    .ppu_write = mapper10_ppu_write,
};
