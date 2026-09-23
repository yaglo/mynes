/* Gamepad direction auto-repeat in the menu and the ROM browser. */
#include "nav_repeat.h"
#include <stdio.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while(0)

enum { UP = 0x10, DOWN = 0x20, LEFT = 0x40, RIGHT = 0x80 };

/* Polls once per 60 Hz loop iteration, as main.c does with a menu open,
 * and returns how many repeats a hold from `from` to `to` produced. */
static int repeats_between(NavRepeat *r, uint8_t held, uint64_t from, uint64_t to, uint64_t *first) {
    int count = 0;
    *first = 0;
    for (uint64_t frame = 0; from + frame * 50 / 3 < to; frame++) {
        uint64_t now = from + frame * 50 / 3;
        if (nav_repeat_poll(r, held, now)) {
            if (!count) *first = now;
            count++;
        }
    }
    return count;
}

int main(void) {
    NavRepeat r = {0};
    uint64_t first;
    /* Nothing pressed, nothing repeats. */
    CHECK(nav_repeat_poll(&r, DOWN, 1000) == 0);
    /* Held down for one second: the first repeat at 400 ms, then every
     * 80 ms, 400 + 7 x 80 = 960 ms, so eight repeats. */
    nav_repeat_press(&r, DOWN, 1000);
    CHECK(nav_repeat_poll(&r, DOWN, 1399) == 0);
    CHECK(repeats_between(&r, DOWN, 1000, 2000, &first) == 8);
    CHECK(first >= 1400 && first < 1417);
    /* Released: the repeat stops and holding again without a press (the
     * menu opened over a held direction) does not start it. */
    CHECK(nav_repeat_poll(&r, 0, 2000) == 0);
    CHECK(r.direction == 0);
    CHECK(repeats_between(&r, DOWN, 2000, 3000, &first) == 0);
    /* Exact timing without frame quantization. */
    nav_repeat_press(&r, UP, 5000);
    CHECK(nav_repeat_poll(&r, UP, 5400) == UP);
    CHECK(nav_repeat_poll(&r, UP, 5479) == 0);
    CHECK(nav_repeat_poll(&r, UP, 5480) == UP);
    /* A stall of a second gives one repeat, then the interval restarts. */
    CHECK(nav_repeat_poll(&r, UP, 6500) == UP);
    CHECK(nav_repeat_poll(&r, UP, 6501) == 0);
    CHECK(nav_repeat_poll(&r, UP, 6580) == UP);
    /* Stick held diagonally: the later direction repeats; releasing it ends
     * the repeat although the other stays held. */
    nav_repeat_press(&r, UP, 8000);
    nav_repeat_press(&r, RIGHT, 8100);
    CHECK(nav_repeat_poll(&r, UP | RIGHT, 8450) == 0);
    CHECK(nav_repeat_poll(&r, UP | RIGHT, 8500) == RIGHT);
    CHECK(nav_repeat_poll(&r, UP, 8580) == 0);
    CHECK(repeats_between(&r, UP, 8580, 10000, &first) == 0);
    /* Changing direction restarts the delay. */
    nav_repeat_press(&r, LEFT, 11000);
    CHECK(nav_repeat_poll(&r, LEFT, 11300) == 0);
    nav_repeat_press(&r, RIGHT, 11300);
    CHECK(nav_repeat_poll(&r, RIGHT, 11400) == 0);
    CHECK(nav_repeat_poll(&r, RIGHT, 11700) == RIGHT);
    printf("Navigation repeat: %d failures\n", failures);
    return failures != 0;
}
