/*
 * saves.h — battery-backed cartridge RAM and save-state files.
 *
 * Both live beside config.json (see config.h for how that directory is
 * resolved):
 *   <config-dir>/saves/<name>.sav    the cartridge's PRG RAM: the 8 KB at
 *                                    $6000-$7FFF, or MMC5's 64 KB of pages
 *   <config-dir>/states/<name>.s<N>  save-state slots 1..MYNES_STATE_SLOTS
 * <name> is the ROM file's basename without its extension plus the ROM's
 * CRC-32, so two dumps that happen to share a file name never share a save,
 * and renaming a ROM does not orphan its progress as long as the CRC is
 * visible in the file name.
 */
#ifndef MYNES_SAVES_H
#define MYNES_SAVES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "config.h"

#define MYNES_PRG_RAM_SIZE   0x2000    /* the usual cartridge RAM */
#define MYNES_PRG_RAM_MAX    0x10000   /* the most a cartridge has (MMC5) */
#define MYNES_STATE_SLOTS    4
#define MYNES_SAVE_NAME_MAX  128

typedef struct {
    bool     battery;                       /* the cartridge asks for battery RAM */
    uint32_t rom_crc;
    char     name[MYNES_SAVE_NAME_MAX];     /* "<basename>-<crc32>" */
    char     sav_path[MYNES_PATH_MAX];
    size_t   ram_size;                      /* bytes of PRG RAM the file holds */
    uint8_t  on_disk[MYNES_PRG_RAM_MAX];    /* PRG RAM as last read from or written to disk */
} MynesSaves;

/* Directory paths; created lazily by the write helpers, never here. */
void mynes_saves_dir(char *out, int out_sz);
void mynes_states_dir(char *out, int out_sz);

/* "<basename without extension>-<8 hex digits>" for rom_path and rom_crc. */
void mynes_save_basename(const char *rom_path, uint32_t rom_crc, char *out, int out_sz);

/* Bind `s` to a cartridge whose PRG RAM is ram_size bytes (Mapper.prg_ram_size,
 * at most MYNES_PRG_RAM_MAX). Nothing is read from disk yet. */
void mynes_saves_open(MynesSaves *s, const char *rom_path, uint32_t rom_crc, bool has_battery,
                      size_t ram_size);

/* Copy <saves>/<name>.sav into prg_ram (ram_size bytes). Returns true when a
 * file was read; a missing file leaves prg_ram alone (a fresh cartridge) and
 * returns false. A shorter file, such as an 8 KB save from before MMC5 kept
 * all its pages, fills the start of the RAM and leaves the rest. */
bool mynes_saves_restore(MynesSaves *s, uint8_t *prg_ram);

/* Write ram_size bytes of prg_ram to <saves>/<name>.sav when they differ from the copy on disk,
 * or unconditionally with `force`. A cartridge without battery RAM never
 * writes. Returns false only when a write was attempted and failed. */
bool mynes_saves_flush(MynesSaves *s, const uint8_t *prg_ram, bool force);

/* Save-state slots, numbered 1..MYNES_STATE_SLOTS as shown to the player. */
void  mynes_state_path(const MynesSaves *s, int slot, char *out, int out_sz);
bool  mynes_state_write(const MynesSaves *s, int slot, const void *data, size_t size);
/* Whole file, malloc'd; NULL when the slot is empty or unreadable. */
void *mynes_state_read(const MynesSaves *s, int slot, size_t *size);

/* Write through a temporary file and rename it into place, so an interrupted
 * write leaves the previous file intact rather than a truncated one. */
bool mynes_write_file_atomic(const char *path, const void *data, size_t size);

#endif /* MYNES_SAVES_H */
