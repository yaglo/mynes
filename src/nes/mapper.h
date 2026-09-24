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

/* Sunsoft FME-7 (mapper 69) registers */
typedef struct {
    uint8_t command;        /* $8000: selected register (0-15) */
    uint8_t regs[16];       /* Internal registers */
    uint16_t irq_counter;   /* 16-bit IRQ counter, decremented every CPU cycle */
    bool irq_enabled;       /* IRQ fires when counter wraps */
    bool irq_counting;      /* Counter is actively decrementing */
} MapperFME7;

/* MMC5 (mapper 5) registers and ExRAM */
typedef struct {
    /* PRG banking */
    uint8_t prg_mode;           /* $5100: 0-3 */
    uint8_t prg_regs[5];        /* $5113-$5117 */

    /* CHR banking */
    uint8_t chr_mode;           /* $5101: 0-3 */
    uint16_t chr_regs[12];      /* $5120-$512B: effective bank (upper|low) */
    uint8_t chr_upper;          /* $5130: upper 2 bits for CHR bank numbers */
    bool chr_hi_written;        /* last CHR write was to B set ($5128-$512B) */

    /* Nametable / fill */
    uint8_t nt_mapping;         /* $5105 raw value */
    uint8_t fill_tile;          /* $5106 */
    uint8_t fill_attr;          /* $5107 (2 bits) */

    /* ExRAM */
    uint8_t exram_mode;         /* $5104: 0-3 */
    uint8_t exram[0x400];       /* 1KB */

    /* Scanline IRQ */
    uint8_t irq_target;         /* $5203 */
    bool irq_enabled;           /* $5204 bit 7 */
    uint8_t scanline_counter;
    bool in_frame;
    bool irq_status;
    uint16_t last_ppu_read;
    uint8_t repeated_reads;
    uint8_t idle_cpu_cycles;
    bool ppu_read_since_clock;

    /* Multiplier */
    uint8_t multiplicand;       /* $5205 */
    uint8_t multiplier;         /* $5206 */
} MapperMMC5;

typedef struct Mapper {
    uint16_t number;        /* Mapper number (NES 2.0 goes up to 4095) */

    /* PRG ROM banking */
    uint8_t *prg_rom;
    uint32_t prg_rom_size;
    uint16_t prg_banks;     /* Number of 16KB PRG banks */
    uint8_t prg_bank0;      /* Bank at $8000-$BFFF */
    uint8_t prg_bank1;      /* Bank at $C000-$FFFF */
    uint8_t prg_mode;       /* PRG banking mode */

    /* CHR ROM/RAM banking */
    uint8_t *chr_rom;
    uint32_t chr_rom_size;
    uint16_t chr_banks;     /* Number of 8KB CHR banks (or 4KB for some mappers) */
    uint8_t chr_bank0;      /* Bank at $0000-$0FFF (or $0000-$1FFF) */
    uint8_t chr_bank1;      /* Bank at $1000-$1FFF */
    uint8_t chr_ram[0x2000]; /* CHR RAM for mappers that use it */
    bool has_chr_ram;

    /* PRG RAM */
    uint8_t prg_ram[0x2000];
    bool prg_ram_enabled;
    bool prg_ram_write_protect;

    /* Mirroring */
    uint8_t mirroring;      /* 0=horizontal, 1=vertical, 2=single-screen low, 3=single-screen high,
                               4=four-screen */

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

    /* Registers of mappers too large to keep as flat fields above. Only the
     * member for this cartridge's mapper is live. */
    union {
        MapperFME7 fme7;
        MapperMMC5 mmc5;
    } ext;

    /* IRQ signaling */
    bool irq_pending;       /* Set by mapper, cleared by NES system */
    struct NES *nes;

    /* PPU fetch context — set by NES system before each ppu_read call.
     * Mappers that need to distinguish BG from sprite fetches (e.g., MMC5)
     * can check this to switch CHR register sets. */
    uint16_t ppu_dot;       /* Current PPU dot (0-340) */
} Mapper;

/* Mapper lifecycle */
void mapper_init(Mapper *m, uint16_t number,
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

/* Side-effect-free reads for debuggers and tracers: the value a read would
 * return, without acknowledging IRQs or flipping CHR latches. */
uint8_t mapper_cpu_peek(Mapper *m, uint16_t addr);
uint8_t mapper_ppu_peek(Mapper *m, uint16_t addr);

/* Scanline notification (called by PPU at end of each visible scanline) */
void mapper_notify_scanline(Mapper *m);

/* Helpers */
uint8_t mapper_get_mirroring(Mapper *m);
bool mapper_supported(uint16_t number);

#endif /* NES_MAPPER_H */
