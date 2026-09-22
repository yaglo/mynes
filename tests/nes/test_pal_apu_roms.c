#include <stdio.h>
#include "nes/nes.h"
#include "nes/rom.h"

static NES nes;

int main(void) {
    static const char *roms[] = {
        "01.len_ctr", "02.len_table", "03.irq_flag", "04.clock_jitter",
        "05.len_timing_mode0", "06.len_timing_mode1", "07.irq_flag_timing",
        "08.irq_timing", "10.len_halt_timing", "11.len_reload_timing"
    };
    int failures = 0;
    for (unsigned i = 0; i < sizeof(roms) / sizeof(roms[0]); i++) {
        char path[256];
        snprintf(path, sizeof(path), "tests/nes-test-roms/pal_apu_tests/%s.nes", roms[i]);
        ROM rom;
        if (nes_rom_load(&rom, path)) {
            fprintf(stderr, "Cannot load %s\n", path);
            return 1;
        }
        nes_init(&nes);
        nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size,
                        rom.chr_rom, rom.chr_size, rom.mirroring);
        nes_set_region(&nes, 1);
        nes_reset(&nes);
        /* These hardware-verified tests report 1 at $F8 on success. A fixed
         * run budget also catches a test that never reaches its result loop. */
        for (int frame = 0; frame < 300; frame++) nes_run_frame(&nes);
        unsigned result = nes.ram[0xf8];
        printf("%s: %s (result %u, PC %04x)\n", roms[i],
               result == 1 ? "PASS" : "FAIL", result, nes.cpu.PC);
        failures += result != 1;
        nes_rom_free(&rom);
    }
    return failures ? 1 : 0;
}
