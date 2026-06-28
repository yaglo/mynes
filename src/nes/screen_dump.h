/*
 * Screen-to-Text Dump
 *
 * Reads PPU nametable and converts tile indices to ASCII characters
 * using a configurable character map. Useful for automated test verification.
 */

#ifndef NES_SCREEN_DUMP_H
#define NES_SCREEN_DUMP_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* PPU type — defined in ppu/ppu.h, forward-declared here */
typedef struct PPU PPU;

/* ============================================================================
 * Character Map
 * ============================================================================ */

typedef struct {
    char tile_map[256];  /* tile index -> ASCII character */
} CharMap;

/* Set default character map (AccuracyCoin-compatible layout):
 *   $00-$09 -> '0'-'9'
 *   $0A-$23 -> 'A'-'Z'
 *   $24     -> ' '
 *   else    -> '.'
 */
void charmap_set_default(CharMap *cm);

/* Load character map from file.
 * Format: hex=char per line, # comments. Example:
 *   00=0
 *   0A=A
 *   24=
 * Returns true on success. */
bool charmap_load(CharMap *cm, const char *filename);

/* ============================================================================
 * Screen Dump
 * ============================================================================ */

/* Dump the first nametable (32x30 tiles) as ASCII text.
 * Writes 30 lines of 32 chars each to the output buffer.
 * buf must be at least 30*33+1 bytes (30 lines * (32 chars + newline) + null).
 * Returns number of chars written. */
int screen_dump_to_buffer(const PPU *ppu, const CharMap *cm,
                          char *buf, int buf_size);

/* Dump nametable to a FILE handle */
void screen_dump_to_file(const PPU *ppu, const CharMap *cm, FILE *fp);

/* Dump nametable to stdout */
void screen_dump_print(const PPU *ppu, const CharMap *cm);

#endif /* NES_SCREEN_DUMP_H */
