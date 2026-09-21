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
static double audio_energy;
static struct { unsigned long frame; unsigned mask; } inputs[1024];
static size_t input_count;

static void audio_sample(void *context, float sample) {
    (void)context;
    samples++;
    audio_sum += sample;
    audio_energy += (double)sample * sample;
}

static double seconds(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

int main(int argc, char **argv) {
    unsigned long frames = 3600;
    bool audio = true, start = false;
    int region = -1;
    const char *input_path = NULL;
    const char *frame_path = NULL;
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
        else if (!strcmp(argv[i], "--pal")) region = NES_REGION_PAL;
        else if (!strcmp(argv[i], "--ntsc")) region = NES_REGION_NTSC;
        else if (!strcmp(argv[i], "--input") && i + 1 < argc) input_path = argv[++i];
        else if (!strcmp(argv[i], "--frame-output") && i + 1 < argc) frame_path = argv[++i];
        else goto usage;
    }
    if (input_path) {
        FILE *file = fopen(input_path, "r");
        if (!file) { perror(input_path); return 1; }
        char line[128];
        while (fgets(line, sizeof(line), file)) {
            unsigned long frame;
            unsigned mask;
            char extra;
            if (sscanf(line, "%lu %x %c", &frame, &mask, &extra) != 2 ||
                frame >= frames || mask > 255 || input_count == 1024 ||
                (input_count && frame <= inputs[input_count - 1].frame)) {
                fprintf(stderr, "Invalid input replay: expected increasing frame and hex button mask\n");
                fclose(file);
                return 2;
            }
            inputs[input_count].frame = frame;
            inputs[input_count++].mask = mask;
        }
        if (ferror(file)) { perror(input_path); fclose(file); return 1; }
        fclose(file);
    }
    ROM rom;
    int error = nes_rom_load(&rom, argv[1]);
    if (error != ROM_OK) {
        fprintf(stderr, "%s: %s\n", argv[1], nes_rom_error_str(error));
        return 1;
    }
    nes_init(&nes);
    if (!mapper_supported(rom.mapper)) {
        fprintf(stderr, "Unsupported mapper: %u\n", rom.mapper);
        nes_rom_free(&rom);
        return 1;
    }
    nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size,
                    rom.chr_rom, rom.chr_size, rom.mirroring);
    if (rom.tv_system == NES_TV_PAL) nes_set_region(&nes, NES_REGION_PAL);
    if (region >= 0) nes_set_region(&nes, region);
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
    audio_energy = 0;
    size_t next_input = 0;
    double begin = seconds();
    for (unsigned long i = 0; i < frames; i++) {
        if (next_input < input_count && inputs[next_input].frame == i)
            nes_set_controller(&nes, 0, inputs[next_input++].mask);
        nes_run_frame(&nes);
    }
    double elapsed = seconds() - begin;
    if (frame_path) {
        FILE *file = fopen(frame_path, "wb");
        if (!file) { perror(frame_path); nes_rom_free(&rom); return 1; }
        bool ok = fprintf(file, "P6\n256 240\n255\n") > 0;
        ok &= fwrite(nes.ppu.framebuffer, 1, sizeof(nes.ppu.framebuffer), file) == sizeof(nes.ppu.framebuffer);
        if (fclose(file)) ok = false;
        if (!ok) { fprintf(stderr, "Failed to write frame\n"); nes_rom_free(&rom); return 1; }
    }
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < sizeof(nes.ppu.framebuffer); i++)
        hash = (hash ^ nes.ppu.framebuffer[i]) * 16777619u;
    for (size_t i = 0; i < sizeof(nes.ram); i++)
        hash = (hash ^ nes.ram[i]) * 16777619u;
    printf("frames=%lu seconds=%.6f ms/frame=%.6f fps=%.2f audio=%s "
           "samples=%" PRIu64 " audio_sum=%.9g audio_energy=%.9g checksum=%08" PRIx32
           " mapper=%u region=%s\n",
           frames, elapsed, elapsed * 1000 / frames, frames / elapsed,
           audio ? "on" : "off", samples, audio_sum, audio_energy, hash,
           rom.mapper, nes.ppu.region == NES_REGION_PAL ? "PAL" : "NTSC");
    nes_rom_free(&rom);
    return 0;
usage:
    fprintf(stderr, "Usage: %s ROM [frames [--no-audio] [--start] [--pal|--ntsc] [--input FILE] [--frame-output FILE]]\n", argv[0]);
    return 2;
}
