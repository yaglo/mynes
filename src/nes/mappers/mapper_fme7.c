/*
 * Mapper 69: Sunsoft FME-7 / Sunsoft 5B
 *
 * Used by: Batman - Return of the Joker, Gimmick!, Hebereke, etc.
 *
 * Register interface:
 *   $8000: Command register (selects internal register 0-15)
 *   $A000: Parameter register (writes the selected register)
 *   $C000: Audio (Sunsoft 5B only, not implemented)
 *   $E000: Audio (Sunsoft 5B only, not implemented)
 *
 * Internal registers:
 *   0-7:  CHR bank select (1KB granularity, 8 slots)
 *   8:    PRG bank at $6000-$7FFF (bit 7: enable, bit 6: RAM/ROM)
 *   9:    PRG bank at $8000-$9FFF
 *   10:   PRG bank at $A000-$BFFF
 *   11:   PRG bank at $C000-$DFFF
 *   12:   Mirroring (bits 1:0)
 *   13:   IRQ control (bit 7: enable, bit 0: counter enable)
 *   14:   IRQ counter low byte
 *   15:   IRQ counter high byte
 *
 * $E000-$FFFF is always fixed to the last 8KB PRG bank.
 */

#include "mapper_ops.h"

typedef MapperFME7 FME7;

static FME7 *fme7(Mapper *m) {
    return &m->ext.fme7;
}

/* ========================================================================== */
/* PRG banking                                                                */
/* ========================================================================== */

static uint8_t fme7_read_prg(Mapper *m, uint16_t addr) {
    FME7 *s = fme7(m);
    uint32_t total_8k = m->prg_rom_size / 0x2000;
    if (total_8k == 0) total_8k = 1;

    uint32_t bank;

    if (addr >= 0xE000) {
        /* Fixed to last bank */
        bank = total_8k - 1;
    } else if (addr >= 0xC000) {
        bank = s->regs[11] % total_8k;
    } else if (addr >= 0xA000) {
        bank = s->regs[10] % total_8k;
    } else {
        bank = s->regs[9] % total_8k;
    }

    return m->prg_rom[(bank * 0x2000 + (addr & 0x1FFF)) % m->prg_rom_size];
}

/* ========================================================================== */
/* CHR banking (1KB granularity)                                              */
/* ========================================================================== */

static uint8_t fme7_read_chr(Mapper *m, uint16_t addr) {
    FME7 *s = fme7(m);

    if (m->has_chr_ram) {
        /* CHR RAM mode */
        int slot = (addr >> 10) & 7;
        uint32_t bank = s->regs[slot] % (sizeof(m->chr_ram) / 0x400);
        return m->chr_ram[(bank * 0x400 + (addr & 0x3FF)) % sizeof(m->chr_ram)];
    }

    uint32_t total_1k = m->chr_rom_size / 0x400;
    if (total_1k == 0) return 0;

    int slot = (addr >> 10) & 7;
    uint32_t bank = s->regs[slot] % total_1k;
    return m->chr_rom[(bank * 0x400 + (addr & 0x3FF)) % m->chr_rom_size];
}

/* ========================================================================== */
/* Mirroring                                                                  */
/* ========================================================================== */

static void fme7_update_mirroring(Mapper *m) {
    FME7 *s = fme7(m);
    switch (s->regs[12] & 3) {
    case 0: m->mirroring = 1; break; /* Vertical */
    case 1: m->mirroring = 0; break; /* Horizontal */
    case 2: m->mirroring = 2; break; /* Single-screen A */
    case 3: m->mirroring = 3; break; /* Single-screen B */
    }
}

/* ========================================================================== */
/* CPU read                                                                   */
/* ========================================================================== */

static uint8_t mapper69_cpu_read(Mapper *m, uint16_t addr) {
    FME7 *s = fme7(m);

    if (addr >= 0x8000)
        return fme7_read_prg(m, addr);

    if (addr >= 0x6000) {
        /* $6000-$7FFF: register 8 controls this window.
         * Bit 7: 0=disabled, 1=enabled
         * Bit 6: 0=PRG ROM, 1=PRG RAM
         * Bits 5:0: bank number */
        uint8_t r8 = s->regs[8];
        if (!(r8 & 0x80)) return 0; /* Disabled: open bus */
        if (r8 & 0x40) {
            /* PRG RAM */
            return m->prg_ram[addr - 0x6000];
        }
        /* PRG ROM */
        uint32_t total_8k = m->prg_rom_size / 0x2000;
        if (total_8k == 0) return 0;
        uint32_t bank = (r8 & 0x3F) % total_8k;
        return m->prg_rom[(bank * 0x2000 + (addr & 0x1FFF)) % m->prg_rom_size];
    }

    return 0;
}

/* ========================================================================== */
/* CPU write                                                                  */
/* ========================================================================== */

static void mapper69_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    FME7 *s = fme7(m);

    if (addr >= 0x6000 && addr < 0x8000) {
        /* PRG RAM write (if enabled and RAM mode) */
        uint8_t r8 = s->regs[8];
        if ((r8 & 0xC0) == 0xC0) /* enabled + RAM */
            m->prg_ram[addr - 0x6000] = val;
        return;
    }

    switch (addr & 0xE000) {
    case 0x8000: /* Command register */
        s->command = val & 0x0F;
        break;
    case 0xA000: /* Parameter register */
        s->regs[s->command] = val;
        switch (s->command) {
        case 12:
            fme7_update_mirroring(m);
            break;
        case 13:
            s->irq_enabled = (val & 0x80) != 0;
            s->irq_counting = (val & 0x01) != 0;
            m->irq_pending = false; /* Acknowledge IRQ on write */
            break;
        case 14:
            s->irq_counter = (s->irq_counter & 0xFF00) | val;
            break;
        case 15:
            s->irq_counter = (s->irq_counter & 0x00FF) | ((uint16_t)val << 8);
            break;
        }
        break;
    case 0xC000: /* Sunsoft 5B audio (not implemented) */
    case 0xE000:
        break;
    }
}

/* ========================================================================== */
/* PPU read/write                                                             */
/* ========================================================================== */

static uint8_t mapper69_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000)
        return fme7_read_chr(m, addr);
    return 0;
}

static void mapper69_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000 && m->has_chr_ram) {
        FME7 *s = fme7(m);
        int slot = (addr >> 10) & 7;
        uint32_t bank = s->regs[slot] % (sizeof(m->chr_ram) / 0x400);
        m->chr_ram[(bank * 0x400 + (addr & 0x3FF)) % sizeof(m->chr_ram)] = val;
    }
}

/* ========================================================================== */
/* IRQ counter: decremented on every CPU cycle while counting is enabled.     */
/* ========================================================================== */

static void mapper69_cpu_clock(Mapper *m) {
    FME7 *s = fme7(m);
    if (!s->irq_counting) return;
    if (s->irq_counter-- == 0 && s->irq_enabled)
        m->irq_pending = true;
}

/* ========================================================================== */
/* Init                                                                       */
/* ========================================================================== */

static void mapper69_init(Mapper *m) {
    FME7 *s = fme7(m);
    memset(s, 0, sizeof(*s));
    m->prg_ram_enabled = true;
    fme7_update_mirroring(m);
}

/* ========================================================================== */
/* Ops                                                                        */
/* ========================================================================== */

const MapperOps mapper69_ops = {
    .init      = mapper69_init,
    .cpu_read  = mapper69_cpu_read,
    .cpu_write = mapper69_cpu_write,
    .ppu_read  = mapper69_ppu_read,
    .ppu_write = mapper69_ppu_write,
    .cpu_clock = mapper69_cpu_clock,
};
