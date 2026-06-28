/*
 * NES Emulator - Public API
 *
 * A cycle-accurate NES emulator library with declarative DSL-based components.
 * This library is platform-agnostic and can be used with any frontend:
 *   - SDL2 (desktop)
 *   - SwiftUI / AppKit (macOS/iOS)
 *   - Metal / OpenGL / Vulkan
 *   - ESP32-S3-BOX (embedded)
 *   - WebAssembly
 *
 * The core library has no platform dependencies - it only requires:
 *   - C11 compiler
 *   - Standard library (stdint.h, stdbool.h, stddef.h, string.h, stdlib.h)
 *
 * Basic usage:
 *   nes_t *nes = nes_create();
 *   nes_load_rom_file(nes, "game.nes");
 *   while (running) {
 *       nes_run_frame(nes);
 *       display(nes_get_framebuffer(nes));
 *       nes_set_controller(nes, 0, buttons);
 *   }
 *   nes_destroy(nes);
 *
 * Copyright (c) 2024
 * SPDX-License-Identifier: MIT
 */

#ifndef NES_NES_H
#define NES_NES_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * Create a new NES emulator instance.
 *
 * @return Pointer to the emulator instance, or NULL on failure.
 */
nes_t *nes_create(void);

/**
 * Destroy an emulator instance and free all resources.
 *
 * @param nes The emulator instance to destroy (may be NULL).
 */
void nes_destroy(nes_t *nes);

/**
 * Reset the emulator to power-on state.
 *
 * @param nes The emulator instance.
 */
void nes_reset(nes_t *nes);

/* ============================================================================
 * ROM Loading
 * ============================================================================ */

/**
 * Load a ROM from a file.
 *
 * @param nes The emulator instance.
 * @param path Path to the ROM file (.nes format).
 * @return NES_OK on success, error code on failure.
 */
nes_error_t nes_load_rom_file(nes_t *nes, const char *path);

/**
 * Load a ROM from memory.
 *
 * @param nes The emulator instance.
 * @param data Pointer to ROM data in iNES format.
 * @param size Size of the ROM data in bytes.
 * @return NES_OK on success, error code on failure.
 */
nes_error_t nes_load_rom_data(nes_t *nes, const uint8_t *data, size_t size);

/**
 * Unload the current ROM.
 *
 * @param nes The emulator instance.
 */
void nes_unload_rom(nes_t *nes);

/**
 * Get information about the loaded ROM.
 *
 * @param nes The emulator instance.
 * @param info Pointer to structure to fill with ROM info.
 * @return NES_OK on success, NES_ERROR_INVALID_STATE if no ROM loaded.
 */
nes_error_t nes_get_rom_info(const nes_t *nes, nes_rom_info_t *info);

/* ============================================================================
 * Emulation
 * ============================================================================ */

/**
 * Run emulation for one frame (approximately 29780 CPU cycles).
 *
 * @param nes The emulator instance.
 */
void nes_run_frame(nes_t *nes);

/**
 * Run emulation for a specified number of CPU cycles.
 *
 * @param nes The emulator instance.
 * @param cycles Number of CPU cycles to execute.
 * @return Actual number of cycles executed.
 */
uint32_t nes_run_cycles(nes_t *nes, uint32_t cycles);

/**
 * Run emulation for a single CPU cycle.
 * Use this for debugging or precise timing control.
 *
 * @param nes The emulator instance.
 */
void nes_step(nes_t *nes);

/**
 * Check if the current frame is complete.
 *
 * @param nes The emulator instance.
 * @return true if frame is complete and ready for display.
 */
bool nes_frame_complete(const nes_t *nes);

/* ============================================================================
 * Video Output
 * ============================================================================ */

/**
 * Get the framebuffer for the current frame.
 *
 * @param nes The emulator instance.
 * @return Pointer to framebuffer (256x240 RGB pixels), or NULL.
 */
const nes_rgb_t *nes_get_framebuffer(const nes_t *nes);

/**
 * Set a callback to be called when a frame is complete.
 *
 * @param nes The emulator instance.
 * @param callback The callback function (may be NULL to disable).
 * @param user_data User data passed to the callback.
 */
void nes_set_video_callback(nes_t *nes, nes_video_callback_t callback, void *user_data);

/**
 * Copy framebuffer to a destination buffer in RGBA format (32-bit).
 * Useful for GPU textures that expect RGBA8888 format.
 *
 * @param nes The emulator instance.
 * @param dest Destination buffer (must be at least 256*240*4 bytes).
 * @param alpha Alpha value to use (typically 255 for opaque).
 */
void nes_get_framebuffer_rgba(const nes_t *nes, uint8_t *dest, uint8_t alpha);

/**
 * Copy framebuffer to a destination buffer in BGRA format (32-bit).
 * Useful for Windows/DirectX textures.
 *
 * @param nes The emulator instance.
 * @param dest Destination buffer (must be at least 256*240*4 bytes).
 * @param alpha Alpha value to use (typically 255 for opaque).
 */
void nes_get_framebuffer_bgra(const nes_t *nes, uint8_t *dest, uint8_t alpha);

/**
 * Copy framebuffer to a destination buffer in RGB565 format (16-bit).
 * Useful for embedded displays and low-memory systems.
 *
 * @param nes The emulator instance.
 * @param dest Destination buffer (must be at least 256*240*2 bytes).
 */
void nes_get_framebuffer_rgb565(const nes_t *nes, uint16_t *dest);

/* ============================================================================
 * Audio Output
 * ============================================================================ */

/**
 * Set the audio sample rate.
 *
 * @param nes The emulator instance.
 * @param sample_rate Sample rate in Hz (e.g., 44100, 48000).
 */
void nes_set_audio_sample_rate(nes_t *nes, uint32_t sample_rate);

/**
 * Set a callback to receive audio samples.
 *
 * @param nes The emulator instance.
 * @param callback The callback function (may be NULL to disable).
 * @param user_data User data passed to the callback.
 */
void nes_set_audio_callback(nes_t *nes, nes_audio_callback_t callback, void *user_data);

/* ============================================================================
 * Input
 * ============================================================================ */

/**
 * Set controller button state.
 *
 * @param nes The emulator instance.
 * @param controller Controller number (0 or 1).
 * @param buttons Button state (OR of NES_BUTTON_* constants).
 */
void nes_set_controller(nes_t *nes, int controller, uint8_t buttons);

/**
 * Get current controller button state.
 *
 * @param nes The emulator instance.
 * @param controller Controller number (0 or 1).
 * @return Current button state.
 */
uint8_t nes_get_controller(const nes_t *nes, int controller);

/* ============================================================================
 * Save States
 * ============================================================================ */

/**
 * Get the size needed for a save state.
 *
 * @param nes The emulator instance.
 * @return Size in bytes needed for save state.
 */
size_t nes_save_state_size(const nes_t *nes);

/**
 * Save the current state to a buffer.
 *
 * @param nes The emulator instance.
 * @param buffer Buffer to save state to.
 * @param size Size of the buffer.
 * @return NES_OK on success, error code on failure.
 */
nes_error_t nes_save_state(const nes_t *nes, void *buffer, size_t size);

/**
 * Load state from a buffer.
 *
 * @param nes The emulator instance.
 * @param buffer Buffer containing saved state.
 * @param size Size of the buffer.
 * @return NES_OK on success, error code on failure.
 */
nes_error_t nes_load_state(nes_t *nes, const void *buffer, size_t size);

/* ============================================================================
 * Debugging
 * ============================================================================ */

/**
 * Get current CPU program counter.
 *
 * @param nes The emulator instance.
 * @return Current PC value.
 */
uint16_t nes_get_cpu_pc(const nes_t *nes);

/**
 * Get CPU register values.
 *
 * @param nes The emulator instance.
 * @param a Pointer to store A register (may be NULL).
 * @param x Pointer to store X register (may be NULL).
 * @param y Pointer to store Y register (may be NULL).
 * @param sp Pointer to store SP register (may be NULL).
 * @param p Pointer to store P (status) register (may be NULL).
 */
void nes_get_cpu_registers(const nes_t *nes, uint8_t *a, uint8_t *x,
                           uint8_t *y, uint8_t *sp, uint8_t *p);

/**
 * Read a byte from CPU memory space.
 *
 * @param nes The emulator instance.
 * @param address Memory address (0x0000-0xFFFF).
 * @return Value at the address.
 */
uint8_t nes_read_cpu_memory(const nes_t *nes, uint16_t address);

/**
 * Read a byte from PPU memory space.
 *
 * @param nes The emulator instance.
 * @param address Memory address (0x0000-0x3FFF).
 * @return Value at the address.
 */
uint8_t nes_read_ppu_memory(const nes_t *nes, uint16_t address);

/**
 * Get the current PPU scanline and dot.
 *
 * @param nes The emulator instance.
 * @param scanline Pointer to store scanline (may be NULL).
 * @param dot Pointer to store dot (may be NULL).
 */
void nes_get_ppu_position(const nes_t *nes, int *scanline, int *dot);

/**
 * Get total CPU cycles executed.
 *
 * @param nes The emulator instance.
 * @return Total cycle count.
 */
uint64_t nes_get_cpu_cycles(const nes_t *nes);

/**
 * Get current frame number.
 *
 * @param nes The emulator instance.
 * @return Frame count since reset.
 */
uint64_t nes_get_frame_count(const nes_t *nes);

/* ============================================================================
 * Utility
 * ============================================================================ */

/**
 * Get a human-readable error message.
 *
 * @param error The error code.
 * @return Static string describing the error.
 */
const char *nes_error_string(nes_error_t error);

/**
 * Get the library version string.
 *
 * @return Version string (e.g., "0.2.0").
 */
const char *nes_version(void);

#ifdef __cplusplus
}
#endif

#endif /* NES_NES_H */
