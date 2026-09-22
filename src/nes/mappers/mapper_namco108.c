/*
 * Mapper 206: Namco 108 / DxROM (also Tengen MIMIC-1)
 *
 * The MMC3's ancestor: the same bank-select/bank-data register pair, but
 * with the layout hard-wired. R0/R1 are 2 KB CHR banks at $0000/$0800,
 * R2-R5 1 KB banks at $1000-$1C00, R6/R7 8 KB PRG at $8000/$A000, and the
 * last two PRG banks are fixed. There is no IRQ counter, no mirroring
 * control (the board hard-wires it) and no PRG RAM, and the mode bits of
 * the bank-select value do nothing. The chip decodes only A0 within
 * $8000-$FFFF, so the pair repeats across the whole range. The MMC3 register
 * array is reused because the registers mean the same thing here.
 */

#include "mapper_ops.h"

static void mapper206_init(Mapper *m) {
    m->mmc3_bank_select = 0;
    for (int i = 0; i < 8; i++) m->mmc3_banks[i] = 0;
}

static uint8_t mapper206_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t total_8k = m->prg_rom_size / 0x2000;
        uint32_t bank;
        if (total_8k == 0) total_8k = 1;
        if (addr < 0xA000) bank = m->mmc3_banks[6];
        else if (addr < 0xC000) bank = m->mmc3_banks[7];
        else if (addr < 0xE000) bank = (total_8k > 1) ? (total_8k - 2) : 0;
        else bank = total_8k - 1;
        uint32_t offset = (addr & 0x1FFF) + ((bank % total_8k) * 0x2000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    return 0;
}

static void mapper206_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x8000) return;
    if (addr & 1) {
        uint8_t reg = m->mmc3_bank_select;
        /* CHR registers are six bits wide (64 KB), PRG registers four (128 KB). */
        m->mmc3_banks[reg] = (reg < 6) ? (val & 0x3F) : (val & 0x0F);
    } else {
        m->mmc3_bank_select = val & 0x07;
    }
}

static uint32_t mapper206_chr_addr(const Mapper *m, uint16_t addr) {
    uint32_t bank, offset;
    if (addr < 0x1000) {
        bank = m->mmc3_banks[addr >> 11] & 0x3E;  /* 2 KB: low bit ignored */
        offset = addr & 0x07FF;
    } else {
        bank = m->mmc3_banks[2 + ((addr - 0x1000) >> 10)];
        offset = addr & 0x03FF;
    }
    return offset + (bank * 0x0400);
}

static uint8_t mapper206_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) {
        uint32_t chr_addr = mapper206_chr_addr(m, addr);
        if (m->has_chr_ram) {
            return m->chr_ram[chr_addr % sizeof(m->chr_ram)];
        }
        return m->chr_rom[chr_addr % m->chr_rom_size];
    }
    return 0;
}

static void mapper206_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[mapper206_chr_addr(m, addr) % sizeof(m->chr_ram)] = val;
    }
}

const MapperOps mapper206_ops = {
    .init = mapper206_init,
    .cpu_read = mapper206_cpu_read,
    .cpu_write = mapper206_cpu_write,
    .ppu_read = mapper206_ppu_read,
    .ppu_write = mapper206_ppu_write,
};
