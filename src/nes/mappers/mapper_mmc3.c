/*
 * Mapper 4: MMC3 (TxROM)
 */

#include "mapper_ops.h"

static void mapper4_update_banks(Mapper *m) {
    uint16_t total_prg_8k = m->prg_banks * 2;
    uint16_t total_chr_1k = m->has_chr_ram ? (sizeof(m->chr_ram) / 0x400)
                                           : (m->chr_rom_size / 0x400);
    if (total_chr_1k == 0) total_chr_1k = 1;

    /* Mask R0/R1 to even for 2KB uses */
    uint8_t r0 = m->mmc3_banks[0] & 0xFE;
    uint8_t r1 = m->mmc3_banks[1] & 0xFE;
    uint8_t r2 = m->mmc3_banks[2];
    uint8_t r3 = m->mmc3_banks[3];
    uint8_t r4 = m->mmc3_banks[4];
    uint8_t r5 = m->mmc3_banks[5];

    /* Wrap CHR indexes to available banks */
    r0 %= total_chr_1k;
    if (r0 & 1) r0--;
    r1 %= total_chr_1k;
    if (r1 & 1) r1--;
    r2 %= total_chr_1k;
    r3 %= total_chr_1k;
    r4 %= total_chr_1k;
    r5 %= total_chr_1k;

    m->mmc3_banks[0] = r0;
    m->mmc3_banks[1] = r1;
    m->mmc3_banks[2] = r2;
    m->mmc3_banks[3] = r3;
    m->mmc3_banks[4] = r4;
    m->mmc3_banks[5] = r5;

    /* PRG bank mode (bit 6 of bank select) */
    if (m->mmc3_bank_select & 0x40) {
        /* $8000: second-to-last, $C000: R6 */
        m->prg_bank0 = (total_prg_8k > 1) ? (total_prg_8k - 2) : 0;
        m->prg_bank1 = m->mmc3_banks[6] % (total_prg_8k ? total_prg_8k : 1);
    } else {
        /* $8000: R6, $C000: second-to-last */
        m->prg_bank0 = m->mmc3_banks[6] % (total_prg_8k ? total_prg_8k : 1);
        m->prg_bank1 = (total_prg_8k > 1) ? (total_prg_8k - 2) : 0;
    }
}

static void mapper4_init(Mapper *m) {
    m->mmc3_bank_select = 0;
    for (int i = 0; i < 8; i++) m->mmc3_banks[i] = 0;
    m->mmc3_irq_latch = 0;
    m->mmc3_irq_counter = 0;
    m->mmc3_irq_enabled = false;
    m->mmc3_irq_reload = false;
    m->prg_mode = 0;
    m->prg_ram_enabled = true;
    mapper4_update_banks(m);
}

static uint8_t mapper4_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t bank, offset;
        uint8_t total_prg_8k = m->prg_banks * 2;
        if (addr < 0xA000) {
            bank = m->prg_bank0;
        } else if (addr < 0xC000) {
            bank = m->mmc3_banks[7] % (total_prg_8k ? total_prg_8k : 1);
        } else if (addr < 0xE000) {
            bank = m->prg_bank1;
        } else {
            bank = (total_prg_8k > 0) ? (total_prg_8k - 1) : 0;  /* Last 8KB bank */
        }
        offset = (addr & 0x1FFF) + (bank * 0x2000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000 && m->prg_ram_enabled) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper4_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        switch (addr & 0xE001) {
        case 0x8000:  /* Bank select */
            m->mmc3_bank_select = val;
            mapper4_update_banks(m);
            break;
        case 0x8001:  /* Bank data */
            m->mmc3_banks[m->mmc3_bank_select & 0x07] = val;
            mapper4_update_banks(m);
            break;
        case 0xA000:  /* Mirroring */
            m->mirroring = (val & 0x01) ? 0 : 1;  /* 0=vertical, 1=horizontal */
            break;
        case 0xA001:  /* PRG RAM protect */
            m->prg_ram_enabled = (val & 0x80) != 0;
            m->prg_ram_write_protect = (val & 0x40) != 0;
            break;
        case 0xC000:  /* IRQ latch */
            m->mmc3_irq_latch = val;
            break;
        case 0xC001:  /* IRQ reload */
            m->mmc3_irq_counter = 0;
            m->mmc3_irq_reload = true;
            break;
        case 0xE000:  /* IRQ disable + acknowledge */
            m->mmc3_irq_enabled = false;
            m->irq_pending = false;
            break;
        case 0xE001:  /* IRQ enable */
            m->mmc3_irq_enabled = true;
            break;
        }
    } else if (addr >= 0x6000 && m->prg_ram_enabled && !m->prg_ram_write_protect) {
        m->prg_ram[addr - 0x6000] = val;
    }
}

static uint8_t mapper4_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        uint32_t bank;
        uint32_t chr_addr;
        if (m->mmc3_bank_select & 0x80) {
            /* Inverted: 1KB banks at $0000-$0FFF, 2KB at $1000-$1FFF */
            if (addr < 0x0400) bank = m->mmc3_banks[2];
            else if (addr < 0x0800) bank = m->mmc3_banks[3];
            else if (addr < 0x0C00) bank = m->mmc3_banks[4];
            else if (addr < 0x1000) bank = m->mmc3_banks[5];
            else if (addr < 0x1800) bank = m->mmc3_banks[0] & 0xFE;
            else bank = m->mmc3_banks[1] & 0xFE;

            if (addr < 0x1000) {
                chr_addr = (addr & 0x03FF) + (bank * 0x0400);
            } else {
                chr_addr = (addr & 0x07FF) + (bank * 0x0400);
            }
        } else {
            /* Normal: 2KB banks at $0000-$0FFF, 1KB at $1000-$1FFF */
            if (addr < 0x0800) bank = m->mmc3_banks[0] & 0xFE;
            else if (addr < 0x1000) bank = m->mmc3_banks[1] & 0xFE;
            else if (addr < 0x1400) bank = m->mmc3_banks[2];
            else if (addr < 0x1800) bank = m->mmc3_banks[3];
            else if (addr < 0x1C00) bank = m->mmc3_banks[4];
            else bank = m->mmc3_banks[5];

            if (addr < 0x1000) {
                chr_addr = (addr & 0x07FF) + (bank * 0x0400);
            } else {
                chr_addr = (addr & 0x03FF) + (bank * 0x0400);
            }
        }
        if (m->has_chr_ram) {
            return m->chr_ram[chr_addr % sizeof(m->chr_ram)];
        }
        return m->chr_rom[chr_addr % m->chr_rom_size];
    }
    return 0;
}

static void mapper4_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        uint32_t bank;
        uint32_t chr_addr;
        if (m->mmc3_bank_select & 0x80) {
            if (addr < 0x0400) bank = m->mmc3_banks[2];
            else if (addr < 0x0800) bank = m->mmc3_banks[3];
            else if (addr < 0x0C00) bank = m->mmc3_banks[4];
            else if (addr < 0x1000) bank = m->mmc3_banks[5];
            else if (addr < 0x1800) bank = m->mmc3_banks[0] & 0xFE;
            else bank = m->mmc3_banks[1] & 0xFE;

            if (addr < 0x1000) {
                chr_addr = (addr & 0x03FF) + (bank * 0x0400);
            } else {
                chr_addr = (addr & 0x07FF) + (bank * 0x0400);
            }
        } else {
            if (addr < 0x0800) bank = m->mmc3_banks[0] & 0xFE;
            else if (addr < 0x1000) bank = m->mmc3_banks[1] & 0xFE;
            else if (addr < 0x1400) bank = m->mmc3_banks[2];
            else if (addr < 0x1800) bank = m->mmc3_banks[3];
            else if (addr < 0x1C00) bank = m->mmc3_banks[4];
            else bank = m->mmc3_banks[5];

            if (addr < 0x1000) {
                chr_addr = (addr & 0x07FF) + (bank * 0x0400);
            } else {
                chr_addr = (addr & 0x03FF) + (bank * 0x0400);
            }
        }
        m->chr_ram[chr_addr % sizeof(m->chr_ram)] = val;
    }
}

static void mapper4_scanline(Mapper *m) {
    if (m->mmc3_irq_reload || m->mmc3_irq_counter == 0) {
        m->mmc3_irq_counter = m->mmc3_irq_latch;
        m->mmc3_irq_reload = false;
    } else {
        m->mmc3_irq_counter--;
    }

    if (m->mmc3_irq_counter == 0 && m->mmc3_irq_enabled) {
        m->irq_pending = true;
    }
}

const MapperOps mapper4_ops = {
    .init = mapper4_init,
    .cpu_read = mapper4_cpu_read,
    .cpu_write = mapper4_cpu_write,
    .ppu_read = mapper4_ppu_read,
    .ppu_write = mapper4_ppu_write,
    .scanline = mapper4_scanline,
};
