/*
 * Mapper 34: BNROM and NINA-001
 *
 * Two unrelated boards share the number and iNES 1.0 cannot tell them
 * apart, so the CHR type decides: BNROM only ever shipped with CHR RAM and
 * NINA-001 only with CHR ROM. BNROM latches a 32 KB PRG bank on writes to
 * $8000-$FFFF. NINA-001 keeps its registers at the top of PRG RAM ($7FFD
 * PRG, $7FFE/$7FFF the 4 KB CHR banks); those bytes stay ordinary RAM as
 * far as reads are concerned.
 */

#include "mapper_ops.h"

static void mapper34_init(Mapper *m) {
    m->prg_bank0 = 0;
    m->chr_bank0 = 0;
    m->chr_bank1 = 1;
    m->prg_ram_enabled = true;
}

static uint8_t mapper34_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t offset = (addr & 0x7FFF) + (m->prg_bank0 * 0x8000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper34_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0x8000) {
        if (m->has_chr_ram) {  /* BNROM */
            m->prg_bank0 = val;
        }
    } else if (addr >= 0x6000) {
        m->prg_ram[addr - 0x6000] = val;
        if (!m->has_chr_ram) {  /* NINA-001 */
            switch (addr) {
            case 0x7FFD: m->prg_bank0 = val & 0x01; break;
            case 0x7FFE: m->chr_bank0 = val & 0x0F; break;
            case 0x7FFF: m->chr_bank1 = val & 0x0F; break;
            default: break;
            }
        }
    }
}

static uint8_t mapper34_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        if (m->has_chr_ram) {
            return m->chr_ram[addr];
        }
        uint32_t bank = (addr < 0x1000) ? m->chr_bank0 : m->chr_bank1;
        uint32_t offset = (addr & 0x0FFF) + (bank * 0x1000);
        return m->chr_rom[offset % m->chr_rom_size];
    }
    return 0;
}

static void mapper34_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[addr] = val;
    }
}

const MapperOps mapper34_ops = {
    .init = mapper34_init,
    .cpu_read = mapper34_cpu_read,
    .cpu_write = mapper34_cpu_write,
    .ppu_read = mapper34_ppu_read,
    .ppu_write = mapper34_ppu_write,
};
