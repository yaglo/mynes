#include <stdio.h>
#include <stdlib.h>

#include <allegro5/allegro.h>

#include "fce.h"

int main(int argc, char *argv[])
{
    if (fce_load_rom(argv[1]) == -1) {
        exit(1);
    }

    fce_init();
    fce_run();

    return 0;
}
