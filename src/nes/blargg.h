/*
 * Blargg Test ROM Auto-Detection
 *
 * Blargg test ROMs write a signature to $6000-$6003 to signal test status:
 *   $6000: status (0x80 = running, 0x00 = passed, other = fail code)
 *   $6001-$6003: magic bytes $DE, $B0, $61
 *   $6004+: null-terminated result text
 */

#ifndef NES_BLARGG_H
#define NES_BLARGG_H

#include <stdint.h>

struct NES;

typedef enum {
    BLARGG_NOT_DETECTED = 0,
    BLARGG_RUNNING,
    BLARGG_PASSED,
    BLARGG_FAILED
} BlarggStatus;

/* Check Blargg test status by reading PRG RAM at $6000-$6003.
 * Returns NOT_DETECTED if the magic signature isn't present. */
static inline BlarggStatus blargg_check(const struct NES *nes) {
    /* PRG RAM is at $6000-$7FFF, mapped via mapper or nes->prg_ram */
    const uint8_t *ram;
    if (nes->mapper_loaded)
        ram = nes->mapper.prg_ram;
    else
        ram = nes->prg_ram;

    /* Check magic signature at $6001-$6003 */
    if (ram[0x0001] != 0xDE || ram[0x0002] != 0xB0 || ram[0x0003] != 0x61)
        return BLARGG_NOT_DETECTED;

    uint8_t status = ram[0x0000];
    if (status == 0x80) return BLARGG_RUNNING;
    if (status == 0x00) return BLARGG_PASSED;
    return BLARGG_FAILED;
}

/* Get the Blargg result code (only meaningful when status is FAILED) */
static inline uint8_t blargg_get_code(const struct NES *nes) {
    if (nes->mapper_loaded)
        return nes->mapper.prg_ram[0x0000];
    return nes->prg_ram[0x0000];
}

/* Read the Blargg result text at $6004+.
 * Copies up to max_len-1 chars into buf, null-terminates.
 * Returns number of chars copied. */
static inline int blargg_get_text(const struct NES *nes, char *buf, int max_len) {
    const uint8_t *ram;
    if (nes->mapper_loaded)
        ram = nes->mapper.prg_ram;
    else
        ram = nes->prg_ram;

    int i;
    for (i = 0; i < max_len - 1; i++) {
        uint8_t c = ram[0x0004 + i];
        if (c == 0) break;
        buf[i] = (char)c;
    }
    buf[i] = '\0';
    return i;
}

/* Status as string */
static inline const char *blargg_status_str(BlarggStatus s) {
    switch (s) {
        case BLARGG_NOT_DETECTED: return "not detected";
        case BLARGG_RUNNING:      return "running";
        case BLARGG_PASSED:       return "PASSED";
        case BLARGG_FAILED:       return "FAILED";
    }
    return "unknown";
}

#endif /* NES_BLARGG_H */
