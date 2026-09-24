/* Find an input replay that keeps the Contra player alive through the
 * showcase clip from the boss state (contra-boss.c).
 *
 * The state is saved just after the forced walk ends, so the player falls
 * and dies at once; he respawns about 135 frames after the state loads,
 * and his respawn invincibility (ram 0xae) runs out about 250 frames after
 * it. Standing still, a fireball kills him again at 290. The clip starts
 * after a lead-in the recorder runs without input (--record-after, the
 * shot's record_after); clip frame 1 is the first recorded frame, as the
 * replay format counts it. From the first clip frame at or after 150
 * frames from the load, this tries random schedules of common actions
 * (fire, aim, run, jump, lie down), each held 12 to 47 frames, and writes
 * the first one in which the player stays alive (ram 0x90 != 2, lives at
 * 0x32 unchanged) to 10 frames past the end of the clip. The search is
 * deterministic.
 *
 * usage: contra-replay ROM STATE OUT.replay LEAD_IN CLIP_FRAMES [trials] */
#include <stdio.h>
#include <stdlib.h>
#include "nes/rom.h"
#include "nes/nes.h"
#include "nes/state.h"

enum { MAX_SEGMENTS = 40 };
/* B, Up+B, Right+B, Left+B, A+B, Right+A+B, Left+A+B, Down+B, Up+Right+B,
 * Up+Left+B, nothing, A */
static const unsigned char ACTIONS[] = {0x02, 0x12, 0x82, 0x42, 0x03, 0x83, 0x43, 0x22, 0x92, 0x52, 0x00, 0x01};
static NES nes;
static unsigned seed = 12345;
static unsigned next(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

int main(int argc, char **argv) {
    if (argc < 6) { fprintf(stderr, "usage: contra-replay ROM STATE OUT.replay LEAD_IN CLIP_FRAMES [trials]\n"); return 1; }
    const int LEAD_IN = atoi(argv[4]), END = atoi(argv[5]) + 10;
    const int START = LEAD_IN >= 150 ? 1 : 150 - LEAD_IN;
    ROM rom;
    if (nes_rom_load(&rom, argv[1]) != ROM_OK) return 1;
    nes_init(&nes);
    nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size, rom.chr_rom, rom.chr_size, rom.mirroring);
    nes_reset(&nes);
    FILE *f = fopen(argv[2], "rb");
    if (!f) return 1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) return 1;
    fclose(f);
    char error[200];
    if (!nes_state_load(&nes, buf, (size_t)n, error, sizeof error)) { fprintf(stderr, "%s\n", error); return 1; }
    for (int i = 0; i < LEAD_IN; i++) nes_run_frame(&nes);
    nes_set_controller(&nes, 0, 0x12);                   /* the replay's first row */
    for (int i = 1; i < START; i++) nes_run_frame(&nes);
    size_t size = nes_state_size(&nes);
    void *start = malloc(size);
    nes_state_save(&nes, start, size);
    const unsigned char lives = nes.ram[0x32];
    int trials = argc > 6 ? atoi(argv[6]) : 2000;
    for (int t = 0; t < trials; t++) {
        int at[MAX_SEGMENTS], count = 0;
        unsigned char mask[MAX_SEGMENTS];
        for (int frame = START; frame < END && count < MAX_SEGMENTS; frame += 12 + next() % 36) {
            at[count] = frame; mask[count] = ACTIONS[next() % sizeof ACTIONS]; count++;
        }
        nes_state_load(&nes, start, size, NULL, 0);
        int k = 0, alive = 1;
        for (int frame = START; frame <= END && alive; frame++) {
            if (k < count && at[k] == frame) nes_set_controller(&nes, 0, mask[k++]);
            nes_run_frame(&nes);
            alive = nes.ram[0x90] != 2 && nes.ram[0x32] == lives;
        }
        if (!alive) continue;
        FILE *out = fopen(argv[3], "w");
        if (!out) return 1;
        if (START > 1) fprintf(out, "1 12\n");
        for (int i = 0; i < count; i++) fprintf(out, "%d %x\n", at[i], mask[i]);
        fclose(out);
        printf("trial %d keeps the player alive to clip frame %d; wrote %s\n", t, END, argv[3]);
        return 0;
    }
    fprintf(stderr, "no schedule in %d trials\n", trials);
    return 1;
}
