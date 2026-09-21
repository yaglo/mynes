/*
 * config.c — MyNES persistent settings (see config.h).
 *
 * Hand-rolled JSON I/O. The format is line-oriented and tolerant: keys
 * are recognised by simple substring match, unknown lines are skipped.
 * No external deps.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

/* ---------------------------------------------------------------------------
 * Path resolution
 * ------------------------------------------------------------------------- */

static void config_dir(char *out, int out_sz) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) {
        snprintf(out, out_sz, "%s/mynes", xdg);
        return;
    }
    if (home && *home) {
        /* Prefer ~/.config/mynes if ~/.config exists, else ~/.mynes. */
        char xdg_home[MYNES_PATH_MAX];
        snprintf(xdg_home, sizeof(xdg_home), "%s/.config", home);
        struct stat st;
        if (stat(xdg_home, &st) == 0 && S_ISDIR(st.st_mode))
            snprintf(out, out_sz, "%s/mynes", xdg_home);
        else
            snprintf(out, out_sz, "%s/.mynes", home);
        return;
    }
    snprintf(out, out_sz, ".mynes");
}

void mynes_config_path(char *out, int out_sz) {
    char dir[MYNES_PATH_MAX];
    config_dir(dir, sizeof(dir));
    snprintf(out, out_sz, "%s/config.json", dir);
}

void mynes_user_presets_dir(char *out, int out_sz) {
    char dir[MYNES_PATH_MAX];
    config_dir(dir, sizeof(dir));
    snprintf(out, out_sz, "%s/presets", dir);
}

/* mkdir -p — public via mynes_mkdir_p (declared in config.h). */
bool mynes_mkdir_p(const char *path) {
    char buf[MYNES_PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0755);  /* ignore EEXIST */
            *p = '/';
        }
    }
    if (mkdir(buf, 0755) == 0 || errno == EEXIST) return true;
    return false;
}

/* ---------------------------------------------------------------------------
 * JSON I/O — line-oriented, dependency-free
 * ------------------------------------------------------------------------- */

static void json_escape(FILE *f, const char *s) {
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '\\': fputs("\\\\", f); break;
        case '"':  fputs("\\\"", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (c < 0x20) fprintf(f, "\\u%04x", c);
            else          fputc((int)c, f);
        }
    }
    fputc('"', f);
}

/* Pull the next quoted JSON string off `*cursor`. Writes the unescaped
 * value into `out` (capped at out_sz-1) and advances `*cursor` past the
 * closing quote. Returns false if no string was found. */
static bool json_take_string(const char **cursor, char *out, int out_sz) {
    const char *p = *cursor;
    while (*p && *p != '"') p++;
    if (*p != '"') return false;
    p++;  /* skip opening quote */
    int o = 0;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) {
            char esc = p[1];
            char ch = esc;
            switch (esc) {
            case 'n': ch = '\n'; break;
            case 'r': ch = '\r'; break;
            case 't': ch = '\t'; break;
            case '"': case '\\': case '/': ch = esc; break;
            default: ch = esc; break;
            }
            if (o + 1 < out_sz) out[o++] = ch;
            p += 2;
        } else {
            if (o + 1 < out_sz) out[o++] = *p;
            p++;
        }
    }
    if (*p != '"') { out[0] = '\0'; return false; }
    out[o] = '\0';
    *cursor = p + 1;  /* skip closing quote */
    return true;
}

bool mynes_config_load(MynesConfig *cfg) {
    if (!cfg) return false;
    memset(cfg, 0, sizeof(*cfg));

    char path[MYNES_PATH_MAX];
    mynes_config_path(path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0 || sz > 1 << 20) { fclose(f); return false; }
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return false; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    /* Parse line-by-line for simplicity. */
    bool in_recent = false;
    char *line = buf;
    while (line && *line) {
        char *eol = strchr(line, '\n');
        if (eol) *eol = '\0';

        /* Skip whitespace. */
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == ',') s++;

        if (strstr(s, "\"recent_roms\"") && strchr(s, '[')) {
            in_recent = true;
        } else if (in_recent && strchr(s, ']')) {
            in_recent = false;
        } else if (in_recent) {
            const char *cur = s;
            char tmp[MYNES_PATH_MAX];
            if (json_take_string(&cur, tmp, sizeof(tmp))
                && cfg->recent_count < MYNES_RECENT_MAX) {
                strncpy(cfg->recent_roms[cfg->recent_count], tmp,
                        MYNES_PATH_MAX - 1);
                cfg->recent_roms[cfg->recent_count][MYNES_PATH_MAX - 1] = '\0';
                cfg->recent_count++;
            }
        } else if (strstr(s, "\"gpu_mask_alignment\"")) {
            const char *colon=strchr(s, ':');
            cfg->gpu_mask_alignment=colon && atoi(colon+1)==1 ? 1 : 0;
        } else if (strstr(s, "\"last_preset\"")) {
            const char *colon = strchr(s, ':');
            if (colon) {
                const char *cur = colon + 1;
                json_take_string(&cur, cfg->last_preset, MYNES_PRESET_MAX);
            }
        }

        line = eol ? eol + 1 : NULL;
    }

    free(buf);
    return true;
}

bool mynes_config_save(const MynesConfig *cfg) {
    if (!cfg) return false;
    char dir[MYNES_PATH_MAX], path[MYNES_PATH_MAX];
    config_dir(dir, sizeof(dir));
    mynes_config_path(path, sizeof(path));

    if (!mynes_mkdir_p(dir)) {
        fprintf(stderr, "mynes_config_save: cannot create %s: %s\n",
                dir, strerror(errno));
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "mynes_config_save: cannot open %s: %s\n",
                path, strerror(errno));
        return false;
    }

    fprintf(f, "{\n");
    fprintf(f, "    \"recent_roms\": [\n");
    for (int i = 0; i < cfg->recent_count; i++) {
        fprintf(f, "        ");
        json_escape(f, cfg->recent_roms[i]);
        fputs(i + 1 < cfg->recent_count ? ",\n" : "\n", f);
    }
    fprintf(f, "    ],\n");
    fprintf(f, "    \"gpu_mask_alignment\": %d,\n",cfg->gpu_mask_alignment==1 ? 1 : 0);
    fprintf(f, "    \"last_preset\": ");
    json_escape(f, cfg->last_preset);
    fprintf(f, "\n}\n");

    fclose(f);
    return true;
}

void mynes_config_add_recent(MynesConfig *cfg, const char *path) {
    if (!cfg || !path || !*path) return;

    /* If already present, remove the existing entry first (move-to-front). */
    int found = -1;
    for (int i = 0; i < cfg->recent_count; i++) {
        if (strcmp(cfg->recent_roms[i], path) == 0) { found = i; break; }
    }
    if (found >= 0) {
        for (int i = found; i + 1 < cfg->recent_count; i++) {
            memcpy(cfg->recent_roms[i], cfg->recent_roms[i + 1], MYNES_PATH_MAX);
        }
        cfg->recent_count--;
    }

    /* Make room at the front. */
    int max_keep = MYNES_RECENT_MAX - 1;
    int n = cfg->recent_count;
    if (n > max_keep) n = max_keep;
    for (int i = n; i > 0; i--) {
        memcpy(cfg->recent_roms[i], cfg->recent_roms[i - 1], MYNES_PATH_MAX);
    }
    strncpy(cfg->recent_roms[0], path, MYNES_PATH_MAX - 1);
    cfg->recent_roms[0][MYNES_PATH_MAX - 1] = '\0';
    if (cfg->recent_count < MYNES_RECENT_MAX) cfg->recent_count++;
}

void mynes_config_set_last_preset(MynesConfig *cfg, const char *slug) {
    if (!cfg) return;
    if (!slug) slug = "";
    strncpy(cfg->last_preset, slug, MYNES_PRESET_MAX - 1);
    cfg->last_preset[MYNES_PRESET_MAX - 1] = '\0';
}
