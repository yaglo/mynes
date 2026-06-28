/*
 * Mapper 5: MMC5 (ExROM)
 *
 * Used by: Castlevania 3, Laser Invasion, Just Breed, etc.
 *
 * Features implemented (sufficient for Castlevania 3 US):
 *   - PRG banking modes 0-3 (8/16/16+8/32 KB granularity)
 *   - 1KB CHR banking (registers $5120-$512B)
 *   - Scanline IRQ ($5203/$5204)
 *   - Nametable mapping ($5105) translated to standard mirroring
 *   - Fill-mode tile/attribute ($5106/$5107) — stored but not rendered
 *   - PRG RAM at $6000-$7FFF
 *   - Multiplicand/multiplier hardware ($5205/$5206)
 *   - ExRAM ($5C00-$5FFF) as general-purpose RAM
 *
 * Not implemented (not needed for CV3):
 *   - ExRAM as extended nametable attributes (needs PPU changes)
 *   - Split-screen mode ($5200-$5202)
 *   - PCM audio channel
 *   - 8x16 sprite mode CHR bank switching
 */

#include "mapper_ops.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * MMC5 state — overlaid on chr_ram (unused since MMC5 uses CHR ROM)
 * -------------------------------------------------------------------------- */

typedef struct {
    /* PRG banking */
    uint8_t prg_mode;           /* $5100: 0-3 */
    uint8_t prg_regs[5];       /* $5113-$5117 */

    /* CHR banking */
    uint8_t chr_mode;           /* $5101: 0-3 */
    uint16_t chr_regs[12];     /* $5120-$512B: effective bank (upper|low) */
    uint8_t chr_upper;         /* $5130: upper 2 bits for CHR bank numbers */
    bool     chr_hi_written;   /* last CHR write was to B set ($5128-$512B) */

    /* Nametable / fill */
    uint8_t nt_mapping;        /* $5105 raw value */
    uint8_t fill_tile;         /* $5106 */
    uint8_t fill_attr;         /* $5107 (2 bits) */

    /* ExRAM */
    uint8_t exram_mode;        /* $5104: 0-3 */
    uint8_t exram[0x400];      /* 1KB */

    /* Scanline IRQ */
    uint8_t irq_target;        /* $5203 */
    bool    irq_enabled;       /* $5204 bit 7 */
    uint8_t scanline_counter;
    bool    in_frame;
    bool    irq_status;

    /* Multiplier */
    uint8_t multiplicand;      /* $5205 */
    uint8_t multiplier;        /* $5206 */
} MMC5;

_Static_assert(sizeof(MMC5) <= 0x2000, "MMC5 state must fit in chr_ram");

static MMC5 *mmc5(Mapper *m) {
    return (MMC5 *)(void *)m->chr_ram;
}

/* ========================================================================== */
/* Nametable mapping → standard mirroring                                     */
/*                                                                            */
/* $5105 has four 2-bit fields: [NT3 NT2 NT1 NT0]. Values 0/1 select CIRAM   */
/* pages; 2=ExRAM; 3=fill. We approximate by looking at which CIRAM pages     */
/* are used and mapping to the closest standard mode.                         */
/* ========================================================================== */

static void mmc5_update_mirroring(Mapper *m) {
    MMC5 *s = mmc5(m);
    uint8_t v = s->nt_mapping;
    uint8_t nt0 = (v >> 0) & 3;
    uint8_t nt1 = (v >> 2) & 3;
    uint8_t nt2 = (v >> 4) & 3;
    uint8_t nt3 = (v >> 6) & 3;

    /* Common patterns used by CV3 and other MMC5 games: */
    if (nt0 == 0 && nt1 == 0 && nt2 == 0 && nt3 == 0) {
        m->mirroring = 2; /* Single-screen A */
    } else if (nt0 == 1 && nt1 == 1 && nt2 == 1 && nt3 == 1) {
        m->mirroring = 3; /* Single-screen B */
    } else if (nt0 == 0 && nt1 == 1 && nt2 == 0 && nt3 == 1) {
        m->mirroring = 1; /* Vertical */
    } else if (nt0 == 0 && nt1 == 0 && nt2 == 1 && nt3 == 1) {
        m->mirroring = 0; /* Horizontal */
    } else {
        /* Fallback: use lower-left page for single-screen */
        m->mirroring = (nt0 == 0) ? 2 : 3;
    }
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
 * Registers $5114-$5116: bit 7 = 1 means PRG RAM, 0 means PRG ROM.
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

/*
 * MMC5 CHR banking. For 8x8 sprite mode (CV3), only the A set
 * ($5120-$5127) is used for ALL fetches. The B set is ignored.
 *   Dots 1-256, 321-336: BG tile fetches → A set
 *   Dots 257-320: sprite tile fetches → B set
 */
static uint8_t mmc5_read_chr(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);
    uint32_t total_1k = m->chr_rom_size / 0x400;
    if (total_1k == 0) return 0;

    uint32_t bank = 0;
    int slot = (addr >> 10) & 7;

    /* For 8x8 sprites (CV3), only A set is used. B set is ignored. */
    bool use_b = false;

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

static uint8_t mapper5_cpu_read(Mapper *m, uint16_t addr) {
    MMC5 *s = mmc5(m);

    if (addr >= 0x8000)
        return mmc5_read_prg(m, addr);

    if (addr >= 0x6000)
        return m->prg_ram[addr - 0x6000];

    /* Internal registers */
    switch (addr) {
    case 0x5204: {
        uint8_t val = 0;
        if (s->irq_status) val |= 0x80;
        if (s->in_frame)   val |= 0x40;
        s->irq_status = false;
        m->irq_pending = false;
        return val;
    }
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
        mmc5_update_mirroring(m);
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
    case 0x5204: s->irq_enabled = (val & 0x80) != 0; break;

    /* Multiplier */
    case 0x5205: s->multiplicand = val; break;
    case 0x5206: s->multiplier = val; break;
    }
}

/* ========================================================================== */
/* PPU read/write — CHR pattern tables only ($0000-$1FFF).                    */
/* Nametable mapping uses standard mirroring via m->mirroring.                */
/* ========================================================================== */

static uint8_t mapper5_ppu_read(Mapper *m, uint16_t addr) {
    if (addr < 0x2000)
        return mmc5_read_chr(m, addr);
    return 0;
}

static void mapper5_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    (void)m; (void)addr; (void)val; /* CHR ROM read-only */
}

/* ========================================================================== */
/* Scanline counter                                                           */
/* ========================================================================== */

/*
 * Scanline counter — matches Mesen's two-phase detection:
 *
 * Real MMC5 detects scanlines by watching for consecutive identical
 * nametable reads on the PPU address bus. We approximate using the
 * scanline callback from nes_step (fires at dot 260 of each visible
 * scanline).
 *
 * Phase 1 (first callback of frame): set in_frame, counter = 0, NO check.
 * Phase 2 (subsequent callbacks): increment counter, check against target.
 *
 * This matches Mesen's behavior where the first scanline detection
 * initializes the counter but doesn't fire an IRQ.
 */
static void mapper5_scanline(Mapper *m) {
    MMC5 *s = mmc5(m);

    if (!s->in_frame) {
        /* First scanline of frame — initialize, don't check IRQ */
        s->in_frame = true;
        s->scanline_counter = 0;
        return;
    }

    s->scanline_counter++;

    if (s->scanline_counter == s->irq_target) {
        s->irq_status = true;
        if (s->irq_enabled)
            m->irq_pending = true;
    }

    /* Reset in-frame after visible scanlines end */
    if (s->scanline_counter >= 240)
        s->in_frame = false;
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

    mmc5_update_mirroring(m);
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
    .scanline  = mapper5_scanline,
};
