#include "nes/hooks.h"
#include <string.h>

DebugHooks debug_hooks;

void hooks_init(void) {
    memset(&debug_hooks, 0, sizeof(debug_hooks));
}

void hooks_clear(void) {
    memset(&debug_hooks, 0, sizeof(debug_hooks));
}
