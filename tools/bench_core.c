/* Unpaced core benchmark, including audio synthesis by default. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <inttypes.h>
#include <time.h>
#include "nes/rom.h"
#include "nes/nes.h"

static NES nes;
static uint64_t samples;
static double audio_sum;

static void audio_sample(void *context, float sample) {
    (void)context;
    samples++;
    audio_sum += sample;
}

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    unsigned long frames = 3600;
    bool audio = true, start = false;
    if (argc < 2) goto usage;
    if (argc >= 3) {
        char *end;
        errno = 0;
        frames = strtoul(argv[2], &end, 10);
        if (errno || !argv[2][0] || *end || !frames || frames > 10000000) goto usage;
    }
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--no-audio")) audio = false;
        else if (!strcmp(argv[i], "--start")) start = true;
        else goto usage;
    }
    ROM rom;
    int error = nes_rom_load(&rom, argv[1]);
    if (error != ROM_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], nes_rom_error_str(error));
        return 1;
    }
    nes_init(&nes);
    nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size,
                    rom.chr_rom, rom.chr_size, rom.mirroring);
    if (rom.tv_system == NES_TV_PAL) nes_set_region(&nes, NES_REGION_PAL);
    nes_reset(&nes);
    if (audio) apu_set_audio_callback(&nes.apu, audio_sample, NULL);
    for (unsigned i = 0; i < 120; i++) nes_run_frame(&nes);
    if (start) {
        nes_set_controller(&nes, 0, BTN_START);
        nes_run_frame(&nes);
        nes_run_frame(&nes);
        nes_set_controller(&nes, 0, 0);
    }
    samples = 0;
    audio_sum = 0;
    double begin = seconds();
    for (unsigned long i = 0; i < frames; i++) nes_run_frame(&nes);
    double elapsed = seconds() - begin;
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < sizeof(nes.ppu.framebuffer); i++)
        hash = (hash ^ nes.ppu.framebuffer[i]) * 16777619u;
    for (size_t i = 0; i < sizeof(nes.ram); i++)
        hash = (hash ^ nes.ram[i]) * 16777619u;
    printf("frames=%lu seconds=%.6f ms/frame=%.6f fps=%.2f audio=%s "
           "samples=%" PRIu64 " audio_sum=%.9g checksum=%08" PRIx32 "\n",
           frames, elapsed, elapsed * 1000 / frames, frames / elapsed,
           audio ? "on" : "off", samples, audio_sum, hash);
    nes_rom_free(&rom);
    return 0;
usage:
    fprintf(stderr, "Usage: %s ROM [frames [--no-audio] [--start]]\n", argv[0]);
    return 2;
}
