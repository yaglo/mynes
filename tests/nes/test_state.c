/* Save-state round trips on a real cartridge. Determinism is the contract:
 * a machine restored from a state must produce exactly the frames the
 * original went on to produce, in the same instance and in a fresh one. */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nes/rom.h"
#include "nes/nes.h"
#include "nes/state.h"
#include "nes/crc32.h"

#define ROM_PATH "tests/accuracy_coin/AccuracyCoin.nes"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "State FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* Observable machine position after a run. */
typedef struct {
    uint8_t a, x, y, sp, p;
    uint16_t pc;
    uint64_t master_tick;
    uint64_t cpu_cycles;
    uint32_t framebuffer_crc;
    uint32_t ram_crc;
    uint32_t prg_ram_crc;
} Snapshot;

static void frames(NES *nes, unsigned count) {
    while (count--) nes_run_frame(nes);
}

static Snapshot snapshot(const NES *nes) {
    Snapshot s;
    memset(&s, 0, sizeof(s)); /* padding takes part in the comparison */
    s.a = nes->cpu.A; s.x = nes->cpu.X; s.y = nes->cpu.Y;
    s.sp = nes->cpu.SP; s.p = nes->cpu.P; s.pc = nes->cpu.PC;
    s.master_tick = nes->master_tick;
    s.cpu_cycles = nes->cpu.cycles;
    s.framebuffer_crc = nes_crc32(0, nes->ppu.framebuffer, sizeof(nes->ppu.framebuffer));
    s.ram_crc = nes_crc32(0, nes->ram, sizeof(nes->ram));
    s.prg_ram_crc = nes_crc32(0, nes->mapper.prg_ram, sizeof(nes->mapper.prg_ram));
    return s;
}

static bool same(const Snapshot *a, const Snapshot *b) {
    return memcmp(a, b, sizeof(*a)) == 0;
}

static void boot(NES *nes, const ROM *rom) {
    nes_init(nes);
    nes_load_mapper(nes, rom->mapper, rom->prg_rom, rom->prg_size,
                    rom->chr_rom, rom->chr_size, rom->mirroring);
    nes_reset(nes);
}

/* A load that must be refused: the error text is set and the instance is
 * bit-for-bit what it was. */
static void expect_rejected(NES *nes, const void *state, size_t size, const char *why) {
    NES *before = malloc(sizeof(NES));
    char error[160] = "";
    memcpy(before, nes, sizeof(NES));
    bool loaded = nes_state_load(nes, state, size, error, sizeof(error));
    CHECK(!loaded);
    CHECK(error[0] != '\0');
    CHECK(memcmp(before, nes, sizeof(NES)) == 0);
    printf("  rejected %s: %s\n", why, error);
    free(before);
}

int main(void) {
    static ROM rom;
    static NES nes, other;
    if (nes_rom_load(&rom, ROM_PATH) != ROM_OK) {
        fprintf(stderr, "Cannot load %s (run from the repository root)\n", ROM_PATH);
        return 1;
    }
    boot(&nes, &rom);
    frames(&nes, 120);

    size_t size = nes_state_size(&nes);
    CHECK(size == sizeof(NESStateHeader) + sizeof(NES));
    uint8_t *state = malloc(size), *again = malloc(size);
    CHECK(nes_state_save(&nes, state, size));
    CHECK(!nes_state_save(&nes, state, size - 1));
    /* Pointers are scrubbed, so a second save of the same machine is
     * identical: nothing address-dependent leaks into the file. */
    CHECK(nes_state_save(&nes, again, size));
    CHECK(memcmp(state, again, size) == 0);

    NESStateHeader header;
    memcpy(&header, state, sizeof(header));
    CHECK(memcmp(header.magic, "MYNESST", 8) == 0);
    CHECK(header.version == NES_STATE_VERSION);
    CHECK(header.struct_size == sizeof(NES));
    CHECK(header.mapper == rom.mapper);
    CHECK(header.prg_size == rom.prg_size && header.chr_size == rom.chr_size);
    CHECK(header.rom_crc == nes_state_rom_crc(rom.prg_rom, rom.prg_size, rom.chr_rom, rom.chr_size));
    CHECK(header.image_crc == nes_crc32(0, state + sizeof(header), sizeof(NES)));

    Snapshot at_save = snapshot(&nes);
    frames(&nes, 60);
    Snapshot reference = snapshot(&nes);
    CHECK(!same(&at_save, &reference));

    /* Same instance: rewind and replay. */
    char error[160];
    CHECK(nes_state_load(&nes, state, size, error, sizeof(error)));
    Snapshot restored = snapshot(&nes);
    CHECK(same(&restored, &at_save));
    CHECK(nes.cpu.user_data == &nes && nes.ppu.user_data == &nes && nes.mapper.nes == &nes);
    CHECK(nes.cpu.mem_read == nes_cpu_read && nes.ppu.cart_read == nes_ppu_read);
    CHECK(nes.prg_rom == rom.prg_rom && nes.mapper.prg_rom == rom.prg_rom);
    frames(&nes, 60);
    Snapshot replay = snapshot(&nes);
    CHECK(same(&replay, &reference));

    /* Fresh instance with the same cartridge. */
    boot(&other, &rom);
    other.ppu.color_palette = ppu_palette_2c02; /* must survive the load */
    CHECK(nes_state_load(&other, state, size, error, sizeof(error)));
    CHECK(other.ppu.color_palette == ppu_palette_2c02);
    CHECK(other.cpu.user_data == &other && other.ppu.user_data == &other && other.mapper.nes == &other);
    CHECK(other.apu.audio_callback == NULL);
    frames(&other, 60);
    Snapshot fresh = snapshot(&other);
    CHECK(same(&fresh, &reference));

    /* The output filter and analog character are user settings: a load
     * keeps the ones in use, and the DAC tables follow them. */
    boot(&other, &rom);
    other.apu.filter_config.lp_alpha = 0.5;
    other.apu.analog.dac_nonlinearity = 1.0f;
    other.apu.analog.output_gain = 2.0f;
    apu_build_dac_tables(&other.apu);
    float curved_dac = other.apu.pulse_dac[30];
    CHECK(nes_state_load(&other, state, size, error, sizeof(error)));
    CHECK(other.apu.filter_config.lp_alpha == 0.5);
    CHECK(other.apu.analog.dac_nonlinearity == 1.0f && other.apu.analog.output_gain == 2.0f);
    CHECK(other.apu.pulse_dac[30] == curved_dac && curved_dac != nes.apu.pulse_dac[30]);

    /* Corruption and mismatches are refused without touching the machine. */
    memcpy(again, state, size);
    again[0] = 'X';
    expect_rejected(&nes, again, size, "bad magic");

    memcpy(again, state, size);
    memcpy(&header, again, sizeof(header));
    header.rom_crc ^= 0x12345678;
    memcpy(again, &header, sizeof(header));
    expect_rejected(&nes, again, size, "ROM CRC mismatch");

    memcpy(again, state, size);
    memcpy(&header, again, sizeof(header));
    header.mapper = 4;
    memcpy(again, &header, sizeof(header));
    expect_rejected(&nes, again, size, "mapper mismatch");

    memcpy(again, state, size);
    memcpy(&header, again, sizeof(header));
    header.version = NES_STATE_VERSION + 1;
    memcpy(again, &header, sizeof(header));
    expect_rejected(&nes, again, size, "unknown version");

    memcpy(again, state, size);
    memcpy(&header, again, sizeof(header));
    header.struct_size -= 8;
    memcpy(again, &header, sizeof(header));
    expect_rejected(&nes, again, size, "different build");

    memcpy(again, state, size);
    memcpy(&header, again, sizeof(header));
    header.region = PPU_REGION_PAL;
    memcpy(again, &header, sizeof(header));
    expect_rejected(&nes, again, size, "region mismatch");

    expect_rejected(&nes, state, size - 1, "truncated image");
    expect_rejected(&nes, state, 4, "truncated header");

    /* A body that changed after the header was written (bit rot, a
     * partial overwrite): every header check passes, the image CRC does not. */
    memcpy(again, state, size);
    again[sizeof(header) + offsetof(NES, ram)] ^= 0xff;
    expect_rejected(&nes, again, size, "corrupted image");

    /* A body with a matching CRC still cannot bring its own cartridge
     * geometry: the mappers index the ROM buffers with these sizes and bank
     * counts, so they stay the live instance's, like the pointers. */
    memcpy(again, state, size);
    uint32_t bogus_size = 0x7fffffff;
    uint8_t bogus_banks = 0xff;
    memcpy(again + sizeof(header) + offsetof(NES, prg_rom_size), &bogus_size, sizeof(bogus_size));
    memcpy(again + sizeof(header) + offsetof(NES, chr_rom_size), &bogus_size, sizeof(bogus_size));
    memcpy(again + sizeof(header) + offsetof(NES, mapper.prg_rom_size), &bogus_size, sizeof(bogus_size));
    memcpy(again + sizeof(header) + offsetof(NES, mapper.chr_rom_size), &bogus_size, sizeof(bogus_size));
    memcpy(again + sizeof(header) + offsetof(NES, mapper.prg_banks), &bogus_banks, sizeof(bogus_banks));
    memcpy(again + sizeof(header) + offsetof(NES, mapper.chr_banks), &bogus_banks, sizeof(bogus_banks));
    memcpy(again + sizeof(header) + offsetof(NES, mapper.number), &bogus_banks, sizeof(bogus_banks));
    memcpy(again + sizeof(header) + offsetof(NES, mapper.has_chr_ram), &bogus_banks, sizeof(bogus_banks));
    /* PRG RAM size too: battery saves copy that many bytes out of the
     * console, so 0 would stop them and a large value overrun them. */
    uint32_t live_ram_size = nes.mapper.prg_ram_size;
    memcpy(again + sizeof(header) + offsetof(NES, mapper.prg_ram_size), &bogus_size, sizeof(bogus_size));
    memcpy(&header, again, sizeof(header));
    header.image_crc = nes_crc32(0, again + sizeof(header), sizeof(NES));
    memcpy(again, &header, sizeof(header));
    CHECK(nes_state_load(&nes, again, size, error, sizeof(error)));
    CHECK(nes.prg_rom_size == rom.prg_size && nes.chr_rom_size == rom.chr_size);
    CHECK(nes.mapper.prg_rom_size == rom.prg_size && nes.mapper.chr_rom_size == rom.chr_size);
    CHECK(nes.mapper.prg_banks == rom.prg_size / 0x4000 && nes.mapper.chr_banks == rom.chr_size / 0x2000);
    CHECK(nes.mapper.number == rom.mapper && nes.mapper.has_chr_ram == (rom.chr_size == 0));
    CHECK(nes.mapper.prg_ram_size == live_ram_size);
    frames(&nes, 60);
    replay = snapshot(&nes);
    CHECK(same(&replay, &reference));

    /* Index fields the emulator uses unmasked are folded back into their
     * arrays' range, so a crafted body with a valid CRC stays in bounds. */
    memcpy(again, state, size);
    uint8_t bogus_index = 0xff;
    memcpy(again + sizeof(header) + offsetof(NES, ppu.secondary_addr), &bogus_index, 1);
    memcpy(again + sizeof(header) + offsetof(NES, ppu.sprites_on_line), &bogus_index, 1);
    memcpy(&header, again, sizeof(header));
    header.image_crc = nes_crc32(0, again + sizeof(header), sizeof(NES));
    memcpy(again, &header, sizeof(header));
    CHECK(nes_state_load(&nes, again, size, error, sizeof(error)));
    CHECK(nes.ppu.secondary_addr == 0x1f && nes.ppu.sprites_on_line == 8);

    /* The channel outputs index the mixer's DAC tables: a negative
     * envelope or an 8-bit DMC level would reach outside them. */
    memcpy(again, state, size);
    int bogus_level = -100;
    memcpy(again + sizeof(header) + offsetof(NES, apu.pulse[0].envelope_decay), &bogus_level, sizeof(int));
    memcpy(again + sizeof(header) + offsetof(NES, apu.pulse[1].envelope_decay), &bogus_level, sizeof(int));
    memcpy(again + sizeof(header) + offsetof(NES, apu.noise.envelope_decay), &bogus_level, sizeof(int));
    memcpy(again + sizeof(header) + offsetof(NES, apu.pulse[0].envelope_divider), &bogus_level, sizeof(int));
    memcpy(again + sizeof(header) + offsetof(NES, apu.dmc.output_level), &bogus_index, 1);
    memcpy(again + sizeof(header) + offsetof(NES, apu.pulse[0].sweep_shift), &bogus_index, 1);
    memcpy(&header, again, sizeof(header));
    header.image_crc = nes_crc32(0, again + sizeof(header), sizeof(NES));
    memcpy(again, &header, sizeof(header));
    CHECK(nes_state_load(&nes, again, size, error, sizeof(error)));
    CHECK(nes.apu.pulse[0].envelope_decay == (bogus_level & 15));
    CHECK(nes.apu.pulse[1].envelope_decay == (bogus_level & 15));
    CHECK(nes.apu.noise.envelope_decay == (bogus_level & 15));
    CHECK(nes.apu.pulse[0].envelope_divider == (bogus_level & 15));
    CHECK(nes.apu.dmc.output_level == 0x7f && nes.apu.pulse[0].sweep_shift == 7);
    frames(&nes, 1);

    /* A valid state still loads after the refusals. */
    CHECK(nes_state_load(&nes, state, size, error, sizeof(error)));
    restored = snapshot(&nes);
    CHECK(same(&restored, &at_save));

    /* A four-screen board's extra 2 KB is ppu.vram $2800-$2FFF and its
     * layout is mirroring mode 4, both already part of the image, so the
     * four tables round-trip in the existing format. */
    static uint8_t cart[INES_HEADER_SIZE + 2 * INES_PRG_BANK_SIZE + INES_CHR_BANK_SIZE];
    static ROM four;
    memcpy(cart, "NES\x1A\x02\x01", 6);
    cart[6] = 0x48;   /* MMC3, four-screen */
    CHECK(nes_rom_load_data(&four, cart, sizeof(cart)) == ROM_OK);
    boot(&other, &four);
    for (uint16_t t = 0; t < 4; t++)
        ppu_write(&other.ppu, 0x2000 + t * 0x400, 0xA0 + t);
    size_t four_size = nes_state_size(&other);
    uint8_t *four_state = malloc(four_size);
    CHECK(nes_state_save(&other, four_state, four_size));
    for (uint16_t t = 0; t < 4; t++)
        ppu_write(&other.ppu, 0x2000 + t * 0x400, 0);
    CHECK(nes_state_load(&other, four_state, four_size, error, sizeof(error)));
    nes_cpu_write(&other.cpu, 0xA000, 0x01);   /* MMC3 horizontal: no effect here */
    frames(&other, 1);
    CHECK(other.ppu.mirroring == 4);
    for (uint16_t t = 0; t < 4; t++)
        CHECK(ppu_read(&other.ppu, 0x2000 + t * 0x400) == 0xA0 + t);
    free(four_state);
    nes_rom_free(&four);

    free(state);
    free(again);
    nes_rom_free(&rom);
    printf("Save state regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
