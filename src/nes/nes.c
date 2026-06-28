/*
 * NES Emulator - Public API Implementation
 *
 * Implements the public API functions declared in include/nes/nes.h
 * Provides a clean interface for frontend applications.
 */

#include "../../include/nes/nes.h"
#include "../../include/nes/types.h"
#include "rom.h"
#include "mapper.h"
#include "nes.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* Include internal headers for implementation */
#include "../internal/cpu_internal.h"
#include "../internal/ppu_internal.h"
#include "../internal/apu_internal.h"

/* ============================================================================
 * Internal NES Structure Definition
 * ============================================================================ */

struct nes {
    /* Internal NES core — embedded by value */
    NES core;

    /* Framebuffer */
    nes_rgb_t framebuffer[NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT];

    /* Timing and state */
    uint64_t cpu_cycles;
    uint64_t frame_count;
    bool frame_complete;

    /* Callbacks */
    nes_video_callback_t video_callback;
    void *video_user_data;
    nes_audio_callback_t audio_callback;
    void *audio_user_data;

    /* ROM data */
    uint8_t *prg_rom;
    uint8_t *chr_rom;
    uint32_t prg_size;
    uint32_t chr_size;
    uint8_t mapper_num;
    uint8_t mirroring;
};

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

nes_t *nes_create(void) {
    nes_t *nes = (nes_t *)calloc(1, sizeof(nes_t));
    if (!nes) {
        return NULL;
    }

    /* Initialize NES core (embedded, no separate allocation needed) */
    nes_init(&nes->core);

    return nes;
}

void nes_destroy(nes_t *nes) {
    if (!nes) {
        return;
    }

    if (nes->prg_rom) {
        free(nes->prg_rom);
    }
    if (nes->chr_rom) {
        free(nes->chr_rom);
    }

    free(nes);
}

void nes_reset(nes_t *nes) {
    if (!nes) {
        return;
    }

    /* Reset system RAM */
    memset(nes->core.ram, 0, sizeof(nes->core.ram));

    /* Reset controller state */
    memset(nes->core.controller, 0, sizeof(nes->core.controller));
    memset(nes->core.controller_shift, 0, sizeof(nes->core.controller_shift));
    nes->core.controller_strobe = 0;

    /* Reset timing */
    nes->cpu_cycles = 0;
    nes->frame_count = 0;
    nes->frame_complete = false;

    /* Reset components */
    cpu_reset(&nes->core.cpu);
    ppu_reset(&nes->core.ppu);
    apu_reset(&nes->core.apu);

    /* Reset mapper */
    if (nes->core.mapper_loaded) {
        mapper_reset(&nes->core.mapper);
    }
}

/* ============================================================================
 * ROM Loading
 * ============================================================================ */

nes_error_t nes_load_rom_file(nes_t *nes, const char *path) {
    if (!nes || !path) {
        return NES_ERROR_INVALID_ARGUMENT;
    }

    /* Unload any existing ROM */
    nes_unload_rom(nes);

    /* Load ROM using internal loader */
    ROM rom;
    int err = nes_rom_load(&rom, path);
    if (err != ROM_OK) {
        switch (err) {
            case ROM_ERR_FILE:   return NES_ERROR_FILE_NOT_FOUND;
            case ROM_ERR_HEADER: return NES_ERROR_INVALID_ROM;
            case ROM_ERR_MAPPER: return NES_ERROR_UNSUPPORTED_MAPPER;
            case ROM_ERR_ALLOC:  return NES_ERROR_OUT_OF_MEMORY;
            default:             return NES_ERROR_FILE_READ;
        }
    }

    /* Copy ROM data */
    nes->prg_size = rom.prg_size;
    nes->chr_size = rom.chr_size;
    nes->mapper_num = rom.mapper;
    nes->mirroring = rom.mirroring;

    nes->prg_rom = (uint8_t *)malloc(nes->prg_size);
    nes->chr_rom = (nes->chr_size > 0) ? (uint8_t *)malloc(nes->chr_size) : NULL;

    if (!nes->prg_rom || (nes->chr_size > 0 && !nes->chr_rom)) {
        nes_rom_free(&rom);
        nes_unload_rom(nes);
        return NES_ERROR_OUT_OF_MEMORY;
    }

    memcpy(nes->prg_rom, rom.prg_rom, nes->prg_size);
    if (nes->chr_rom && rom.chr_rom) {
        memcpy(nes->chr_rom, rom.chr_rom, nes->chr_size);
    }

    /* Initialize mapper */
    nes_load_mapper(&nes->core, nes->mapper_num,
                    nes->prg_rom, nes->prg_size,
                    nes->chr_rom, nes->chr_size,
                    nes->mirroring);

    nes_rom_free(&rom);
    return NES_OK;
}

nes_error_t nes_load_rom_data(nes_t *nes, const uint8_t *data, size_t size) {
    if (!nes || !data || size < INES_HEADER_SIZE) {
        return NES_ERROR_INVALID_ARGUMENT;
    }

    /* Unload any existing ROM */
    nes_unload_rom(nes);

    /* Parse header */
    if (data[0] != 'N' || data[1] != 'E' || data[2] != 'S' || data[3] != 0x1A) {
        return NES_ERROR_INVALID_ROM;
    }

    uint8_t prg_banks = data[4];
    uint8_t chr_banks = data[5];
    uint8_t flags6 = data[6];
    uint8_t flags7 = data[7];

    /* Archaic iNES / "DiskDude!" heuristic — see rom.h for rationale.
     * NES 2.0 (flags7 bits 2-3 == 0b10) is detected from the RAW flags7
     * first, and only ROMs that aren't NES 2.0 fall through to the
     * "bytes 12-15 non-zero → treat flags7 as junk" fallback. Without
     * this ordering, NES 2.0 ROMs that have legit data in bytes 12-15
     * (the entire point of NES 2.0 is to use those bytes) get their
     * mapper upper-nibble wiped. */
    bool is_nes2 = ((flags7 & 0x0C) == 0x08);
    if (!is_nes2 && (data[12] | data[13] | data[14] | data[15]) != 0) {
        flags7 = 0;
    }

    nes->prg_size = prg_banks * INES_PRG_BANK_SIZE;
    nes->chr_size = chr_banks * INES_CHR_BANK_SIZE;
    nes->mapper_num = ((flags6 >> 4) & 0x0F) | (flags7 & 0xF0);
    nes->mirroring = flags6 & 0x01;

    /* Check for supported mappers */
    if (!mapper_supported(nes->mapper_num)) {
        return NES_ERROR_UNSUPPORTED_MAPPER;
    }

    /* Calculate data offset */
    size_t offset = INES_HEADER_SIZE;
    if (flags6 & 0x04) {  /* Trainer */
        offset += INES_TRAINER_SIZE;
    }

    /* Check if we have enough data */
    if (size < offset + nes->prg_size + nes->chr_size) {
        return NES_ERROR_INVALID_ROM;
    }

    /* Copy ROM data */
    nes->prg_rom = (uint8_t *)malloc(nes->prg_size);
    nes->chr_rom = (nes->chr_size > 0) ? (uint8_t *)malloc(nes->chr_size) : NULL;

    if (!nes->prg_rom || (nes->chr_size > 0 && !nes->chr_rom)) {
        nes_unload_rom(nes);
        return NES_ERROR_OUT_OF_MEMORY;
    }

    memcpy(nes->prg_rom, data + offset, nes->prg_size);
    if (nes->chr_rom) {
        memcpy(nes->chr_rom, data + offset + nes->prg_size, nes->chr_size);
    }

    /* Initialize mapper */
    nes_load_mapper(&nes->core, nes->mapper_num,
                    nes->prg_rom, nes->prg_size,
                    nes->chr_rom, nes->chr_size,
                    nes->mirroring);

    return NES_OK;
}

void nes_unload_rom(nes_t *nes) {
    if (!nes) {
        return;
    }

    if (nes->prg_rom) {
        free(nes->prg_rom);
        nes->prg_rom = NULL;
    }
    if (nes->chr_rom) {
        free(nes->chr_rom);
        nes->chr_rom = NULL;
    }

    nes->prg_size = 0;
    nes->chr_size = 0;
    nes->mapper_num = 0;
    nes->mirroring = 0;
}

nes_error_t nes_get_rom_info(const nes_t *nes, nes_rom_info_t *info) {
    if (!nes || !info) {
        return NES_ERROR_INVALID_ARGUMENT;
    }

    if (!nes->prg_rom) {
        return NES_ERROR_INVALID_STATE;
    }

    info->mapper = nes->mapper_num;
    info->prg_rom_banks = nes->prg_size / INES_PRG_BANK_SIZE;
    info->chr_rom_banks = nes->chr_size / INES_CHR_BANK_SIZE;
    info->prg_ram_banks = 1;  /* TODO: Get from mapper */
    info->mirroring = (nes_mirroring_t)nes->mirroring;
    info->has_battery = false;  /* TODO: Get from ROM header */
    info->has_trainer = false;  /* TODO: Get from ROM header */

    return NES_OK;
}

/* ============================================================================
 * Emulation
 * ============================================================================ */

void nes_run_frame(nes_t *nes) {
    if (!nes) {
        return;
    }

    nes->frame_complete = false;

    /* Run 29780 CPU cycles (one frame) */
    nes_run_cycles(nes, 29780);

    nes->frame_count++;
    nes->frame_complete = true;

    /* Call video callback if set */
    if (nes->video_callback) {
        nes->video_callback(nes->video_user_data, nes->framebuffer);
    }
}

uint32_t nes_run_cycles(nes_t *nes, uint32_t cycles) {
    if (!nes) {
        return 0;
    }

    uint32_t cycles_executed = 0;

    while (cycles_executed < cycles) {
        nes_step(&nes->core);
        cycles_executed++;
    }

    return cycles_executed;
}

void nes_step(nes_t *nes) {
    if (!nes) {
        return;
    }

    /* Execute one CPU cycle */
    nes_step(&nes->core);
    nes->cpu_cycles++;
}

bool nes_frame_complete(const nes_t *nes) {
    return nes ? nes->frame_complete : false;
}

/* ============================================================================
 * Video Output
 * ============================================================================ */

const nes_rgb_t *nes_get_framebuffer(const nes_t *nes) {
    if (!nes) {
        return NULL;
    }

    /* Copy from PPU framebuffer to public framebuffer */
    memcpy((void *)nes->framebuffer, nes->core.ppu.framebuffer,
           sizeof(nes->framebuffer));

    return nes->framebuffer;
}

void nes_set_video_callback(nes_t *nes, nes_video_callback_t callback, void *user_data) {
    if (!nes) {
        return;
    }

    nes->video_callback = callback;
    nes->video_user_data = user_data;
}

void nes_get_framebuffer_rgba(const nes_t *nes, uint8_t *dest, uint8_t alpha) {
    if (!nes || !dest) {
        return;
    }

    const nes_rgb_t *src = nes_get_framebuffer(nes);
    if (!src) {
        return;
    }

    for (int i = 0; i < NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT; i++) {
        dest[i * 4 + 0] = src[i].r;
        dest[i * 4 + 1] = src[i].g;
        dest[i * 4 + 2] = src[i].b;
        dest[i * 4 + 3] = alpha;
    }
}

void nes_get_framebuffer_bgra(const nes_t *nes, uint8_t *dest, uint8_t alpha) {
    if (!nes || !dest) {
        return;
    }

    const nes_rgb_t *src = nes_get_framebuffer(nes);
    if (!src) {
        return;
    }

    for (int i = 0; i < NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT; i++) {
        dest[i * 4 + 0] = src[i].b;
        dest[i * 4 + 1] = src[i].g;
        dest[i * 4 + 2] = src[i].r;
        dest[i * 4 + 3] = alpha;
    }
}

void nes_get_framebuffer_rgb565(const nes_t *nes, uint16_t *dest) {
    if (!nes || !dest) {
        return;
    }

    const nes_rgb_t *src = nes_get_framebuffer(nes);
    if (!src) {
        return;
    }

    for (int i = 0; i < NES_SCREEN_WIDTH * NES_SCREEN_HEIGHT; i++) {
        uint8_t r = src[i].r >> 3;
        uint8_t g = src[i].g >> 2;
        uint8_t b = src[i].b >> 3;
        dest[i] = (r << 11) | (g << 5) | b;
    }
}

/* ============================================================================
 * Audio Output
 * ============================================================================ */

void nes_set_audio_sample_rate(nes_t *nes, uint32_t sample_rate) {
    if (!nes) {
        return;
    }

    /* TODO: Implement APU sample rate setting */
    (void)sample_rate;
}

void nes_set_audio_callback(nes_t *nes, nes_audio_callback_t callback, void *user_data) {
    if (!nes) {
        return;
    }

    nes->audio_callback = callback;
    nes->audio_user_data = user_data;
}

/* ============================================================================
 * Input
 * ============================================================================ */

void nes_set_controller(nes_t *nes, int controller, uint8_t buttons) {
    if (!nes || controller < 0 || controller > 1) {
        return;
    }

    nes->core.controller[controller] = buttons;
    nes_set_controller(&nes->core, controller, buttons);
}

uint8_t nes_get_controller(const nes_t *nes, int controller) {
    if (!nes || controller < 0 || controller > 1) {
        return 0;
    }

    return nes->core.controller[controller];
}

/* ============================================================================
 * Save States
 * ============================================================================ */

size_t nes_save_state_size(const nes_t *nes) {
    if (!nes) {
        return 0;
    }

    /* TODO: Calculate actual save state size */
    return sizeof(nes_t) + 4096;  /* Rough estimate */
}

nes_error_t nes_save_state(const nes_t *nes, void *buffer, size_t size) {
    if (!nes || !buffer) {
        return NES_ERROR_INVALID_ARGUMENT;
    }

    if (size < nes_save_state_size(nes)) {
        return NES_ERROR_INVALID_ARGUMENT;
    }

    /* TODO: Implement actual save state */
    memset(buffer, 0, size);
    return NES_OK;
}

nes_error_t nes_load_state(nes_t *nes, const void *buffer, size_t size) {
    if (!nes || !buffer) {
        return NES_ERROR_INVALID_ARGUMENT;
    }

    /* TODO: Implement actual load state */
    (void)size;
    return NES_OK;
}

/* ============================================================================
 * Debugging
 * ============================================================================ */

uint16_t nes_get_cpu_pc(const nes_t *nes) {
    if (!nes) {
        return 0;
    }

    return nes->core.cpu.PC;
}

void nes_get_cpu_registers(const nes_t *nes, uint8_t *a, uint8_t *x,
                        uint8_t *y, uint8_t *sp, uint8_t *p) {
    if (!nes) {
        return;
    }

    if (a) *a = nes->core.cpu.A;
    if (x) *x = nes->core.cpu.X;
    if (y) *y = nes->core.cpu.Y;
    if (sp) *sp = nes->core.cpu.SP;
    if (p) *p = nes->core.cpu.P;
}

uint8_t nes_read_cpu_memory(const nes_t *nes, uint16_t address) {
    if (!nes) {
        return 0;
    }

    return nes_cpu_read((CPU *)&nes->core.cpu, address);
}

uint8_t nes_read_ppu_memory(const nes_t *nes, uint16_t address) {
    if (!nes) {
        return 0;
    }

    return ppu_read((PPU *)&nes->core.ppu, address);
}

void nes_get_ppu_position(const nes_t *nes, int *scanline, int *dot) {
    if (!nes) {
        if (scanline) *scanline = 0;
        if (dot) *dot = 0;
        return;
    }

    if (scanline) *scanline = nes->core.ppu.scanline;
    if (dot) *dot = nes->core.ppu.dot;
}

uint64_t nes_get_cpu_cycles(const nes_t *nes) {
    return nes ? nes->cpu_cycles : 0;
}

uint64_t nes_get_frame_count(const nes_t *nes) {
    return nes ? nes->frame_count : 0;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

const char *nes_error_string(nes_error_t error) {
    switch (error) {
        case NES_OK:                     return "Success";
        case NES_ERROR_INVALID_ARGUMENT:  return "Invalid argument";
        case NES_ERROR_OUT_OF_MEMORY:    return "Out of memory";
        case NES_ERROR_INVALID_ROM:      return "Invalid ROM";
        case NES_ERROR_UNSUPPORTED_MAPPER: return "Unsupported mapper";
        case NES_ERROR_FILE_NOT_FOUND:   return "File not found";
        case NES_ERROR_FILE_READ:        return "File read error";
        case NES_ERROR_INVALID_STATE:    return "Invalid state";
        default:                         return "Unknown error";
    }
}

const char *nes_version(void) {
    return NES_VERSION_STRING;
}
