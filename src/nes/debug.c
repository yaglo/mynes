#include "nes/debug.h"
#include "nes/nes.h"
#include <string.h>

void debug_get_cpu_snapshot(const NES *nes, CPUSnapshot *out) {
    const CPU *cpu = &nes->cpu;
    out->PC = cpu->PC;
    out->A = cpu->A;
    out->X = cpu->X;
    out->Y = cpu->Y;
    out->SP = cpu->SP;
    out->P = cpu->P;
    out->IR = cpu->IR;
    out->uPC = cpu->uPC;
    out->cycles = cpu->cycles;
    out->irq_pending = cpu->irq_pending;
    out->nmi_pending = cpu->nmi_pending;
    out->reset_pending = cpu->reset_pending;
}

void debug_get_ppu_snapshot(const NES *nes, PPUSnapshot *out) {
    const PPU *ppu = &nes->ppu;
    out->scanline = ppu->scanline;
    out->dot = ppu->dot;
    out->v = ppu->v;
    out->t = ppu->t;
    out->fine_x = ppu->x;
    out->w = ppu->w;
    out->ctrl = ppu->ctrl;
    out->mask = ppu->mask;
    out->status = ppu->status;
    out->oam_addr = ppu->oam_addr;
    out->in_vblank = (ppu->status & STATUS_VBLANK) != 0;
    out->sprite0_hit = (ppu->status & STATUS_SPRITE_ZERO) != 0;
    out->sprite_overflow = (ppu->status & STATUS_OVERFLOW) != 0;
    out->frame_count = ppu->frame;
}

void debug_get_apu_snapshot(const NES *nes, APUSnapshot *out) {
    const APU *apu = &nes->apu;
    memset(out, 0, sizeof(APUSnapshot));

    out->frame_counter_cycle = apu->frame.cycle;
    out->frame_irq_flag = apu->frame.irq_flag;
    out->frame_irq_inhibit = apu->frame.irq_inhibit;
    out->five_step_mode = apu->frame.five_step;
    out->dmc_irq_flag = apu->dmc.irq_flag;

    /* Pulse 1 */
    out->pulse1.timer = apu->pulse[0].timer;
    out->pulse1.length_counter = apu->pulse[0].length_counter;
    out->pulse1.enabled = apu->pulse[0].enabled;
    out->pulse1.volume = (apu->pulse[0].reg[0] & 0x10)
        ? (apu->pulse[0].reg[0] & 0x0F)
        : apu->pulse[0].envelope_decay;

    /* Pulse 2 */
    out->pulse2.timer = apu->pulse[1].timer;
    out->pulse2.length_counter = apu->pulse[1].length_counter;
    out->pulse2.enabled = apu->pulse[1].enabled;
    out->pulse2.volume = (apu->pulse[1].reg[0] & 0x10)
        ? (apu->pulse[1].reg[0] & 0x0F)
        : apu->pulse[1].envelope_decay;

    /* Triangle */
    out->triangle.timer = apu->triangle.timer;
    out->triangle.length_counter = apu->triangle.length_counter;
    out->triangle.enabled = apu->triangle.enabled;
    out->triangle.volume = apu->triangle.linear_counter;

    /* Noise */
    out->noise.timer = apu->noise.timer;
    out->noise.length_counter = apu->noise.length_counter;
    out->noise.enabled = apu->noise.enabled;
    out->noise.volume = (apu->noise.reg0 & 0x10)
        ? (apu->noise.reg0 & 0x0F)
        : apu->noise.envelope_decay;

    /* DMC */
    out->dmc.enabled = apu->dmc.enabled;
    out->dmc.bytes_remaining = apu->dmc.bytes_remaining;
    out->dmc.output_level = apu->dmc.output_level;
}

uint8_t debug_read_cpu(const NES *nes, uint16_t addr) {
    if (addr < 0x2000) {
        return nes->ram[addr & 0x07FF];
    }
    else if (addr < 0x4020) {
        /* I/O registers — return 0 to avoid side effects */
        return 0;
    }
    else if (addr < 0x6000) {
        return 0; /* Expansion ROM */
    }
    else if (addr < 0x8000) {
        /* PRG RAM, which MMC5 pages through $5113 */
        if (nes->mapper_loaded)
            return mapper_cpu_peek(&nes->mapper, addr);
        return nes->prg_ram[addr & 0x1FFF];
    }
    else {
        /* PRG ROM. A plain read is not safe here: MMC5 ends its frame on
         * a read of the NMI vector. */
        if (nes->mapper_loaded)
            return mapper_cpu_peek(&nes->mapper, addr);
        if (nes->prg_rom)
            return nes->prg_rom[(addr - 0x8000) % nes->prg_rom_size];
        return 0;
    }
}

uint16_t debug_read_cpu_word(const NES *nes, uint16_t addr) {
    return debug_read_cpu(nes, addr) | ((uint16_t)debug_read_cpu(nes, addr + 1) << 8);
}

uint8_t debug_read_ppu_vram(const NES *nes, uint16_t addr) {
    const PPU *ppu = &nes->ppu;
    addr &= 0x3FFF;
    if (addr < 0x2000 || (ppu->cart_nametables && addr < 0x3F00)) {
        /* Pattern tables, and nametables the cartridge maps (MMC5):
         * peek so MMC2/MMC4 latches and MMC5 register state stay put. */
        if (nes->mapper_loaded)
            return mapper_ppu_peek(&nes->mapper, addr);
        if (addr < 0x2000)
            return (nes->chr_rom && nes->chr_rom_size > 0)
                ? nes->chr_rom[addr % nes->chr_rom_size] : ppu->vram[addr];
    }
    /* The PPU's own nametable mirroring and palette mirrors; both paths
     * of ppu_read are plain array lookups there. */
    return ppu_read((PPU *)ppu, addr);
}

uint8_t debug_read_oam(const NES *nes, uint8_t index) {
    return nes->ppu.oam[index];
}

const char *debug_format_cpu_flags(uint8_t p) {
    static char buf[9];
    buf[0] = (p & 0x80) ? 'N' : 'n';
    buf[1] = (p & 0x40) ? 'V' : 'v';
    buf[2] = '-';
    buf[3] = (p & 0x10) ? 'B' : 'b';
    buf[4] = (p & 0x08) ? 'D' : 'd';
    buf[5] = (p & 0x04) ? 'I' : 'i';
    buf[6] = (p & 0x02) ? 'Z' : 'z';
    buf[7] = (p & 0x01) ? 'C' : 'c';
    buf[8] = '\0';
    return buf;
}
