/*
 * config.c — MyNES persistent settings (see config.h).
 *
 * Hand-rolled JSON I/O. The format is line-oriented and tolerant: keys
 * are recognised by simple substring match, unknown lines are skipped.
 * No external deps.
 */
#define _XOPEN_SOURCE 700  /* realpath under strict C11 */
#include "config.h"
#include "saves.h"

#include <stdarg.h>
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

void mynes_config_dir(char *out, int out_sz) {
    config_dir(out, out_sz);
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
    if (!*path) return false;
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

/* Growable text; `failed` sticks after the first allocation failure. */
typedef struct {
    char  *data;
    size_t len, cap;
    bool   failed;
} TextBuf;

static void text_printf(TextBuf *t, const char *fmt, ...) {
    if (t->failed) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { t->failed = true; return; }
    if (t->len + (size_t)n + 1 > t->cap) {
        size_t cap = (t->len + (size_t)n + 1) * 2;
        char *grown = (char *)realloc(t->data, cap);
        if (!grown) { t->failed = true; return; }
        t->data = grown;
        t->cap = cap;
    }
    va_start(ap, fmt);
    vsnprintf(t->data + t->len, t->cap - t->len, fmt, ap);
    va_end(ap);
    t->len += (size_t)n;
}

static void json_escape(TextBuf *t, const char *s) {
    text_printf(t, "\"");
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '\\': text_printf(t, "\\\\"); break;
        case '"':  text_printf(t, "\\\""); break;
        case '\n': text_printf(t, "\\n");  break;
        case '\r': text_printf(t, "\\r");  break;
        case '\t': text_printf(t, "\\t");  break;
        default:
            if (c < 0x20) text_printf(t, "\\u%04x", c);
            else          text_printf(t, "%c", c);
        }
    }
    text_printf(t, "\"");
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

/* Take the recent_roms entries on one line, starting inside the array.
 * Only a `]` outside a string closes it: ROM names like "Metroid (U) [!]"
 * carry brackets. Returns whether the array is still open. */
static bool take_recent_items(const char *p, MynesConfig *cfg) {
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == ',') p++;
        if (*p == ']') return false;
        char tmp[MYNES_PATH_MAX];
        if (*p != '"' || !json_take_string(&p, tmp, sizeof(tmp))) return true;
        if (cfg->recent_count < MYNES_RECENT_MAX) {
            memcpy(cfg->recent_roms[cfg->recent_count], tmp, strlen(tmp) + 1);
            cfg->recent_count++;
        }
    }
}

/* gpu_render_scale values, in MynesConfig order. */
static const char *const render_scale_names[] = { "auto", "1", "0.75", "0.5" };

bool mynes_config_load(MynesConfig *cfg) {
    if (!cfg) return false;
    memset(cfg, 0, sizeof(*cfg));
    /* Absent from older files, and on is the better default: the key is
     * only written as 0 when the user switched it off. */
    cfg->gpu_low_latency = 1;
    cfg->gpu_panel_primaries = 1;
    cfg->gpu_hdr_boost = 100;
    for (int i = 0; i < 3; i++) cfg->gpu_lab_gain[i] = 100;
    cfg->gpu_lab_fill = 28;
    /* Full size: a smaller internal CRT is upsampled, which moves the
     * scanlines off whole panel rows. */
    cfg->gpu_render_scale = 1;

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

        if (in_recent) {
            in_recent = take_recent_items(s, cfg);
        } else if (strstr(s, "\"recent_roms\"") && strchr(s, '[')) {
            in_recent = take_recent_items(strchr(s, '[') + 1, cfg);
        } else if (strstr(s, "\"gpu_hdr_gain_mode\"")) {
            const char *colon=strchr(s, ':');
            cfg->gpu_hdr_gain_mode=colon && atoi(colon+1)==1 ? 1 : 0;
        } else if (strstr(s, "\"gpu_hdr_boost\"")) {
            const char *colon=strchr(s, ':'); int v=colon ? atoi(colon+1) : 100;
            cfg->gpu_hdr_boost=v<100 ? 100 : v>200 ? 200 : v;
        } else if (strstr(s, "\"gpu_panel_primaries\"")) {
            const char *colon=strchr(s, ':');
            cfg->gpu_panel_primaries=colon && atoi(colon+1)==0 ? 0 : 1;
        } else if (strstr(s, "\"gpu_lab_")) {
            const char *colon=strchr(s, ':'); int v=colon ? atoi(colon+1) : 0;
            static const char *names[]={"split","gap_r","gap_g","gap_b","gain_r","gain_g","gain_b","fill","reference","fit"};
            int *slots[]={&cfg->gpu_lab_split,&cfg->gpu_lab_gap[0],&cfg->gpu_lab_gap[1],&cfg->gpu_lab_gap[2],
                          &cfg->gpu_lab_gain[0],&cfg->gpu_lab_gain[1],&cfg->gpu_lab_gain[2],&cfg->gpu_lab_fill,&cfg->gpu_lab_reference,&cfg->gpu_lab_fit};
            for (int i = 0; i < 10; i++) {
                char key[32]; snprintf(key,sizeof(key),"\"gpu_lab_%s\"",names[i]);
                if (strstr(s,key)) *slots[i]=v;
            }
        } else if (strstr(s, "\"gpu_panel_subpixels\"")) {
            const char *colon=strchr(s, ':');
            int order=colon ? atoi(colon+1) : 0;
            cfg->gpu_panel_subpixels=order==1 || order==2 ? order : 0;
        } else if (strstr(s, "\"gpu_mask_alignment\"")) {
            const char *colon=strchr(s, ':');
            cfg->gpu_mask_alignment=colon && atoi(colon+1)==1 ? 1 : 0;
        } else if (strstr(s, "\"gpu_room_reflections\"")) {
            const char *colon=strchr(s, ':');
            cfg->gpu_room_reflections=colon && atoi(colon+1)==1 ? 1 : 0;
        } else if (strstr(s, "\"gpu_render_scale\"")) {
            /* Stored by name. The first builds wrote an index and saved
             * Auto's 0 as the default, so a bare 0 is ignored; 1..3 were
             * explicit menu choices and keep their meaning. */
            const char *colon=strchr(s, ':');
            const char *cur=colon ? colon+1 : NULL;
            char name[16];
            if (cur && json_take_string(&cur, name, sizeof(name))) {
                for (int i=0; i<4; i++)
                    if (strcmp(name, render_scale_names[i])==0) cfg->gpu_render_scale=i;
            } else if (colon) {
                int index=atoi(colon+1);
                if (index>=1 && index<=3) cfg->gpu_render_scale=index;
            }
        } else if (strstr(s, "\"gpu_low_latency\"")) {
            const char *colon=strchr(s, ':');
            cfg->gpu_low_latency=colon && atoi(colon+1)==0 ? 0 : 1;
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

    /* Built in memory and written through a renamed temporary file, so a
     * failed or interrupted save never leaves a truncated config behind. */
    TextBuf t = {0};
    text_printf(&t, "{\n");
    text_printf(&t, "    \"recent_roms\": [\n");
    for (int i = 0; i < cfg->recent_count; i++) {
        text_printf(&t, "        ");
        json_escape(&t, cfg->recent_roms[i]);
        text_printf(&t, "%s", i + 1 < cfg->recent_count ? ",\n" : "\n");
    }
    text_printf(&t, "    ],\n");
    text_printf(&t, "    \"gpu_mask_alignment\": %d,\n",cfg->gpu_mask_alignment==1 ? 1 : 0);
    text_printf(&t, "    \"gpu_hdr_gain_mode\": %d,\n",cfg->gpu_hdr_gain_mode==1 ? 1 : 0);
    text_printf(&t, "    \"gpu_panel_primaries\": %d,\n",cfg->gpu_panel_primaries==0 ? 0 : 1);
    text_printf(&t, "    \"gpu_hdr_boost\": %d,\n",cfg->gpu_hdr_boost);
    text_printf(&t, "    \"gpu_lab_split\": %d,\n    \"gpu_lab_gap_r\": %d,\n    \"gpu_lab_gap_g\": %d,\n    \"gpu_lab_gap_b\": %d,\n",
        cfg->gpu_lab_split,cfg->gpu_lab_gap[0],cfg->gpu_lab_gap[1],cfg->gpu_lab_gap[2]);
    text_printf(&t, "    \"gpu_lab_gain_r\": %d,\n    \"gpu_lab_gain_g\": %d,\n    \"gpu_lab_gain_b\": %d,\n    \"gpu_lab_fill\": %d,\n",
        cfg->gpu_lab_gain[0],cfg->gpu_lab_gain[1],cfg->gpu_lab_gain[2],cfg->gpu_lab_fill);
    text_printf(&t, "    \"gpu_lab_reference\": %d,\n    \"gpu_lab_fit\": %d,\n",cfg->gpu_lab_reference,cfg->gpu_lab_fit);
    text_printf(&t, "    \"gpu_panel_subpixels\": %d,\n",cfg->gpu_panel_subpixels==1 || cfg->gpu_panel_subpixels==2 ? cfg->gpu_panel_subpixels : 0);
    text_printf(&t, "    \"gpu_room_reflections\": %d,\n",cfg->gpu_room_reflections==1 ? 1 : 0);
    text_printf(&t, "    \"gpu_render_scale\": \"%s\",\n",
            render_scale_names[cfg->gpu_render_scale>=0 && cfg->gpu_render_scale<=3 ? cfg->gpu_render_scale : 1]);
    text_printf(&t, "    \"gpu_low_latency\": %d,\n",cfg->gpu_low_latency==0 ? 0 : 1);
    text_printf(&t, "    \"last_preset\": ");
    json_escape(&t, cfg->last_preset);
    text_printf(&t, "\n}\n");

    bool ok = !t.failed;
    if (!ok) fprintf(stderr, "mynes_config_save: out of memory\n");
    ok = ok && mynes_write_file_atomic(path, t.data, t.len);
    free(t.data);
    return ok;
}

void mynes_config_add_recent(MynesConfig *cfg, const char *path) {
    if (!cfg || !path || !*path) return;

    /* A relative path from the command line only means something in the
     * directory it was typed in, and the list is opened from anywhere. */
    char *absolute = realpath(path, NULL);
    if (absolute && strlen(absolute) < MYNES_PATH_MAX) path = absolute;

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
    free(absolute);
}

void mynes_config_set_last_preset(MynesConfig *cfg, const char *slug) {
    if (!cfg) return;
    if (!slug) slug = "";
    strncpy(cfg->last_preset, slug, MYNES_PRESET_MAX - 1);
    cfg->last_preset[MYNES_PRESET_MAX - 1] = '\0';
}
