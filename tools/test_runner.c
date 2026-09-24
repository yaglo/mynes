/*
 * NES Test Runner
 *
 * Headless test execution with Blargg auto-detection, scripting,
 * screen dumps, traces, and CI exit codes.
 *
 * Usage:
 *   test_runner <rom> [options]
 *     --blargg              Enable Blargg test auto-detection
 *     --script <file>       Execute script file
 *     --frames <n>          Max frames (default: 18000 = 5 min)
 *     --trace <categories>  Enable trace categories (cpu,ppu,apu,mem,all)
 *     --trace-mem <S>-<E>   Filter memory traces to address range (hex)
 *     --charmap <file>      Load tile->ASCII character map
 *     --dump-on-exit        Dump screen as text on completion
 *     --screenshot <file>   Save screenshot on completion
 *     --verbose             Verbose output
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "nes/rom.h"
#include "nes/nes.h"
#include "nes/debug.h"
#include "nes/hooks.h"
#include "nes/blargg.h"
#include "nes/trace.h"
#include "nes/cpu_trace.h"
#include "nes/screen_dump.h"
#include "nes/screenshot.h"
#include "nes/script.h"
#include "nes/exit_codes.h"
#include "nes/debug_chr.h"

/* ============================================================================
 * Emulator State
 * ============================================================================ */

static NES nes;
static ROM rom;

/* ============================================================================
 * Options
 * ============================================================================ */

static struct {
    const char *rom_path;
    const char *script_path;
    const char *screenshot_path;
    const char *charmap_path;
    const char *trace_cats;
    const char *trace_mem_range;
    const char *chr_dump_path;
    int max_frames;
    int screenshot_scanline;    /* -1 = disabled */
    bool blargg;
    bool dump_on_exit;
    bool verbose;
} opts = {
    .max_frames = 18000,  /* 5 minutes at 60fps */
    .screenshot_scanline = -1,
};

/* ============================================================================
 * Runtime State
 * ============================================================================ */

static bool running = true;
static int exit_code = NES_EXIT_TIMEOUT;
static BlarggStatus blargg_status = BLARGG_NOT_DETECTED;
static CharMap charmap;
static uint64_t frame_count = 0;

/* Events that scripts can wait for */
static bool event_blargg_done = false;
static bool scanline_screenshot_taken = false;

/* ============================================================================
 * Scanline Hook — mid-frame screenshot capture
 * ============================================================================ */

static void on_scanline(int scanline, uint64_t frame) {
    (void)frame;
    if (!scanline_screenshot_taken && opts.screenshot_scanline >= 0 &&
        scanline == opts.screenshot_scanline && opts.screenshot_path) {
        screenshot_save_ppm(&nes.ppu, opts.screenshot_path);
        scanline_screenshot_taken = true;
        if (opts.verbose)
            printf("Mid-frame screenshot at scanline %d: %s\n",
                   scanline, opts.screenshot_path);
    }
}

/* ============================================================================
 * Frame Hook — Blargg detection + event signaling
 * ============================================================================ */

static void on_frame(uint64_t frame, uint64_t cycles) {
    (void)cycles;
    frame_count = frame;

    if (opts.blargg) {
        BlarggStatus new_status = blargg_check(&nes);
        if (new_status != blargg_status) {
            if (opts.verbose && new_status != BLARGG_NOT_DETECTED) {
                printf("[Blargg] Status: %s", blargg_status_str(new_status));
                if (new_status == BLARGG_FAILED) {
                    printf(" (code %d)", blargg_get_code(&nes));
                }
                printf("\n");
            }
            blargg_status = new_status;
        }

        if (blargg_status == BLARGG_PASSED) {
            exit_code = NES_EXIT_PASS;
            event_blargg_done = true;
            if (!opts.script_path)
                running = false;
        }
        else if (blargg_status == BLARGG_FAILED) {
            exit_code = NES_EXIT_FAIL;
            event_blargg_done = true;
            if (!opts.script_path)
                running = false;
        }
    }
}

/* ============================================================================
 * Script Execution
 * ============================================================================ */

static void execute_script(Script *script) {
    for (int i = 0; i < script->count && running; i++) {
        ScriptCommand *cmd = &script->commands[i];

        switch (cmd->type) {
        case SCRIPT_CMD_WAIT_FRAMES:
            if (opts.verbose)
                printf("[Script] WAIT_FRAMES %d\n", cmd->int_arg);
            for (int f = 0; f < cmd->int_arg && running; f++)
                nes_run_frame(&nes);
            break;

        case SCRIPT_CMD_PRESS_BUTTON:
            if (opts.verbose)
                printf("[Script] PRESS_BUTTON 0x%02X for %d frames\n",
                       cmd->int_arg, cmd->int_arg2);
            nes_set_controller(&nes, 0, (uint8_t)cmd->int_arg);
            for (int f = 0; f < cmd->int_arg2 && running; f++)
                nes_run_frame(&nes);
            nes_set_controller(&nes, 0, 0);
            break;

        case SCRIPT_CMD_WAIT_EVENT:
            if (opts.verbose)
                printf("[Script] WAIT_EVENT %s\n", cmd->str_arg);
            if (strcmp(cmd->str_arg, "blargg_done") == 0) {
                while (!event_blargg_done && running) {
                    nes_run_frame(&nes);
                    if (frame_count >= (uint64_t)opts.max_frames) {
                        running = false;
                        exit_code = NES_EXIT_TIMEOUT;
                    }
                }
            }
            break;

        case SCRIPT_CMD_DUMP_SCREEN:
            if (opts.verbose)
                printf("[Script] DUMP_SCREEN\n");
            if (cmd->str_arg[0]) {
                FILE *fp = fopen(cmd->str_arg, "w");
                if (fp) {
                    screen_dump_to_file(&nes.ppu, &charmap, fp);
                    fclose(fp);
                }
            } else {
                screen_dump_print(&nes.ppu, &charmap);
            }
            break;

        case SCRIPT_CMD_SCREENSHOT:
            if (opts.verbose)
                printf("[Script] SCREENSHOT %s\n", cmd->str_arg);
            {
                const char *path = cmd->str_arg[0] ? cmd->str_arg : "screenshot.ppm";
                screenshot_save_ppm(&nes.ppu, path);
            }
            break;

        case SCRIPT_CMD_STOP:
            if (opts.verbose)
                printf("[Script] STOP\n");
            running = false;
            break;
        }
    }
}

/* ============================================================================
 * Argument Parsing
 * ============================================================================ */

static void print_usage(const char *prog) {
    printf("NES Test Runner\n");
    printf("Usage: %s <rom> [options]\n\n", prog);
    printf("Options:\n");
    printf("  --blargg              Enable Blargg test auto-detection\n");
    printf("  --script <file>       Execute script file\n");
    printf("  --frames <n>          Max frames (default: 18000 = 5 min)\n");
    printf("  --trace <categories>  Enable traces (cpu,ppu,apu,mem,irq,all)\n");
    printf("  --trace-mem <S>-<E>   Memory trace address range (hex)\n");
    printf("  --charmap <file>      Tile-to-ASCII character map\n");
    printf("  --dump-on-exit        Dump screen text on completion\n");
    printf("  --screenshot <file>   Save screenshot on completion\n");
    printf("  --verbose             Verbose output\n");
    printf("\nExit codes: 0=pass, 1=fail, 2=timeout, 3=ROM error\n");
}

static bool parse_args(int argc, char *argv[]) {
    if (argc < 2) {
        print_usage(argv[0]);
        return false;
    }

    opts.rom_path = argv[1];

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--blargg") == 0) {
            opts.blargg = true;
        } else if (strcmp(argv[i], "--script") == 0 && i + 1 < argc) {
            opts.script_path = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            opts.max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--trace") == 0 && i + 1 < argc) {
            opts.trace_cats = argv[++i];
        } else if (strcmp(argv[i], "--trace-mem") == 0 && i + 1 < argc) {
            opts.trace_mem_range = argv[++i];
        } else if (strcmp(argv[i], "--charmap") == 0 && i + 1 < argc) {
            opts.charmap_path = argv[++i];
        } else if (strcmp(argv[i], "--dump-on-exit") == 0) {
            opts.dump_on_exit = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            opts.screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--screenshot-scanline") == 0 && i + 1 < argc) {
            opts.screenshot_scanline = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--chr-dump") == 0 && i + 1 < argc) {
            opts.chr_dump_path = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
            opts.verbose = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }

    return true;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    if (!parse_args(argc, argv))
        return NES_EXIT_ROM_ERR;

    /* Load ROM */
    int err = nes_rom_load(&rom, opts.rom_path);
    if (err != ROM_OK) {
        fprintf(stderr, "Error loading ROM: %s\n", nes_rom_error_str(err));
        return NES_EXIT_ROM_ERR;
    }

    if (opts.verbose)
        nes_rom_print_info(&rom);

    /* Initialize character map */
    if (opts.charmap_path) {
        if (!charmap_load(&charmap, opts.charmap_path)) {
            fprintf(stderr, "Warning: could not load charmap %s, using default\n",
                    opts.charmap_path);
            charmap_set_default(&charmap);
        }
    } else {
        charmap_set_default(&charmap);
    }

    /* Initialize NES */
    hooks_init();
    nes_init(&nes);
    nes_load_mapper(&nes, rom.mapper,
                    rom.prg_rom, rom.prg_size,
                    rom.chr_rom, rom.chr_size,
                    rom.mirroring);
    nes_rom_apply_trainer(&rom, &nes.mapper);
    nes_reset(&nes);

    /* Set up hooks */
    debug_hooks.on_frame = on_frame;
    if (opts.screenshot_scanline >= 0)
        debug_hooks.on_ppu_scanline = on_scanline;

    /* Set up traces */
    if (opts.trace_cats) {
        trace_init();
        trace_set_categories(trace_parse_categories(opts.trace_cats));
        if (opts.trace_mem_range) {
            unsigned int start, end;
            if (sscanf(opts.trace_mem_range, "%x-%x", &start, &end) == 2) {
                trace_set_mem_range((uint16_t)start, (uint16_t)end);
            }
        }
        cpu_trace_set_nes((struct NES *)&nes);  /* For TRACE_CPU_DETAIL */
        trace_install_hooks();
        /* Restore our frame hook (trace may have overwritten it) */
        debug_hooks.on_frame = on_frame;
    }

    /* Execute script or run frames */
    if (opts.script_path) {
        Script script;
        if (!script_load(&script, opts.script_path)) {
            fprintf(stderr, "Error loading script: %s\n", opts.script_path);
            nes_rom_free(&rom);
            return NES_EXIT_ROM_ERR;
        }
        if (opts.verbose)
            printf("Loaded script with %d commands\n", script.count);
        execute_script(&script);
    } else {
        /* Default: run until blargg result or max frames */
        for (int frame = 0; frame < opts.max_frames && running; frame++) {
            nes_run_frame(&nes);
        }
    }

    /* Output results */
    if (opts.blargg && blargg_status != BLARGG_NOT_DETECTED) {
        char text[256];
        blargg_get_text(&nes, text, sizeof(text));
        if (text[0]) {
            printf("%s\n", text);
        }
        printf("Result: %s\n", blargg_status_str(blargg_status));
    }

    if (opts.dump_on_exit) {
        printf("\n--- Screen Dump ---\n");
        screen_dump_print(&nes.ppu, &charmap);
    }

    if (opts.screenshot_path && !scanline_screenshot_taken) {
        screenshot_save_ppm(&nes.ppu, opts.screenshot_path);
        if (opts.verbose)
            printf("Screenshot saved: %s\n", opts.screenshot_path);
    }

    if (opts.chr_dump_path) {
        debug_chr_save_ppm(&nes, opts.chr_dump_path);
        if (opts.verbose)
            printf("CHR dump saved: %s\n", opts.chr_dump_path);
    }

    if (opts.verbose) {
        CPUSnapshot snap;
        debug_get_cpu_snapshot(&nes, &snap);
        printf("CPU: PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X P=%s\n",
               snap.PC, snap.A, snap.X, snap.Y, snap.SP,
               debug_format_cpu_flags(snap.P));
    }

    /* Cleanup */
    if (opts.trace_cats)
        trace_remove_hooks();
    nes_rom_free(&rom);

    return exit_code;
}
