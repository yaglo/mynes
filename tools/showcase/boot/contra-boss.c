/* Reach the Contra waterfall boss the way the earlier fixture did (boot,
 * Start, poke the level and routine bytes, force the player through the
 * level until the scroll stops for the boss), then write a save state the
 * GPU frontend can load. */
#include <stdio.h>
#include <stdlib.h>
#include "nes/rom.h"
#include "nes/nes.h"
#include "nes/state.h"
static NES nes;
int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: contra-state ROM OUT.s1 [settle frames]\n"); return 1; }
    ROM rom; if (nes_rom_load(&rom, argv[1]) != ROM_OK) return 1;
    nes_init(&nes);
    nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size, rom.chr_rom, rom.chr_size, rom.mirroring);
    nes_reset(&nes);
    for (int i = 0; i < 400; i++) nes_run_frame(&nes);
    nes_set_controller(&nes, 0, 8); for (int i = 0; i < 2; i++) nes_run_frame(&nes); nes_set_controller(&nes, 0, 0);
    for (int i = 0; i < 400; i++) nes_run_frame(&nes);
    nes.ram[0x30] = 2; nes.ram[0x18] = 5; nes.ram[0x2c] = 0; nes.ram[0x32] = 30;
    for (int i = 0; i < 500; i++) nes_run_frame(&nes);
    int reached = -1;
    for (int i = 0; i < 2200; i++) {
        nes.ram[0xae] = 30; nes.ram[0x90] = 1;
        if (nes.ram[0x58] != 255) { nes.ram[0x31a] = 0x48; nes.ram[0xc6] = 0xfc; nes.ram[0xc4] = 0; nes.ram[0xa0] = 1; }
        else { nes.ram[0x31a] = 0xd0; nes.ram[0xc6] = 0; if (reached < 0) reached = i; }
        nes_run_frame(&nes);
        if (reached >= 0 && i - reached >= (argc > 3 ? atoi(argv[3]) : 90)) break;
    }
    printf("scroll stop %d (boss reached at step %d), game %d level %d routine %d screen %d\n",
           nes.ram[0x58], reached, nes.ram[0x18], nes.ram[0x30], nes.ram[0x2c], nes.ram[0x64]);
    nes.ram[0xae] = 255; nes.ram[0xb0] = 255;  /* invincible for the first four seconds of the clip */
    size_t n = nes_state_size(&nes); void *buf = malloc(n);
    if (!nes_state_save(&nes, buf, n)) { fprintf(stderr, "state save failed\n"); return 1; }
    FILE *f = fopen(argv[2], "wb"); fwrite(buf, 1, n, f); fclose(f);
    printf("wrote %zu bytes to %s\n", n, argv[2]);
    return 0;
}
