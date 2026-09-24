/*
 * Mapper 9: MMC2 (PxROM), Punch-Out!!
 *
 * The CHR side is MMC4's two-latch scheme, so this reuses the mmc4_* state
 * and its 4 KB bank selection. What differs: PRG is one switchable 8 KB bank
 * at $8000-$9FFF with the last three 8 KB banks fixed above it, and latch 0
 * answers only the exact tile fetches $0FD8/$0FE8 rather than the whole
 * 8-byte tile rows that MMC4 (and latch 1 here) decode.
 */

#include "mapper_ops.h"

static void mapper9_update_chr(Mapper *m) {
    uint8_t total_4k = m->has_chr_ram ? (sizeof(m->chr_ram) / 0x1000)
                                      : (m->chr_rom_size / 0x1000);
    if (total_4k == 0) total_4k = 1;
    uint8_t bank0 = (m->mmc4_latch0 == 0) ? m->mmc4_chr_bank0_fd : m->mmc4_chr_bank0_fe;
    uint8_t bank1 = (m->mmc4_latch1 == 0) ? m->mmc4_chr_bank1_fd : m->mmc4_chr_bank1_fe;
    m->mmc4_chr0 = bank0 % total_4k;
    m->mmc4_chr1 = bank1 % total_4k;
}

/* The fetch that hits a trigger address still returns data from the bank
 * selected before it; the new bank applies from the next fetch on. Callers
 * therefore read first and call this afterwards. */
static void mapper9_check_latch(Mapper *m, uint16_t addr) {
    uint8_t latch0 = m->mmc4_latch0;
    uint8_t latch1 = m->mmc4_latch1;
    if (addr == 0x0FD8) latch0 = 0;
    else if (addr == 0x0FE8) latch0 = 1;
    else if (addr >= 0x1FD8 && addr <= 0x1FDF) latch1 = 0;
    else if (addr >= 0x1FE8 && addr <= 0x1FEF) latch1 = 1;
    if (latch0 != m->mmc4_latch0 || latch1 != m->mmc4_latch1) {
        m->mmc4_latch0 = latch0;
        m->mmc4_latch1 = latch1;
        mapper9_update_chr(m);
    }
}

static void mapper9_init(Mapper *m) {
    m->mmc4_prg_bank = 0;
    m->mmc4_chr_bank0_fd = 0;
    m->mmc4_chr_bank0_fe = 0;
    m->mmc4_chr_bank1_fd = 0;
    m->mmc4_chr_bank1_fe = 0;
    m->mmc4_latch0 = 1; /* Start latched to $FE, as MMC4 does */
    m->mmc4_latch1 = 1;
    mapper9_update_chr(m);
}

static uint32_t mapper9_prg_bank(const Mapper *m, uint16_t addr) {
    uint32_t total_8k = m->prg_rom_size / 0x2000;
    if (total_8k == 0) total_8k = 1;
    if (addr < 0xA000)
        return m->mmc4_prg_bank % total_8k;
    /* $A000, $C000 and $E000 hold the last three banks in order. */
    uint32_t slot = (addr - 0xA000) >> 13;
    return (total_8k >= 3) ? total_8k - 3 + slot : slot % total_8k;
}

static uint8_t mapper9_cpu_read(Mapper *m, uint16_t addr) {
    if (addr >= 0x8000) {
        uint32_t offset = (addr & 0x1FFF) + (mapper9_prg_bank(m, addr) * 0x2000);
        return m->prg_rom[offset % m->prg_rom_size];
    }
    if (addr >= 0x6000) {
        return m->prg_ram[addr - 0x6000];
    }
    return 0;
}

static void mapper9_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr >= 0xA000) {
        switch ((addr >> 12) & 0x07) {
        case 2:  /* $A000-$AFFF: PRG bank */
            m->mmc4_prg_bank = val & 0x0F;
            break;
        case 3:  /* $B000-$BFFF: CHR bank 0, latch $FD */
            m->mmc4_chr_bank0_fd = val & 0x1F;
            mapper9_update_chr(m);
            break;
        case 4:  /* $C000-$CFFF: CHR bank 0, latch $FE */
            m->mmc4_chr_bank0_fe = val & 0x1F;
            mapper9_update_chr(m);
            break;
        case 5:  /* $D000-$DFFF: CHR bank 1, latch $FD */
            m->mmc4_chr_bank1_fd = val & 0x1F;
            mapper9_update_chr(m);
            break;
        case 6:  /* $E000-$EFFF: CHR bank 1, latch $FE */
            m->mmc4_chr_bank1_fe = val & 0x1F;
            mapper9_update_chr(m);
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

static uint32_t mapper9_chr_addr(const Mapper *m, uint16_t addr) {
    uint32_t bank = (addr < 0x1000) ? m->mmc4_chr0 : m->mmc4_chr1;
    return (addr & 0x0FFF) + (bank * 0x1000);
}

/* The latch trigger fetches only switch banks when the PPU makes them, so
 * a debugger's peek is the lookup alone. */
static uint8_t mapper9_ppu_peek(const Mapper *m, uint16_t addr) {
    if (addr >= 0x2000) return 0;
    uint32_t chr_addr = mapper9_chr_addr(m, addr);
    return m->has_chr_ram ? m->chr_ram[chr_addr % sizeof(m->chr_ram)]
                          : m->chr_rom[chr_addr % m->chr_rom_size];
}

static uint8_t mapper9_ppu_read(Mapper *m, uint16_t addr) {
    uint8_t val = mapper9_ppu_peek(m, addr);
    if (addr < 0x2000) mapper9_check_latch(m, addr);
    return val;
}

static void mapper9_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        m->chr_ram[mapper9_chr_addr(m, addr) % sizeof(m->chr_ram)] = val;
    }
}

const MapperOps mapper9_ops = {
    .init = mapper9_init,
    .cpu_read = mapper9_cpu_read,
    .cpu_write = mapper9_cpu_write,
    .ppu_read = mapper9_ppu_read,
    .ppu_write = mapper9_ppu_write,
    .ppu_peek = mapper9_ppu_peek,
};
