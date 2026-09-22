/* config.json defaults and the render scale setting against a throwaway
 * config directory. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    /* Tidy up. */
    char path[MYNES_PATH_MAX];
    mynes_config_path(path, sizeof(path));
    remove(path);
    rmdir(dir);
    rmdir(root);

    printf("Config regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
