/* Auto-repeat of a held gamepad direction in the OSD menu and the ROM
 * browser. Held arrow keys already repeat through SDL key events; a pad
 * sends one event per press or stick deflection. macOS repeats keys after
 * 500 ms and then every 83 ms by default; the pad repeats a little sooner. */
#ifndef NAV_REPEAT_H
#define NAV_REPEAT_H
#include <stdint.h>

#define NAV_REPEAT_DELAY_MS    400
#define NAV_REPEAT_INTERVAL_MS 80

typedef struct {
    uint8_t  direction;  /* controller bit 0x10 up .. 0x80 right; 0 when idle */
    uint64_t next_ms;
} NavRepeat;

/* Call when a press has navigated once. The latest press is the one that
 * repeats, as with two arrow keys held. */
static inline void nav_repeat_press(NavRepeat *r, uint8_t direction, uint64_t now_ms) {
    r->direction = direction;
    r->next_ms = now_ms + NAV_REPEAT_DELAY_MS;
}

/* The direction to navigate again now, or 0. `held` holds the direction bits
 * still down; releasing the repeating one ends the repeat even while another
 * is held. After a stall one repeat is due, not one per missed interval. */
static inline uint8_t nav_repeat_poll(NavRepeat *r, uint8_t held, uint64_t now_ms) {
    if (!(held & r->direction)) {
        r->direction = 0;
        return 0;
    }
    if (now_ms < r->next_ms) return 0;
    r->next_ms += NAV_REPEAT_INTERVAL_MS;
    if (r->next_ms <= now_ms) r->next_ms = now_ms + NAV_REPEAT_INTERVAL_MS;
    return r->direction;
}

#endif
