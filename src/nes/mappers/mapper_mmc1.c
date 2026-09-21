/*
 * Mapper 1: MMC1 (SxROM)
 */

#include "mapper_ops.h"
#include "../nes.h"

static void mapper1_update_prg(Mapper *m) {
    uint8_t bank = m->mmc1_prg_bank & 0x0F;

    switch (m->prg_mode) {
    case 0: case 1:  /* 32KB mode */
        m->prg_bank0 = bank & 0x0E;
        m->prg_bank1 = m->prg_bank0 | 1;
        break;
    case 2:  /* Fix first bank at $8000 */
        m->prg_bank0 = 0;
        m->prg_bank1 = bank;
        break;
    case 3:  /* Fix last bank at $C000 */
        m->prg_bank0 = bank;
        m->prg_bank1 = (m->prg_banks > 0) ? (m->prg_banks - 1) : 0;
        break;
    }
}

static void mapper1_write_control(Mapper *m, uint8_t val) {
    m->mmc1_control = val & 0x1F;

    /* Translate mirroring bits to PPU mirroring modes */
    switch (m->mmc1_control & 0x03) {
    case 0: m->mirroring = 2; break; /* single-screen low */
    case 1: m->mirroring = 3; break; /* single-screen high */
    case 2: m->mirroring = 1; break; /* vertical */
    case 3: m->mirroring = 0; break; /* horizontal */
    }
    m->mmc1_chr_mode = (m->mmc1_control & 0x10) != 0;
    m->prg_mode = (m->mmc1_control >> 2) & 0x03;
    mapper1_update_prg(m);
}

static void mapper1_shift_write(Mapper *m, uint16_t addr, uint8_t val) {
    uint64_t cycle = m->nes ? m->nes->cpu.cycles : 0;

    /* MMC1 ignores serial-data writes on consecutive CPU cycles. Reset
     * writes (D7 set) are always honored. */
    if (!(val & 0x80) && m->nes && m->mmc1_last_write_valid &&
        cycle == m->mmc1_last_write_cycle + 1) {
        m->mmc1_last_write_cycle = cycle;
        return;
    }
    if (m->nes) {
        m->mmc1_last_write_cycle = cycle;
        m->mmc1_last_write_valid = true;
    }

    /* Reset on bit 7 */
    if (val & 0x80) {
        m->mmc1_shift = 0x10;
        m->mmc1_shift_count = 0;
        /* Hardware ORs $0C into Control. CHR mode, mirroring, and both
         * CHR bank registers must remain unchanged. */
        mapper1_write_control(m, m->mmc1_control | 0x0C);
        return;
    }

    /* Shift in bit 0 */
    m->mmc1_shift = ((m->mmc1_shift >> 1) | ((val & 1) << 4));
    m->mmc1_shift_count++;

    /* After 5 writes, register is complete */
    if (m->mmc1_shift_count == 5) {
        uint8_t reg_val = m->mmc1_shift & 0x1F;

        switch ((addr >> 13) & 0x03) {
        case 0:  /* Control ($8000-$9FFF) */
            mapper1_write_control(m, reg_val);
            break;
        case 1:  /* CHR bank 0 ($A000-$BFFF) */
            m->chr_bank0 = reg_val;
            break;
        case 2:  /* CHR bank 1 ($C000-$DFFF) */
            m->chr_bank1 = reg_val;
            break;
        case 3:  /* PRG bank ($E000-$FFFF) */
            m->prg_ram_enabled = !(reg_val & 0x10);
            m->mmc1_prg_bank = reg_val;
            mapper1_update_prg(m);
            break;
        }

        m->mmc1_shift = 0x10;
        m->mmc1_shift_count = 0;
    }
}

static void mapper1_init(Mapper *m) {
    m->mmc1_shift = 0x10;   /* SR with bit 4 set = reset state */
    m->mmc1_shift_count = 0;
    m->mmc1_control = 0x0C;
    m->mmc1_prg_bank = 0;
    m->chr_bank0 = 0;
    m->chr_bank1 = 0;
    m->prg_ram_enabled = true;
    m->mmc1_last_write_cycle = 0;
    m->mmc1_last_write_valid = false;
    mapper1_write_control(m, m->mmc1_control);
}

static uint8_t mapper1_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t bank = (addr < 0xC000) ? m->prg_bank0 : m->prg_bank1;
        uint32_t offset = (addr & 0x3FFF) + (bank * 0x4000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000 && m->prg_ram_enabled) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper1_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        mapper1_shift_write(m, addr, val);
    } else if (addr >= 0x6000 && m->prg_ram_enabled) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t mapper1_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        uint32_t chr_addr;
        if (m->mmc1_chr_mode) {  /* 4KB CHR mode */
            uint32_t bank = (addr < 0x1000) ? m->chr_bank0 : m->chr_bank1;
            chr_addr = (addr & 0x0FFF) + (bank * 0x1000);
        } else {
            uint32_t bank = m->chr_bank0 >> 1;
            chr_addr = (addr & 0x1FFF) + (bank * 0x2000);
        }
        if (m->has_chr_ram) {
            return m->chr_ram[chr_addr % sizeof(m->chr_ram)];
        }
        return m->chr_rom[chr_addr % m->chr_rom_size];
    }
    return 0;
}

static void mapper1_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        uint32_t chr_addr;
        if (m->mmc1_chr_mode) {  /* 4KB CHR mode */
            uint32_t bank = (addr < 0x1000) ? m->chr_bank0 : m->chr_bank1;
            chr_addr = (addr & 0x0FFF) + (bank * 0x1000);
        } else {
            uint32_t bank = m->chr_bank0 >> 1;
            chr_addr = (addr & 0x1FFF) + (bank * 0x2000);
        }
        m->chr_ram[chr_addr % sizeof(m->chr_ram)] = val;
    }
}

const MapperOps mapper1_ops = {
    .init = mapper1_init,
    .cpu_read = mapper1_cpu_read,
    .cpu_write = mapper1_cpu_write,
    .ppu_read = mapper1_ppu_read,
    .ppu_write = mapper1_ppu_write,
};
