/*
 * Mapper 5: MMC5 (ExROM)
 *
 * Used by: Castlevania 3, Laser Invasion, Just Breed, etc.
 *
 * Features implemented:
 *   - PRG banking modes 0-3 (8/16/16+8/32 KB granularity)
 *   - CHR banking modes 0-3, separate BG/sprite banks in 8x16 mode
 *   - Scanline IRQ ($5203/$5204)
 *   - Per-quadrant CIRAM/ExRAM/fill nametable mapping ($5105)
 *   - Fill-mode tile/attribute ($5106/$5107)
 *   - PRG RAM at $6000-$7FFF
 *   - Multiplicand/multiplier hardware ($5205/$5206)
 *   - ExRAM ($5C00-$5FFF) as general-purpose RAM
 *
 * Not implemented (not needed for CV3):
 *   - ExRAM as extended nametable attributes (needs PPU changes)
 *   - Split-screen mode ($5200-$5202)
 *   - PCM audio channel
 */

#include "mapper_ops.h"
#include "../nes.h"
#include <string.h>

typedef MapperMMC5 MMC5;

static MMC5 *mmc5(Mapper *m) {
    return &m->ext.mmc5;
}

/* ========================================================================== */
/* PRG banking                                                                */
/* ========================================================================== */

/*
 * Resolve which register controls a given 8KB slot in $8000-$FFFF.
 * Returns the register index (1-4) into prg_regs[].
 *
 * MMC5 PRG modes:
 *   Mode 0: One 32KB bank — $5117 controls all of $8000-$FFFF
 *   Mode 1: Two 16KB banks — $5115 controls $8000-$BFFF, $5117 controls $C000-$FFFF
 *   Mode 2: 16+8+8 — $5115 for $8000-$BFFF, $5116 for $C000-$DFFF, $5117 for $E000
 *   Mode 3: Four 8KB — $5114/$5115/$5116/$5117 for each 8KB slot
 *
 * Registers $5114-$5116: bit 7 = 1 means PRG ROM, 0 means PRG RAM.
 * Register $5117: always PRG ROM (bit 7 ignored for ROM/RAM selection).
 */
static uint8_t mmc5_get_prg_reg(MMC5 *s, uint16_t addr) {
    int slot_8k = (addr - 0x8000) >> 13; /* 0-3 */

    switch (s->prg_mode) {
    case 0: return s->prg_regs[4]; /* 32KB: $5117 for everything */
    case 1: return (addr < 0xC000) ? s->prg_regs[2] : s->prg_regs[4];
    case 2:
        if (addr < 0xC000) return s->prg_regs[2];
        if (addr < 0xE000) return s->prg_regs[3];
        return s->prg_regs[4];
    case 3: return s->prg_regs[1 + slot_8k]; /* $5114-$5117 */
    }
    return s->prg_regs[4];
}

/*
 * PRG banking — matches Mesen's approach:
 *   1. Strip bit 7 from the register (ROM/RAM flag, ignored for ROM reads)
 *   2. Apply mode-specific alignment mask
 *   3. Map as contiguous 8KB banks within the ROM
 *
 * Mode 0: $5117 & 0x7C → 32KB at $8000
 * Mode 1: $5115 & 0x7E → 16KB at $8000, $5117 & 0x7E → 16KB at $C000
 * Mode 2: $5115 & 0x7E → 16KB at $8000, $5116 & 0x7F → 8KB at $C000, $5117 & 0x7F → 8KB at $E000
 * Mode 3: $5114-$5117 & 0x7F → 8KB each
 */
static uint8_t mmc5_read_prg(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);
    uint32_t total_8k = m->prg_rom_size / 0x2000;
    if (total_8k == 0) total_8k = 1;

    uint32_t bank_8k; /* 8KB bank number */

    switch (s->prg_mode) {
    case 0: { /* 32KB */
        uint8_t base = s->prg_regs[4] & 0x7C; /* strip bit 7, align 32KB */
        bank_8k = (base + ((addr - 0x8000) >> 13)) % total_8k;
        break;
    }
    case 1: /* 16+16KB */
        if (addr < 0xC000) {
            uint8_t base = s->prg_regs[2] & 0x7E;
            bank_8k = (base + ((addr >> 13) & 1)) % total_8k;
        } else {
            uint8_t base = s->prg_regs[4] & 0x7E;
            bank_8k = (base + ((addr >> 13) & 1)) % total_8k;
        }
        break;
    case 2: /* 16+8+8KB */
        if (addr < 0xC000) {
            uint8_t base = s->prg_regs[2] & 0x7E;
            bank_8k = (base + ((addr >> 13) & 1)) % total_8k;
        } else if (addr < 0xE000) {
            bank_8k = (s->prg_regs[3] & 0x7F) % total_8k;
        } else {
            bank_8k = (s->prg_regs[4] & 0x7F) % total_8k;
        }
        break;
    default: { /* mode 3: 8+8+8+8KB */
        int slot = (addr - 0x8000) >> 13;
        bank_8k = (s->prg_regs[1 + slot] & 0x7F) % total_8k;
        break;
    }
    }

    return m->prg_rom[(bank_8k * 0x2000 + (addr & 0x1FFF)) % m->prg_rom_size];
}

/* ========================================================================== */
/* CHR banking                                                                */
/* ========================================================================== */

/* In 8x8 mode all fetches use set A. In 8x16 mode rendering uses
 * A for sprites and B for backgrounds; CPU $2007 accesses outside
 * rendering use the most recently written set. */
static uint8_t mmc5_read_chr(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);
    uint32_t total_1k = m->chr_rom_size / 0x400;
    if (total_1k == 0) return 0;

    uint32_t bank = 0;
    int slot = (addr >> 10) & 7;

    bool use_b = false;
    if (m->nes && (m->nes->ppu.ctrl & CTRL_SPRITE_SIZE)) {
        PPU *ppu = &m->nes->ppu;
        bool rendering = (ppu->mask & (MASK_BG_ENABLE | MASK_SPRITE_ENABLE)) &&
            (ppu->scanline < 240 || ppu->scanline == ppu->prerender_line);
        use_b = rendering ? !(ppu->dot >= 257 && ppu->dot <= 320)
                          : s->chr_hi_written;
    } else {
        s->chr_hi_written = false;
    }

    switch (s->chr_mode) {
    case 0: { /* 8KB */
        uint16_t r = use_b ? s->chr_regs[11] : s->chr_regs[7];
        bank = (r * 8 + slot) % total_1k;
        break;
    }
    case 1: { /* 4KB */
        uint16_t r;
        if (use_b)
            r = s->chr_regs[11];
        else
            r = (addr < 0x1000) ? s->chr_regs[3] : s->chr_regs[7];
        bank = (r * 4 + (slot & 3)) % total_1k;
        break;
    }
    case 2: { /* 2KB */
        uint16_t r;
        if (use_b)
            r = s->chr_regs[8 + ((slot >> 1) & 1) * 2 + 1];
        else {
            static const int a_regs[4] = {1, 3, 5, 7};
            r = s->chr_regs[a_regs[slot >> 1]];
        }
        bank = (r * 2 + (slot & 1)) % total_1k;
        break;
    }
    case 3: /* 1KB — CV3 uses this */
        if (use_b)
            bank = s->chr_regs[8 + (slot & 3)] % total_1k;
        else
            bank = s->chr_regs[slot] % total_1k;
        break;
    }

    return m->chr_rom[(bank * 0x400 + (addr & 0x3FF)) % m->chr_rom_size];
}

/* ========================================================================== */
/* CPU read                                                                   */
/* ========================================================================== */

static uint8_t mapper5_cpu_peek(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);

    if (addr >= 0x8000)
        return mmc5_read_prg(m, addr);

    if (addr >= 0x6000)
        return m->prg_ram[addr - 0x6000];

    /* Internal registers */
    switch (addr) {
    case 0x5204:
        return (s->irq_status ? 0x80 : 0) | (s->in_frame ? 0x40 : 0);
    case 0x5205:
        return (uint8_t)(s->multiplicand * s->multiplier);
    case 0x5206:
        return (uint8_t)((s->multiplicand * s->multiplier) >> 8);
    }

    /* ExRAM read ($5C00-$5FFF) */
    if (addr >= 0x5C00 && addr <= 0x5FFF)
        return s->exram[addr - 0x5C00];

    return 0;
}

static uint8_t mapper5_cpu_read(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);
    uint8_t val = mapper5_cpu_peek(m, addr);

    /* The NMI vector fetch ends the frame; reading $5204 acknowledges. */
    if (addr == 0xFFFA || addr == 0xFFFB) {
        s->in_frame = false;
        s->scanline_counter = 0;
        s->repeated_reads = 0;
        s->irq_status = false;
        m->irq_pending = false;
    } else if (addr == 0x5204) {
        s->irq_status = false;
        m->irq_pending = false;
    }
    return val;
}

/* ========================================================================== */
/* CPU write                                                                  */
/* ========================================================================== */

static void mapper5_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    MMC5 *s = mmc5(m);

    if (addr >= 0x8000) return; /* PRG ROM — writes ignored */

    if (addr >= 0x6000) {
        m->prg_ram[addr - 0x6000] = val;
        return;
    }

    /* ExRAM write ($5C00-$5FFF) */
    if (addr >= 0x5C00 && addr <= 0x5FFF) {
        if (s->exram_mode != 3) /* mode 3 = read-only */
            s->exram[addr - 0x5C00] = val;
        return;
    }

    if (addr >= 0x5120 && addr <= 0x512B)
        s->chr_hi_written = addr >= 0x5128;

    switch (addr) {
    /* PRG mode */
    case 0x5100: s->prg_mode = val & 3; break;

    /* CHR mode */
    case 0x5101: s->chr_mode = val & 3; break;

    /* PRG RAM protect (simplified — always allow) */
    case 0x5102: case 0x5103: break;

    /* ExRAM mode */
    case 0x5104: s->exram_mode = val & 3; break;

    /* Nametable mapping */
    case 0x5105:
        s->nt_mapping = val;
        break;

    /* Fill-mode */
    case 0x5106: s->fill_tile = val; break;
    case 0x5107: s->fill_attr = val & 3; break;

    /* PRG bank registers ($5113-$5117) */
    case 0x5113: s->prg_regs[0] = val; break;
    case 0x5114: s->prg_regs[1] = val; break;
    case 0x5115: s->prg_regs[2] = val; break;
    case 0x5116: s->prg_regs[3] = val; break;
    case 0x5117: s->prg_regs[4] = val; break;

    /* CHR bank registers ($5120-$512B)
     * Effective bank = (chr_upper << 8) | val (10-bit). */
    case 0x5120: s->chr_regs[0]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5121: s->chr_regs[1]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5122: s->chr_regs[2]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5123: s->chr_regs[3]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5124: s->chr_regs[4]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5125: s->chr_regs[5]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5126: s->chr_regs[6]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5127: s->chr_regs[7]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5128: s->chr_regs[8]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x5129: s->chr_regs[9]  = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x512A: s->chr_regs[10] = ((uint16_t)s->chr_upper << 8) | val; break;
    case 0x512B: s->chr_regs[11] = ((uint16_t)s->chr_upper << 8) | val; break;

    /* Upper CHR bank bits ($5130) */
    case 0x5130: s->chr_upper = val & 3; break;

    /* Scanline IRQ */
    case 0x5203: s->irq_target = val; break;
    case 0x5204:
        s->irq_enabled = (val & 0x80) != 0;
        m->irq_pending = s->irq_enabled && s->irq_status;
        break;

    /* Multiplier */
    case 0x5205: s->multiplicand = val; break;
    case 0x5206: s->multiplier = val; break;
    }
}

/* ========================================================================== */
/* PPU memory: MMC5 independently routes each 1KB nametable quadrant.        */
/* ========================================================================== */

static uint8_t mapper5_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000) return mmc5_read_chr(m, addr);
    MMC5 *s = mmc5(m);
    unsigned offset = addr & 0x3ff;
    unsigned source = (s->nt_mapping >> (((addr >> 10) & 3) * 2)) & 3;
    if (source < 2)
        return m->nes ? m->nes->ppu.vram[0x2000 + source * 0x400 + offset] : 0;
    if (source == 2)
        return s->exram_mode < 2 ? s->exram[offset] : 0;
    return offset < 0x3c0 ? s->fill_tile : s->fill_attr * 0x55;
}

/* Outside 8x16 mode a CHR read forgets which register set was written last. */
static uint8_t mapper5_ppu_peek(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);
    bool chr_hi_written = s->chr_hi_written;
    uint8_t val = mapper5_ppu_read(m, addr);
    s->chr_hi_written = chr_hi_written;
    return val;
}

static void mapper5_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    if (addr < 0x2000) return; /* CHR ROM */
    MMC5 *s = mmc5(m);
    unsigned offset = addr & 0x3ff;
    unsigned source = (s->nt_mapping >> (((addr >> 10) & 3) * 2)) & 3;
    if (source < 2 && m->nes)
        m->nes->ppu.vram[0x2000 + source * 0x400 + offset] = val;
    else if (source == 2 && s->exram_mode == 0)
        s->exram[offset] = val;
    /* Fill and unavailable ExRAM mappings ignore writes. */
}

/* ========================================================================== */
/* Scanline counter                                                           */
/* ========================================================================== */

/* MMC5 sees /RD strobes, not the scanline number. Three consecutive
 * identical nametable reads precede the attribute fetch that clocks it.
 * End-of-line dummy fetches provide the first two reads. */
static void mapper5_ppu_bus_read(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);
    s->ppu_read_since_clock = true;
    if (s->repeated_reads == 3) {
        s->repeated_reads = 0;
        if (!s->in_frame) {
            s->in_frame = true;
            s->scanline_counter = 0;
            s->irq_status = false;
        } else if (++s->scanline_counter == 240) {
            s->in_frame = false;
            s->scanline_counter = 0;
            s->irq_status = false;
        } else if (s->irq_target && s->scanline_counter == s->irq_target) {
            s->irq_status = true;
        }
        m->irq_pending = s->irq_enabled && s->irq_status;
    }
    if (addr >= 0x2000 && addr < 0x3000) {
        s->repeated_reads = addr == s->last_ppu_read ? s->repeated_reads + 1 : 1;
    } else {
        s->repeated_reads = 0;
    }
    s->last_ppu_read = addr;
}

static void mapper5_cpu_clock(Mapper *m) {
    MMC5 *s = mmc5(m);
    if (s->ppu_read_since_clock) {
        s->idle_cpu_cycles = 0;
        s->ppu_read_since_clock = false;
    } else if (s->idle_cpu_cycles < 3 && ++s->idle_cpu_cycles == 3) {
        s->in_frame = false;
        s->repeated_reads = 0;
    }
}

/* ========================================================================== */
/* Init                                                                       */
/* ========================================================================== */

static void mapper5_init(Mapper *m) {
    MMC5 *s = mmc5(m);
    memset(s, 0, sizeof(*s));

    s->prg_mode = 3;            /* 8KB banks — CV3 expects this */
    s->chr_mode = 3;            /* 1KB CHR banks */
    s->prg_regs[4] = 0xFF;     /* Last 8KB bank at $E000 (reset vector) */
    s->nt_mapping = 0;

    m->prg_ram_enabled = true;
    m->has_chr_ram = false;

}

/* ========================================================================== */
/* Ops                                                                        */
/* ========================================================================== */

const MapperOps mapper5_ops = {
    .init      = mapper5_init,
    .cpu_read  = mapper5_cpu_read,
    .cpu_write = mapper5_cpu_write,
    .ppu_read  = mapper5_ppu_read,
    .ppu_write = mapper5_ppu_write,
    .ppu_bus_read = mapper5_ppu_bus_read,
    .cpu_clock = mapper5_cpu_clock,
    .cpu_peek  = mapper5_cpu_peek,
    .ppu_peek  = mapper5_ppu_peek,
};
