/*
 * Save states (see state.h for the file layout).
 */

#include "state.h"
#include "crc32.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Every pointer or callback embedded in the NES image, and the cartridge
 * geometry the mappers index the ROM buffers with. A file cannot carry
 * addresses that mean anything in another process, and a size or bank
 * count read from a file would send a ROM read out of bounds (or divide by
 * zero) before any check could catch it, so these always come from the
 * live instance on load and are zeroed in the saved image (which also keeps
 * two saves of the same machine state byte-identical). The header already
 * ties the file to the live cartridge, so nothing is lost. The list must
 * be kept in step with nes.h, cpu_gen.h, ppu.h, apu.h and mapper.h. */
typedef struct {
    size_t offset;
    size_t size;
} StateField;

#define STATE_FIELD(member) { offsetof(NES, member), sizeof(((NES *)0)->member) }

static const StateField state_live_fields[] = {
    /* NES: legacy cartridge views of the ROM buffers */
    STATE_FIELD(prg_rom),
    STATE_FIELD(prg_rom_size),
    STATE_FIELD(chr_rom),
    STATE_FIELD(chr_rom_size),
    /* CPU: bus callbacks and their context */
    STATE_FIELD(cpu.mem_read),
    STATE_FIELD(cpu.mem_write),
    STATE_FIELD(cpu.user_data),
    /* PPU: palette table and cartridge bus callbacks */
    STATE_FIELD(ppu.color_palette),
    STATE_FIELD(ppu.cart_read),
    STATE_FIELD(ppu.cart_write),
    STATE_FIELD(ppu.cart_address),
    STATE_FIELD(ppu.cart_bus_read),
    STATE_FIELD(ppu.user_data),
    /* APU: sample sink */
    STATE_FIELD(apu.audio_callback),
    STATE_FIELD(apu.audio_user_data),
    /* Mapper: ROM buffers, their geometry and the back-pointer to its console */
    STATE_FIELD(mapper.number),
    STATE_FIELD(mapper.prg_rom),
    STATE_FIELD(mapper.prg_rom_size),
    STATE_FIELD(mapper.prg_banks),
    STATE_FIELD(mapper.chr_rom),
    STATE_FIELD(mapper.chr_rom_size),
    STATE_FIELD(mapper.chr_banks),
    STATE_FIELD(mapper.has_chr_ram),
    STATE_FIELD(mapper.nes),
};

#define STATE_LIVE_FIELD_COUNT (sizeof(state_live_fields) / sizeof(state_live_fields[0]))

/* Scratch for the live field bytes while the image is copied over the
 * instance; sized for every entry above with room to spare. */
#define STATE_LIVE_BYTES_MAX (STATE_LIVE_FIELD_COUNT * 2 * sizeof(void *))

/* The image CRC only proves the body is what some build wrote, not that
 * it came from a running machine, and the emulator indexes fixed-size
 * arrays with some fields without masking them again (the PPU's
 * secondary OAM cursor, Namco 108's register select). Fold each such field
 * back into the range its array allows, the way the hardware counter
 * would wrap, so a crafted state cannot reach outside the NES struct. */
static void state_clamp_indices(NES *nes) {
    PPU *ppu = &nes->ppu;
    ppu->secondary_addr &= 0x1F;
    ppu->oam_corruption_row &= 0x1F;
    if (ppu->sprite_count > 8) ppu->sprite_count = 8;
    if (ppu->sprites_on_line > 8) ppu->sprites_on_line = 8;

    /* The mixer indexes its DAC tables with the channel outputs, so the
     * envelope levels and the DMC counter stay within their widths too.
     * The shift keeps the sweep's target a defined shift. */
    APU *apu = &nes->apu;
    for (int i = 0; i < 2; i++) {
        apu->pulse[i].sequence_step &= 7;
        apu->pulse[i].envelope_decay &= 15;
        apu->pulse[i].envelope_divider &= 15;
        apu->pulse[i].sweep_shift &= 7;
        apu->pulse[i].sweep_period &= 7;
    }
    apu->triangle.sequence_step &= 31;
    apu->noise.envelope_decay &= 15;
    apu->noise.envelope_divider &= 15;
    apu->dmc.output_level &= 0x7F;

    Mapper *m = &nes->mapper;
    /* MMC3 keeps its mode bits beside the register number and masks on
     * use; Namco 108 stores the bare number and indexes with it. */
    if (m->number == 206)
        m->mmc3_bank_select &= 0x07;
    if (m->number == 69)
        m->ext.fme7.command &= 0x0F;
}

static void state_error(char *error, size_t error_size, const char *fmt, ...) {
    if (!error || !error_size) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(error, error_size, fmt, args);
    va_end(args);
}

uint32_t nes_state_rom_crc(const uint8_t *prg, uint32_t prg_size,
                           const uint8_t *chr, uint32_t chr_size) {
    uint32_t crc = 0;
    if (prg && prg_size) crc = nes_crc32(crc, prg, prg_size);
    if (chr && chr_size) crc = nes_crc32(crc, chr, chr_size);
    return crc;
}

size_t nes_state_size(const NES *nes) {
    (void)nes;
    return sizeof(NESStateHeader) + sizeof(NES);
}

bool nes_state_save(const NES *nes, void *buf, size_t size) {
    if (!nes || !buf || size < nes_state_size(nes)) return false;

    NESStateHeader header;
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, NES_STATE_MAGIC, sizeof(header.magic));
    header.version = NES_STATE_VERSION;
    header.struct_size = (uint32_t)sizeof(NES);
    header.prg_size = nes->prg_rom_size;
    header.chr_size = nes->chr_rom_size;
    header.mapper = nes->mapper.number;
    header.region = nes->ppu.region;
    header.rom_crc = nes_state_rom_crc(nes->prg_rom, nes->prg_rom_size,
                                       nes->chr_rom, nes->chr_rom_size);

    uint8_t *out = (uint8_t *)buf;
    uint8_t *image = out + sizeof(header);
    memcpy(image, nes, sizeof(NES));
    for (size_t i = 0; i < STATE_LIVE_FIELD_COUNT; i++)
        memset(image + state_live_fields[i].offset, 0, state_live_fields[i].size);
    header.image_crc = nes_crc32(0, image, sizeof(NES));
    memcpy(out, &header, sizeof(header));
    return true;
}

bool nes_state_load(NES *nes, const void *buf, size_t size,
                    char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!nes || !buf) {
        state_error(error, error_size, "No save state data");
        return false;
    }
    if (size < sizeof(NESStateHeader)) {
        state_error(error, error_size, "Save state is truncated");
        return false;
    }

    NESStateHeader header;
    memcpy(&header, buf, sizeof(header));
    if (memcmp(header.magic, NES_STATE_MAGIC, sizeof(header.magic)) != 0) {
        state_error(error, error_size, "Not a MyNES save state");
        return false;
    }
    if (header.version != NES_STATE_VERSION) {
        state_error(error, error_size,
                    "Save state version %u is not supported (this build reads version %u)",
                    (unsigned)header.version, (unsigned)NES_STATE_VERSION);
        return false;
    }
    if (header.struct_size != sizeof(NES)) {
        state_error(error, error_size,
                    "Save state comes from a different MyNES build (%u-byte image, expected %u)",
                    (unsigned)header.struct_size, (unsigned)sizeof(NES));
        return false;
    }
    if (size < sizeof(header) + sizeof(NES)) {
        state_error(error, error_size, "Save state is truncated (%u of %u bytes)",
                    (unsigned)size, (unsigned)(sizeof(header) + sizeof(NES)));
        return false;
    }
    if (header.mapper != nes->mapper.number ||
        header.prg_size != nes->prg_rom_size ||
        header.chr_size != nes->chr_rom_size) {
        state_error(error, error_size,
                    "Save state belongs to a different cartridge (mapper %u, %u KB PRG, %u KB CHR)",
                    (unsigned)header.mapper, (unsigned)(header.prg_size / 1024),
                    (unsigned)(header.chr_size / 1024));
        return false;
    }
    uint32_t crc = nes_state_rom_crc(nes->prg_rom, nes->prg_rom_size,
                                     nes->chr_rom, nes->chr_rom_size);
    if (header.rom_crc != crc) {
        state_error(error, error_size,
                    "Save state belongs to a different ROM (CRC %08X, loaded ROM %08X)",
                    (unsigned)header.rom_crc, (unsigned)crc);
        return false;
    }
    /* Region changes the PPU line count and APU clocks baked into the
     * image; the frontend's signal chain would disagree with it too. */
    if (header.region != nes->ppu.region) {
        state_error(error, error_size, "Save state was made in %s mode but the console runs %s",
                    header.region == PPU_REGION_PAL ? "PAL" : "NTSC",
                    nes->ppu.region == PPU_REGION_PAL ? "PAL" : "NTSC");
        return false;
    }
    /* The header only proves the cartridge; the image itself is copied
     * over the machine unparsed, so a rotted or half-written body has to be
     * caught here, before it becomes CPU state and array indices. */
    const uint8_t *image = (const uint8_t *)buf + sizeof(header);
    uint32_t image_crc = nes_crc32(0, image, sizeof(NES));
    if (header.image_crc != image_crc) {
        state_error(error, error_size, "Save state is corrupted (CRC %08X, expected %08X)",
                    (unsigned)image_crc, (unsigned)header.image_crc);
        return false;
    }

    uint8_t live[STATE_LIVE_BYTES_MAX];
    size_t used = 0;
    for (size_t i = 0; i < STATE_LIVE_FIELD_COUNT; i++) {
        if (used + state_live_fields[i].size > sizeof(live)) {
            state_error(error, error_size, "Save state pointer table overflow");
            return false;
        }
        memcpy(live + used, (const uint8_t *)nes + state_live_fields[i].offset,
               state_live_fields[i].size);
        used += state_live_fields[i].size;
    }

    memcpy(nes, image, sizeof(NES));

    used = 0;
    for (size_t i = 0; i < STATE_LIVE_FIELD_COUNT; i++) {
        memcpy((uint8_t *)nes + state_live_fields[i].offset, live + used,
               state_live_fields[i].size);
        used += state_live_fields[i].size;
    }
    state_clamp_indices(nes);
    return true;
}
