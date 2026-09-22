/*
 * NES ROM Loader (iNES Format)
 *
 * Parses iNES format ROM files (.nes) and loads PRG/CHR data.
 * Supports mappers: 0, 1, 2, 3, 4, 5, 7, 10, 69, and 227.
 */

#ifndef NES_ROM_H
#define NES_ROM_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ============================================================================
 * iNES Header Format
 * ============================================================================
 *
 * Offset  Size  Description
 * ------  ----  -----------
 * 0-3     4     Magic: "NES\x1A"
 * 4       1     PRG ROM size (16KB units)
 * 5       1     CHR ROM size (8KB units, 0 = CHR RAM)
 * 6       1     Flags 6: Mirroring, battery, trainer, mapper low
 * 7       1     Flags 7: Mapper high, VS/Playchoice, NES 2.0
 * 8-15    8     Padding (zeros in iNES 1.0)
 */

#define INES_HEADER_SIZE 16
#define INES_PRG_BANK_SIZE 16384  /* 16KB */
#define INES_CHR_BANK_SIZE 8192   /* 8KB */
#define INES_TRAINER_SIZE 512

/* Error codes */
#define ROM_OK                0
#define ROM_ERR_FILE         -1  /* File not found or read error */
#define ROM_ERR_HEADER       -2  /* Invalid iNES header */
#define ROM_ERR_MAPPER       -3  /* Unsupported mapper */
#define ROM_ERR_ALLOC        -4  /* Memory allocation failed */

/* ============================================================================
 * ROM Structure
 * ============================================================================ */

/* TV system / region */
#define NES_TV_NTSC        0
#define NES_TV_PAL         1
#define NES_TV_MULTI       2  /* Multi-region */
#define NES_TV_DENDY       3

typedef struct {
    uint8_t *prg_rom;       /* PRG ROM data (allocated) */
    uint32_t prg_size;      /* PRG ROM size in bytes */
    uint8_t *chr_rom;       /* CHR ROM data (allocated, NULL if CHR RAM) */
    uint32_t chr_size;      /* CHR ROM size in bytes (0 = CHR RAM) */
    uint8_t mapper;         /* Mapper number */
    uint8_t mirroring;      /* 0=horizontal, 1=vertical */
    uint8_t tv_system;      /* NES_TV_NTSC / NES_TV_PAL / NES_TV_MULTI / NES_TV_DENDY */
    bool region_from_filename; /* Legacy header corrected by explicit PAL filename tag */
    bool is_nes2;           /* true if NES 2.0 header detected */
    bool has_battery;       /* Battery-backed RAM */
    bool has_trainer;       /* 512-byte trainer present */
} ROM;

/* ============================================================================
 * ROM Loading
 * ============================================================================ */

/* Older iNES dumps often leave byte 9 at zero even for PAL releases.
 * Use explicit, conventional tags in the basename only as a fallback.
 * A NES 2.0 timing declaration or an explicit iNES PAL bit wins. */
static inline bool nes_rom_pal_filename(const char *path) {
    const char *base = path;
    for (const char *p = path; *p; ++p)
        if (*p == '/' || *p == '\\') base = p + 1;
    const char *tags[] = {"(e)", "(europe)", "(pal)", "(australia)",
                          "(europe, australia)"};
    for (const char *p = base; *p; ++p) {
        for (size_t i = 0; i < sizeof(tags)/sizeof(tags[0]); ++i) {
            size_t j = 0;
            while (p[j] && tags[i][j]) {
                char c = p[j];
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != tags[i][j]) break;
                ++j;
            }
            if (!tags[i][j]) return true;
        }
    }
    return false;
}

/* Load ROM from file path */
static inline int nes_rom_load(ROM *rom, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ROM_ERR_FILE;
    }

    /* Initialize ROM struct */
    memset(rom, 0, sizeof(ROM));

    /* Read header */
    uint8_t header[INES_HEADER_SIZE];
    if (fread(header, 1, INES_HEADER_SIZE, fp) != INES_HEADER_SIZE) {
        fclose(fp);
        return ROM_ERR_FILE;
    }

    /* Verify magic number: "NES\x1A" */
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A) {
        fclose(fp);
        return ROM_ERR_HEADER;
    }

    /* Parse header */
    uint8_t prg_banks = header[4];
    uint8_t chr_banks = header[5];
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];

    /*
     * Detect NES 2.0 FIRST, from the raw flags7. NES 2.0 is signaled by
     * flags7 bits 2-3 == 0b10, and NES 2.0 ROMs legitimately use bytes
     * 12-15 for extended fields (TV system, mapper-plane, PRG/CHR-RAM
     * size, etc.) — so those bytes being non-zero is EXPECTED for NES 2.0
     * and must not trigger the archaic-iNES fallback.
     *
     * Archaic iNES / "DiskDude!" heuristic: many old dumps (e.g. Blaster
     * Master (U)) contain ASCII garbage in bytes 7-15 of the header. Only
     * apply the "zero out flags7's mapper bits" fallback when we've
     * already ruled out NES 2.0 AND bytes 12-15 are non-zero (so the
     * header looks like it has junk rather than legit NES 2.0 data).
     */
    bool is_nes2 = ((flags7 & 0x0C) == 0x08);
    bool header_bytes_12_15_zero =
        (header[12] | header[13] | header[14] | header[15]) == 0;
    if (!is_nes2 && !header_bytes_12_15_zero) {
        flags7 = 0;
    }

    rom->prg_size = prg_banks * INES_PRG_BANK_SIZE;
    rom->chr_size = chr_banks * INES_CHR_BANK_SIZE;
    rom->mirroring = (flags6 & 0x01);           /* Bit 0: mirroring */
    rom->has_battery = (flags6 & 0x02) != 0;    /* Bit 1: battery */
    rom->has_trainer = (flags6 & 0x04) != 0;    /* Bit 2: trainer */
    rom->mapper = ((flags6 >> 4) & 0x0F) | (flags7 & 0xF0);
    rom->is_nes2 = is_nes2;

    /* TV system detection */
    if (rom->is_nes2) {
        /* NES 2.0: byte 12 bits 0-1 */
        rom->tv_system = header[12] & 0x03;
    } else if (header_bytes_12_15_zero) {
        /* iNES 1.0: byte 9 bit 0. Only trust it when the archaic-iNES
         * garbage heuristic says bytes 12-15 look clean; otherwise byte 9
         * is part of the junk (e.g. "DiskDude!" dumps) and we default to
         * NTSC. */
        rom->tv_system = (header[9] & 0x01) ? NES_TV_PAL : NES_TV_NTSC;
    } else {
        rom->tv_system = NES_TV_NTSC;
    }

    if (!rom->is_nes2 && rom->tv_system == NES_TV_NTSC &&
        nes_rom_pal_filename(path)) {
        rom->tv_system = NES_TV_PAL;
        rom->region_from_filename = true;
    }

    /* Check for supported mappers. */
    if (rom->mapper > 5 && rom->mapper != 7 && rom->mapper != 10 &&
        rom->mapper != 69 && rom->mapper != 227) {
        fclose(fp);
        return ROM_ERR_MAPPER;
    }

    /* Skip trainer if present */
    if (rom->has_trainer) {
        fseek(fp, INES_TRAINER_SIZE, SEEK_CUR);
    }

    /* Allocate and read PRG ROM */
    if (rom->prg_size > 0) {
        rom->prg_rom = (uint8_t *)malloc(rom->prg_size);
        if (!rom->prg_rom) {
            fclose(fp);
            return ROM_ERR_ALLOC;
        }
        if (fread(rom->prg_rom, 1, rom->prg_size, fp) != rom->prg_size) {
            free(rom->prg_rom);
            rom->prg_rom = NULL;
            fclose(fp);
            return ROM_ERR_FILE;
        }
    }

    /* Allocate and read CHR ROM (if present) */
    if (rom->chr_size > 0) {
        rom->chr_rom = (uint8_t *)malloc(rom->chr_size);
        if (!rom->chr_rom) {
            free(rom->prg_rom);
            rom->prg_rom = NULL;
            fclose(fp);
            return ROM_ERR_ALLOC;
        }
        if (fread(rom->chr_rom, 1, rom->chr_size, fp) != rom->chr_size) {
            free(rom->prg_rom);
            free(rom->chr_rom);
            rom->prg_rom = NULL;
            rom->chr_rom = NULL;
            fclose(fp);
            return ROM_ERR_FILE;
        }
    }

    fclose(fp);
    return ROM_OK;
}

/* Load ROM from an in-memory buffer (e.g. drag-and-drop, network).
 * Same header parsing as nes_rom_load but reads from (data, size)
 * instead of a FILE*. Allocates PRG/CHR ROM via malloc. */
static inline int nes_rom_load_data(ROM *rom, const uint8_t *data, size_t size) {
    memset(rom, 0, sizeof(ROM));

    if (size < INES_HEADER_SIZE) return ROM_ERR_FILE;

    const uint8_t *header = data;
    if (header[0] != 'N' || header[1] != 'E' || header[2] != 'S' || header[3] != 0x1A)
        return ROM_ERR_HEADER;

    uint8_t prg_banks = header[4];
    uint8_t chr_banks = header[5];
    uint8_t flags6 = header[6];
    uint8_t flags7 = header[7];

    bool is_nes2 = ((flags7 & 0x0C) == 0x08);
    bool header_bytes_12_15_zero =
        (header[12] | header[13] | header[14] | header[15]) == 0;
    if (!is_nes2 && !header_bytes_12_15_zero) flags7 = 0;

    rom->prg_size = prg_banks * INES_PRG_BANK_SIZE;
    rom->chr_size = chr_banks * INES_CHR_BANK_SIZE;
    rom->mirroring = (flags6 & 0x01);
    rom->has_battery = (flags6 & 0x02) != 0;
    rom->has_trainer = (flags6 & 0x04) != 0;
    rom->mapper = ((flags6 >> 4) & 0x0F) | (flags7 & 0xF0);
    rom->is_nes2 = is_nes2;

    if (rom->is_nes2) {
        rom->tv_system = header[12] & 0x03;
    } else if (header_bytes_12_15_zero) {
        rom->tv_system = (header[9] & 0x01) ? NES_TV_PAL : NES_TV_NTSC;
    } else {
        rom->tv_system = NES_TV_NTSC;
    }

    if (rom->mapper > 5 && rom->mapper != 7 && rom->mapper != 10 &&
        rom->mapper != 69 && rom->mapper != 227)
        return ROM_ERR_MAPPER;

    size_t offset = INES_HEADER_SIZE;
    if (rom->has_trainer) offset += INES_TRAINER_SIZE;

    if (size < offset + rom->prg_size + rom->chr_size)
        return ROM_ERR_FILE;

    if (rom->prg_size > 0) {
        rom->prg_rom = (uint8_t *)malloc(rom->prg_size);
        if (!rom->prg_rom) return ROM_ERR_ALLOC;
        memcpy(rom->prg_rom, data + offset, rom->prg_size);
        offset += rom->prg_size;
    }
    if (rom->chr_size > 0) {
        rom->chr_rom = (uint8_t *)malloc(rom->chr_size);
        if (!rom->chr_rom) { free(rom->prg_rom); rom->prg_rom = NULL; return ROM_ERR_ALLOC; }
        memcpy(rom->chr_rom, data + offset, rom->chr_size);
    }
    return ROM_OK;
}

/* Free ROM memory */
static inline void nes_rom_free(ROM *rom) {
    if (rom->prg_rom) {
        free(rom->prg_rom);
        rom->prg_rom = NULL;
    }
    if (rom->chr_rom) {
        free(rom->chr_rom);
        rom->chr_rom = NULL;
    }
    rom->prg_size = 0;
    rom->chr_size = 0;
}

/* ============================================================================
 * ROM Info
 * ============================================================================ */

/* Get error message for error code */
static inline const char *nes_rom_error_str(int err) {
    switch (err) {
        case ROM_OK:         return "Success";
        case ROM_ERR_FILE:   return "File not found or read error";
        case ROM_ERR_HEADER: return "Invalid iNES header";
        case ROM_ERR_MAPPER: return "Unsupported mapper";
        case ROM_ERR_ALLOC:  return "Memory allocation failed";
        default:             return "Unknown error";
    }
}

/* Print ROM info */
static inline void nes_rom_print_info(const ROM *rom) {
    printf("ROM Info:\n");
    printf("  PRG ROM: %u KB (%u banks)\n", rom->prg_size / 1024, rom->prg_size / INES_PRG_BANK_SIZE);
    printf("  CHR %s: %u KB (%u banks)\n",
           rom->chr_size > 0 ? "ROM" : "RAM",
           rom->chr_size > 0 ? rom->chr_size / 1024 : 8,
           rom->chr_size > 0 ? rom->chr_size / INES_CHR_BANK_SIZE : 1);
    printf("  Mapper: %u\n", rom->mapper);
    printf("  Mirroring: %s\n", rom->mirroring ? "Vertical" : "Horizontal");
    const char *tv_names[] = {"NTSC", "PAL", "Multi-region", "Dendy"};
    printf("  TV System: %s%s\n", tv_names[rom->tv_system & 3],
           rom->is_nes2 ? " (NES 2.0)" :
           rom->region_from_filename ? " (filename fallback)" : "");
    printf("  Battery: %s\n", rom->has_battery ? "Yes" : "No");
    printf("  Trainer: %s\n", rom->has_trainer ? "Yes" : "No");
}

#endif /* NES_ROM_H */
