/*
 * NES Emulator - Common Types
 *
 * This file defines common types and constants used throughout the NES emulator.
 * Part of the public API.
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#ifndef NES_TYPES_H
#define NES_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Version Information
 * ============================================================================ */

#define NES_VERSION_MAJOR 0
#define NES_VERSION_MINOR 2
#define NES_VERSION_PATCH 0
#define NES_VERSION_STRING "0.2.0"

/* ============================================================================
 * System Constants
 * ============================================================================ */

/* Timing (NTSC) */
#define NES_MASTER_CLOCK_NTSC   21477272    /* Hz */
#define NES_CPU_CLOCK_DIVIDER   12          /* Master / 12 = CPU clock */
#define NES_PPU_CLOCK_DIVIDER   4           /* Master / 4 = PPU clock */
#define NES_CPU_CLOCK_NTSC      1789773     /* Hz (approximate) */
#define NES_PPU_CLOCK_NTSC      5369318     /* Hz (approximate) */

/* PPU Constants */
#define NES_SCREEN_WIDTH        256
#define NES_SCREEN_HEIGHT       240
#define NES_PPU_DOTS_PER_LINE   341
#define NES_PPU_SCANLINES       262         /* NTSC */
#define NES_PPU_VBLANK_START    241
#define NES_PPU_PRERENDER_LINE  261

/* Memory Sizes */
#define NES_RAM_SIZE            0x800       /* 2KB internal RAM */
#define NES_VRAM_SIZE           0x800       /* 2KB video RAM */
#define NES_OAM_SIZE            256         /* 64 sprites * 4 bytes */
#define NES_PALETTE_SIZE        32          /* 32 palette entries */

/* Controller Buttons */
#define NES_BUTTON_A            0x01
#define NES_BUTTON_B            0x02
#define NES_BUTTON_SELECT       0x04
#define NES_BUTTON_START        0x08
#define NES_BUTTON_UP           0x10
#define NES_BUTTON_DOWN         0x20
#define NES_BUTTON_LEFT         0x40
#define NES_BUTTON_RIGHT        0x80

/* ============================================================================
 * Pixel Format
 * ============================================================================ */

/* RGB pixel (24-bit) */
typedef struct nes_rgb {
    uint8_t r, g, b;
} nes_rgb_t;

/* RGBA pixel (32-bit) for easier integration */
typedef struct nes_rgba {
    uint8_t r, g, b, a;
} nes_rgba_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

typedef enum nes_error {
    NES_OK = 0,
    NES_ERROR_INVALID_ARGUMENT,
    NES_ERROR_OUT_OF_MEMORY,
    NES_ERROR_INVALID_ROM,
    NES_ERROR_UNSUPPORTED_MAPPER,
    NES_ERROR_FILE_NOT_FOUND,
    NES_ERROR_FILE_READ,
    NES_ERROR_INVALID_STATE,
} nes_error_t;

/* ============================================================================
 * Mirroring Modes
 * ============================================================================ */

typedef enum nes_mirroring {
    NES_MIRROR_HORIZONTAL = 0,
    NES_MIRROR_VERTICAL = 1,
    NES_MIRROR_SINGLE_LOWER = 2,
    NES_MIRROR_SINGLE_UPPER = 3,
    NES_MIRROR_FOUR_SCREEN = 4,
} nes_mirroring_t;

/* ============================================================================
 * ROM Information
 * ============================================================================ */

typedef struct nes_rom_info {
    uint16_t mapper;            /* Mapper number */
    uint8_t prg_rom_banks;      /* 16KB PRG ROM banks */
    uint8_t chr_rom_banks;      /* 8KB CHR ROM banks */
    uint8_t prg_ram_banks;      /* 8KB PRG RAM banks */
    nes_mirroring_t mirroring;  /* Nametable mirroring */
    bool has_battery;           /* Battery-backed RAM */
    bool has_trainer;           /* 512-byte trainer present */
} nes_rom_info_t;

/* ============================================================================
 * Callback Types
 * ============================================================================ */

/* Audio sample callback - called when audio samples are ready */
typedef void (*nes_audio_callback_t)(void *user_data, const float *samples, size_t count);

/* Video frame callback - called when a frame is complete */
typedef void (*nes_video_callback_t)(void *user_data, const nes_rgb_t *framebuffer);

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

typedef struct nes nes_t;
typedef struct nes_rom nes_rom_t;

#ifdef __cplusplus
}
#endif

#endif /* NES_TYPES_H */
