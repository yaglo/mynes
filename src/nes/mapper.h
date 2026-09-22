/*
 * NES Mapper System
 *
 * Interface and shared state for cartridge mappers.
 */

#ifndef NES_MAPPER_H
#define NES_MAPPER_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Forward declarations */
struct NES;

typedef struct Mapper {
    uint8_t number;         /* Mapper number */

    /* PRG ROM banking */
    uint8_t *prg_rom;
    uint32_t prg_rom_size;
    uint8_t prg_banks;      /* Number of 16KB PRG banks */
    uint8_t prg_bank0;      /* Bank at $8000-$BFFF */
    uint8_t prg_bank1;      /* Bank at $C000-$FFFF */
    uint8_t prg_mode;       /* PRG banking mode */

    /* CHR ROM/RAM banking */
    uint8_t *chr_rom;
    uint32_t chr_rom_size;
    uint8_t chr_banks;      /* Number of 8KB CHR banks (or 4KB for some mappers) */
    uint8_t chr_bank0;      /* Bank at $0000-$0FFF (or $0000-$1FFF) */
    uint8_t chr_bank1;      /* Bank at $1000-$1FFF */
    uint8_t chr_ram[0x2000]; /* CHR RAM for mappers that use it */
    bool has_chr_ram;

    /* PRG RAM */
    uint8_t prg_ram[0x2000];
    bool prg_ram_enabled;
    bool prg_ram_write_protect;

    /* Mirroring */
    uint8_t mirroring;      /* 0=horizontal, 1=vertical, 2=single-screen low, 3=single-screen high */

    /* MMC1 specific */
    uint8_t mmc1_shift;     /* Shift register */
    uint8_t mmc1_shift_count;
    uint8_t mmc1_control;   /* Control register */
    uint8_t mmc1_prg_bank;  /* Raw PRG bank register */
    bool mmc1_chr_mode;     /* true = 4KB CHR banks, false = 8KB */
    uint64_t mmc1_last_write_cycle;
    bool mmc1_last_write_valid;

    /* MMC3 specific; Namco 108 (mapper 206) shares the bank registers */
    uint8_t mmc3_bank_select;
    uint8_t mmc3_banks[8];
    uint8_t mmc3_irq_latch;
    uint8_t mmc3_irq_counter;
    bool mmc3_irq_enabled;
    bool mmc3_irq_reload;
    bool mmc3_a12_high;
    uint16_t mmc3_a12_low_cycles;
    uint64_t mmc3_a12_last_low_cpu_cycle;
    uint8_t mmc3_a12_low_cpu_cycles;

    /* MMC2/MMC4 specific (MMC2's PRG bank is 8 KB, MMC4's 16 KB) */
    uint8_t mmc4_prg_bank;
    uint8_t mmc4_chr_bank0_fd;
    uint8_t mmc4_chr_bank0_fe;
    uint8_t mmc4_chr_bank1_fd;
    uint8_t mmc4_chr_bank1_fe;
    uint8_t mmc4_latch0;
    uint8_t mmc4_latch1;
    uint8_t mmc4_chr0; /* Selected 4KB bank for $0000-$0FFF */
    uint8_t mmc4_chr1; /* Selected 4KB bank for $1000-$1FFF */

    /* IRQ signaling */
    bool irq_pending;       /* Set by mapper, cleared by NES system */
    struct NES *nes;

    /* PPU fetch context — set by NES system before each ppu_read call.
     * Mappers that need to distinguish BG from sprite fetches (e.g., MMC5)
     * can check this to switch CHR register sets. */
    uint16_t ppu_dot;       /* Current PPU dot (0-340) */
} Mapper;

/* Mapper lifecycle */
void mapper_init(Mapper *m, uint8_t number,
                 uint8_t *prg_rom, uint32_t prg_size,
                 uint8_t *chr_rom, uint32_t chr_size,
                 uint8_t mirroring);
void mapper_reset(Mapper *m);

/* CPU/PPU access */
uint8_t mapper_cpu_read(Mapper *m, uint16_t addr);
void mapper_cpu_write(Mapper *m, uint16_t addr, uint8_t val);
uint8_t mapper_ppu_read(Mapper *m, uint16_t addr);
void mapper_ppu_write(Mapper *m, uint16_t addr, uint8_t val);
void mapper_ppu_address(Mapper *m, uint16_t addr);
void mapper_ppu_bus_read(Mapper *m, uint16_t addr);
void mapper_cpu_clock(Mapper *m);

/* Scanline notification (called by PPU at end of each visible scanline) */
void mapper_notify_scanline(Mapper *m);

/* Helpers */
uint8_t mapper_get_mirroring(Mapper *m);
bool mapper_supported(uint8_t number);

#endif /* NES_MAPPER_H */
