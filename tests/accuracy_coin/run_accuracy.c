/* Headless runner for the bundled AccuracyCoin ROM. Results and menu names
 * come from the ROM's own table (AccuracyCoin.asm: TableTable at $8100).
 * Never infer success from rendered tiles or change the ROM's test state. */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "nes/rom.h"
#include "nes/nes.h"

#define PAGE_COUNT 22
#define MAX_TESTS 160
#define FRAME_LIMIT 18000

static NES nes;
static ROM rom;

typedef struct {
    char name[64];
    unsigned page;
    uint16_t result;
} AccuracyTest;
static AccuracyTest tests[MAX_TESTS];
static unsigned test_count;

static bool number(const char *text, unsigned max, unsigned *value) {
    char *end;
    errno = 0;
    unsigned long n = strtoul(text, &end, 10);
    if (errno || !*text || *end || n > max) return false;
    *value = (unsigned)n;
    return true;
}

static bool read_word(unsigned *pos, unsigned *value) {
    if (*pos < 0x8000 || *pos > 0xFFFE) return false;
    unsigned offset = *pos - 0x8000;
    *value = rom.prg_rom[offset] | (rom.prg_rom[offset + 1] << 8);
    *pos += 2;
    return true;
}

static bool read_name(unsigned *pos, char *name, size_t size) {
    size_t len = 0;
    while (*pos >= 0x8000 && *pos <= 0xFFFF) {
        unsigned c = rom.prg_rom[(*pos)++ - 0x8000];
        if (c == 0xFF) { name[len] = '\0'; return true; }
        if (c < 32 || c > 126 || len + 1 >= size) return false;
        name[len++] = (char)c;
    }
    return false;
}

static bool read_tests(void) {
    if (rom.mapper != 0 || rom.prg_size != 32768) return false;
    unsigned table = 0x8100;
    for (unsigned page = 1; page <= PAGE_COUNT; page++) {
        unsigned pos;
        char name[64];
        if (!read_word(&table, &pos) || !read_name(&pos, name, sizeof(name))) return false;
        for (;;) {
            unsigned result, entry;
            if (!read_name(&pos, name, sizeof(name))) return false;
            if (!name[0]) break;
            if (!read_word(&pos, &result) || !read_word(&pos, &entry)) return false;
            /* DRAW entries are informational, excluded by the ROM's Start mode. */
            if (result == 0x3FF || result == 0x360) continue;
            if (result < 0x400 || result > 0x4FF || test_count == MAX_TESTS) return false;
            tests[test_count].page = page;
            tests[test_count].result = (uint16_t)result;
            snprintf(tests[test_count].name, sizeof(tests[test_count].name), "%s", name);
            test_count++;
        }
    }
    return test_count > 0;
}

static void frames(unsigned count) {
    while (count--) nes_run_frame(&nes);
}

static void press(uint8_t button) {
    nes_set_controller(&nes, 0, button);
    frames(2);
    nes_set_controller(&nes, 0, 0);
}

int main(int argc, char **argv) {
    unsigned page = 0, phase = 12, align = 3;
    const char *path = argc > 1 ? argv[1] : "tests/accuracy_coin/AccuracyCoin.nes";
    if (argc > 3 || (argc > 2 && (!number(argv[2], PAGE_COUNT, &page) || !page))) {
        fprintf(stderr, "Usage: %s [AccuracyCoin.nes [page 1-22]]\n", argv[0]);
        return 2;
    }
    const char *env = getenv("NES_CPU_PHASE");
    if (env) {
        if (!number(env, 11, &phase)) { fprintf(stderr, "Invalid NES_CPU_PHASE\n"); return 2; }
    } else if ((env = getenv("NES_ALIGN"))) {
        if (!number(env, 2, &align)) { fprintf(stderr, "Invalid NES_ALIGN\n"); return 2; }
    }
    int err = nes_rom_load(&rom, path);
    if (err != ROM_OK) {
        fprintf(stderr, "Cannot load %s: %s\n", path, nes_rom_error_str(err));
        return 1;
    }
    if (!read_tests()) {
        fprintf(stderr, "Unsupported AccuracyCoin menu table\n");
        nes_rom_free(&rom);
        return 1;
    }
    nes_init(&nes);
    if (phase < 12) nes_set_cpu_phase_offset(&nes, (uint8_t)phase);
    else if (align < 3) nes_set_cpu_align(&nes, (uint8_t)align);
    phase = nes.cpu_phase_offset;
    nes_load_mapper(&nes, rom.mapper, rom.prg_rom, rom.prg_size,
                    rom.chr_rom, rom.chr_size, rom.mirroring);
    nes_reset(&nes);
    frames(90);
    for (unsigned p = 1; p < page; p++) { press(BTN_RIGHT); frames(30); }
    printf("AccuracyCoin: %s, CPU phase %u\n", page ? "selected page" : "all tests", phase);
    fflush(stdout);
    press(page ? BTN_A : BTN_START);

    /* The ROM uses $34/$35 for page/all-suite execution. Require observing
     * the run begin as well as end; a failed button press is not a pass. */
    unsigned flag = page ? 0x34 : 0x35;
    bool started = nes.ram[flag] != 0, complete = false;
    unsigned elapsed;
    for (elapsed = 0; elapsed < FRAME_LIMIT; elapsed++) {
        nes_run_frame(&nes);
        if (nes.ram[flag]) started = true;
        else if (started) {
            bool finished = true;
            for (unsigned i = 0; i < test_count; i++) {
                if (page && tests[i].page != page) continue;
                unsigned result = nes.ram[tests[i].result] & 3;
                if (result != 1 && result != 2) finished = false;
            }
            if (finished) { complete = true; break; }
        }
    }
    unsigned passed = 0, failed = 0, pending = 0;
    for (unsigned i = 0; i < test_count; i++) {
        const AccuracyTest *test = &tests[i];
        if (page && test->page != page) continue;
        unsigned result = nes.ram[test->result];
        const char *status;
        if (result != 0xFF && (result & 3) == 1) { passed++; status = "PASS"; }
        else if ((result & 3) == 2) { failed++; status = "FAIL"; }
        else { pending++; status = "NOT RUN"; }
        printf("  %-7s page %2u $%04X %-30s code %u\n",
               status, test->page, test->result, test->name, result >> 2);
    }
    printf("Summary: %u PASS, %u FAIL, %u NOT RUN (%u frames)\n", passed, failed, pending, elapsed);
    if (!complete) fprintf(stderr, "Timed out: PC=$%04X, test result address=$%02X%02X\n",
                           nes.cpu.PC, nes.ram[0x1F], nes.ram[0x1E]);
    nes_rom_free(&rom);
    return complete && passed && !failed && !pending ? 0 : 1;
}
