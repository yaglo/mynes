/* config.json defaults and the render scale setting against a throwaway
 * config directory. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Config FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static void write_config(const char *text) {
    char path[MYNES_PATH_MAX];
    mynes_config_path(path, sizeof(path));
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fputs(text, f); fclose(f); }
}

static bool config_contains(const char *needle) {
    char path[MYNES_PATH_MAX], text[4096];
    mynes_config_path(path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return strstr(text, needle) != NULL;
}

int main(void) {
    char root[] = "/tmp/mynes-config-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    setenv("XDG_CONFIG_HOME", root, 1);
    char dir[MYNES_PATH_MAX];
    mynes_config_dir(dir, sizeof(dir));
    CHECK(mynes_mkdir_p(dir));

    /* No file: full-size render scale and low latency on. */
    MynesConfig cfg;
    CHECK(!mynes_config_load(&cfg));
    CHECK(cfg.gpu_render_scale == 1);
    CHECK(cfg.gpu_low_latency == 1);

    /* The first builds stored Auto's index as the default; it is ignored. */
    write_config("{\n    \"gpu_render_scale\": 0,\n    \"gpu_low_latency\": 1,\n    \"last_preset\": \"x.json\"\n}\n");
    CHECK(mynes_config_load(&cfg));
    CHECK(cfg.gpu_render_scale == 1);
    CHECK(strcmp(cfg.last_preset, "x.json") == 0);
    /* Their other indices were explicit choices and are kept. */
    write_config("{\n    \"gpu_render_scale\": 2,\n}\n");
    CHECK(mynes_config_load(&cfg));
    CHECK(cfg.gpu_render_scale == 2);
    write_config("{\n    \"gpu_render_scale\": 7,\n}\n");
    CHECK(mynes_config_load(&cfg));
    CHECK(cfg.gpu_render_scale == 1);

    /* Every value survives a save and load, stored by name. */
    const char *names[] = { "\"auto\"", "\"1\"", "\"0.75\"", "\"0.5\"" };
    for (int i = 0; i < 4; i++) {
        MynesConfig out;
        mynes_config_load(&out);
        out.gpu_render_scale = i;
        CHECK(mynes_config_save(&out));
        CHECK(config_contains(names[i]));
        CHECK(mynes_config_load(&cfg));
        CHECK(cfg.gpu_render_scale == i);
    }

    /* Unknown names and out-of-range values fall back to full size. */
    write_config("{\n    \"gpu_render_scale\": \"0.3\"\n}\n");
    CHECK(mynes_config_load(&cfg));
    CHECK(cfg.gpu_render_scale == 1);
    cfg.gpu_render_scale = 9;
    CHECK(mynes_config_save(&cfg));
    CHECK(config_contains("\"gpu_render_scale\": \"1\""));

    /* Brackets inside a ROM name do not close the recent list. */
    write_config("{\n    \"recent_roms\": [\n        \"/roms/Metroid (U) [!].nes\",\n"
                 "        \"/roms/a]b.nes\",\n        \"/roms/c.nes\"\n    ],\n"
                 "    \"gpu_render_scale\": 2,\n    \"last_preset\": \"y.json\"\n}\n");
    CHECK(mynes_config_load(&cfg));
    CHECK(cfg.recent_count == 3);
    CHECK(strcmp(cfg.recent_roms[0], "/roms/Metroid (U) [!].nes") == 0);
    CHECK(strcmp(cfg.recent_roms[1], "/roms/a]b.nes") == 0);
    CHECK(strcmp(cfg.recent_roms[2], "/roms/c.nes") == 0);
    CHECK(cfg.gpu_render_scale == 2);
    CHECK(strcmp(cfg.last_preset, "y.json") == 0);
    CHECK(mynes_config_save(&cfg));
    MynesConfig again;
    CHECK(mynes_config_load(&again));
    CHECK(again.recent_count == 3);
    CHECK(strcmp(again.recent_roms[0], "/roms/Metroid (U) [!].nes") == 0);
    CHECK(strcmp(again.recent_roms[2], "/roms/c.nes") == 0);
    /* An array on one line, and an empty one, close where they end. */
    write_config("{\n    \"recent_roms\": [\"/roms/[x].nes\", \"/roms/y.nes\"],\n"
                 "    \"last_preset\": \"z.json\"\n}\n");
    CHECK(mynes_config_load(&cfg));
    CHECK(cfg.recent_count == 2);
    CHECK(strcmp(cfg.recent_roms[0], "/roms/[x].nes") == 0);
    CHECK(strcmp(cfg.last_preset, "z.json") == 0);
    write_config("{\n    \"recent_roms\": [],\n    \"last_preset\": \"w.json\"\n}\n");
    CHECK(mynes_config_load(&cfg));
    CHECK(cfg.recent_count == 0);
    CHECK(strcmp(cfg.last_preset, "w.json") == 0);

    /* A relative path is stored absolute, through its directory. */
    char cwd[MYNES_PATH_MAX], rom[MYNES_PATH_MAX + 32], other[MYNES_PATH_MAX + 32];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
    CHECK(chdir(dir) == 0);
    char *real_dir = realpath(".", NULL);
    CHECK(real_dir != NULL);
    if (!real_dir) return 1;
    CHECK(mkdir("roms", 0755) == 0);
    FILE *rf = fopen("roms/game.nes", "wb");
    CHECK(rf != NULL);
    if (rf) fclose(rf);
    MynesConfig recents;
    memset(&recents, 0, sizeof(recents));
    mynes_config_add_recent(&recents, "roms/game.nes");
    mynes_config_add_recent(&recents, "missing.nes");
    mynes_config_add_recent(&recents, "./roms/../roms/game.nes");
    snprintf(rom, sizeof(rom), "%s/roms/game.nes", real_dir);
    CHECK(recents.recent_count == 2);
    CHECK(strcmp(recents.recent_roms[0], rom) == 0);
    /* A missing file is anchored where it was typed. */
    snprintf(other, sizeof(other), "%s/missing.nes", real_dir);
    CHECK(strcmp(recents.recent_roms[1], other) == 0);

    /* A symlinked ROM keeps its own name, which its saves are named after,
     * rather than its target's. */
    CHECK(symlink("roms/game.nes", "Linked Game.nes") == 0);
    mynes_config_add_recent(&recents, "Linked Game.nes");
    snprintf(other, sizeof(other), "%s/Linked Game.nes", real_dir);
    CHECK(recents.recent_count == 3);
    CHECK(strcmp(recents.recent_roms[0], other) == 0);

    /* An entry an older build stored relative is the same ROM when it names
     * the same file from here: reopening moves it, it is not listed twice. */
    memset(&recents, 0, sizeof(recents));
    snprintf(recents.recent_roms[0], MYNES_PATH_MAX, "roms/game.nes");
    snprintf(recents.recent_roms[1], MYNES_PATH_MAX, "roms/other.nes");
    recents.recent_count = 2;
    mynes_config_add_recent(&recents, "roms/other.nes");
    mynes_config_add_recent(&recents, rom);
    CHECK(recents.recent_count == 2);
    CHECK(strcmp(recents.recent_roms[0], rom) == 0);
    snprintf(other, sizeof(other), "%s/roms/other.nes", real_dir);
    CHECK(strcmp(recents.recent_roms[1], other) == 0);

    remove("Linked Game.nes");
    remove("roms/game.nes");
    rmdir("roms");
    free(real_dir);
    CHECK(chdir(cwd) == 0);

    /* A save that cannot be written reports it and leaves the old file. */
    char blocked[MYNES_PATH_MAX + 8];
    mynes_config_path(blocked, sizeof(blocked));
    strcat(blocked, ".tmp");
    CHECK(mkdir(blocked, 0755) == 0);
    mynes_config_set_last_preset(&cfg, "v.json");
    CHECK(!mynes_config_save(&cfg));
    CHECK(config_contains("\"w.json\"") && !config_contains("\"v.json\""));
    rmdir(blocked);

    /* Tidy up. */
    char path[MYNES_PATH_MAX];
    mynes_config_path(path, sizeof(path));
    remove(path);
    rmdir(dir);
    rmdir(root);

    printf("Config regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
