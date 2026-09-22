/*
 * Save states
 *
 * A state is a fixed header followed by the raw bytes of the NES struct.
 * The struct embeds the CPU, PPU, APU and mapper by value, so one copy
 * captures the whole machine; the header ties the image to one build
 * (struct size), one cartridge (mapper, sizes, ROM CRC) and one region.
 * Pointers, callbacks and cartridge geometry inside the image are never
 * trusted on load: the live instance keeps its own (see state.c for the
 * list), and the header's CRC of the image refuses a body that changed
 * after it was written.
 */

#ifndef NES_STATE_H
#define NES_STATE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "nes.h"

#define NES_STATE_MAGIC   "MYNESST"   /* 7 characters plus the terminator */
#define NES_STATE_VERSION 2u

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t struct_size;   /* sizeof(NES) of the build that wrote the state */
    uint32_t prg_size;
    uint32_t chr_size;
    uint32_t mapper;
    uint32_t region;        /* PPU_REGION_NTSC / PPU_REGION_PAL */
    uint32_t rom_crc;       /* nes_state_rom_crc() of PRG then CHR */
    uint32_t image_crc;     /* nes_crc32(0, image, struct_size) as written */
} NESStateHeader;

/* CRC-32 over PRG ROM followed by CHR ROM, the identity used to match a
 * state (or a battery file) to its cartridge regardless of file name. */
uint32_t nes_state_rom_crc(const uint8_t *prg, uint32_t prg_size,
                           const uint8_t *chr, uint32_t chr_size);

/* Bytes needed by nes_state_save for this instance. */
size_t nes_state_size(const NES *nes);

/* Writes header + image into buf. Fails only when buf is too small. */
bool nes_state_save(const NES *nes, void *buf, size_t size);

/* Validates the header against the live instance and, only when every
 * check passes, replaces its state. On failure the instance is untouched
 * and `error` (when given) holds a one-line reason for the user. */
bool nes_state_load(NES *nes, const void *buf, size_t size,
                    char *error, size_t error_size);

#endif /* NES_STATE_H */
