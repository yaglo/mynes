/*
 * NES Debug API - Side-effect-free state inspection
 *
 * All functions are safe to call during emulation without affecting state.
 * Snapshot structs capture component state at a point in time.
 */

#ifndef NES_DEBUG_H
#define NES_DEBUG_H

#include <stdint.h>
#include <stdbool.h>

/* Forward declarations (actual types defined in component headers) */
struct NES;
struct CPU;
struct PPU;
struct APU;
struct Mapper;

/* ============================================================================
 * State Snapshots
 * ============================================================================ */

typedef struct {
    uint16_t PC;
    uint8_t A, X, Y, SP, P;
    uint8_t IR;             /* Current instruction register */
    uint16_t uPC;           /* Microcode position (0 = instruction boundary) */
    uint64_t cycles;
    bool irq_pending;
    bool nmi_pending;
    bool reset_pending;
} CPUSnapshot;

typedef struct {
    int scanline;
    int dot;
    uint16_t v;             /* Current VRAM address */
    uint16_t t;             /* Temporary VRAM address */
    uint8_t fine_x;
    bool w;                 /* Write toggle */
    uint8_t ctrl;           /* $2000 */
    uint8_t mask;           /* $2001 */
    uint8_t status;         /* $2002 */
    uint8_t oam_addr;
    bool in_vblank;
    bool sprite0_hit;
    bool sprite_overflow;
    uint64_t frame_count;
} PPUSnapshot;

typedef struct {
    int timer;
    int length_counter;
    bool enabled;
    int volume;             /* Effective volume (envelope or constant) */
} APUChannelSnapshot;

typedef struct {
    int frame_counter_cycle;
    bool frame_irq_flag;
    bool frame_irq_inhibit;
    bool five_step_mode;
    bool dmc_irq_flag;
    APUChannelSnapshot pulse1;
    APUChannelSnapshot pulse2;
    APUChannelSnapshot triangle;
    APUChannelSnapshot noise;
    struct {
        bool enabled;
        int bytes_remaining;
        uint8_t output_level;
    } dmc;
} APUSnapshot;

/* ============================================================================
 * Snapshot Functions
 * ============================================================================ */

void debug_get_cpu_snapshot(const struct NES *nes, CPUSnapshot *out);
void debug_get_ppu_snapshot(const struct NES *nes, PPUSnapshot *out);
void debug_get_apu_snapshot(const struct NES *nes, APUSnapshot *out);

/* ============================================================================
 * Side-Effect-Free Memory Reads
 *
 * These read memory without triggering register side effects.
 * I/O registers ($2000-$401F) return 0 instead of performing reads.
 * ============================================================================ */

uint8_t debug_read_cpu(const struct NES *nes, uint16_t addr);
uint16_t debug_read_cpu_word(const struct NES *nes, uint16_t addr);
uint8_t debug_read_ppu_vram(const struct NES *nes, uint16_t addr);
uint8_t debug_read_oam(const struct NES *nes, uint8_t index);

/* ============================================================================
 * Formatting Helpers
 * ============================================================================ */

/* Returns static string like "NV-BDIZC" with set flags uppercase, clear lowercase */
const char *debug_format_cpu_flags(uint8_t p);

#endif /* NES_DEBUG_H */
