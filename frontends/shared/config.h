/*
 * config.h — MyNES persistent settings (recent ROMs + last preset).
 *
 * Stored as a tiny JSON file at the user's config dir. Resolution:
 *   1. $XDG_CONFIG_HOME/mynes/config.json
 *   2. $HOME/.config/mynes/config.json
 *   3. $HOME/.mynes/config.json   (fallback for older XDG-less systems)
 *
 * The schema is intentionally minimal so the file remains hand-editable.
 */
#ifndef MYNES_CONFIG_H
#define MYNES_CONFIG_H

#include <stdbool.h>

#define MYNES_RECENT_MAX  50
#define MYNES_PATH_MAX    512
#define MYNES_PRESET_MAX  128

typedef struct {
    char recent_roms[MYNES_RECENT_MAX][MYNES_PATH_MAX];  /* most-recent first */
    int  recent_count;
    int  gpu_room_reflections; /* host setting: simulated room light, off by default */
    int  gpu_mask_alignment; /* host setting: 0=panel pixels, 1=CRT pitch */
    int  gpu_panel_subpixels; /* host setting: 0=off, 1=RGB stripe, 2=BGR stripe */
    int  gpu_hdr_gain_mode;   /* host setting: 0=Auto (fit headroom), 1=preset gain */
    int  gpu_hdr_boost;       /* host setting: Auto gain times this, hundredths, 100 to 200 */
    int  gpu_lab_fit;         /* subpixel lab: 1 = fit the lab half's own gain */
    int  gpu_panel_primaries; /* host setting: 0=sRGB, 1=Display P3 when the panel's layer can take it */
    int  gpu_lab_split;       /* subpixel lab: 0 off, 1 right half, 2 left half */
    int  gpu_lab_reference;   /* subpixel lab: other half 0 shipped grille, 1 no grille, 2 mask/glass bypass */
    int  gpu_lab_gap[3];      /* subpixel lab: gap share per colour, hundredths */
    int  gpu_lab_gain[3];     /* subpixel lab: gain per channel, hundredths */
    int  gpu_lab_fill;        /* subpixel lab: stripe fill, hundredths */
    int  gpu_render_scale;   /* host setting: 0=auto, 1=full (default), 2=3/4, 3=half */
    int  gpu_low_latency;    /* host setting: 1=one picture queued ahead of the display */
    char last_preset[MYNES_PRESET_MAX];                  /* slug or filename */
} MynesConfig;

/* Resolve the config-file path into `out` (size `out_sz`). Always succeeds,
 * even if the directory doesn't exist yet — call this before save/load. */
void mynes_config_path(char *out, int out_sz);

/* The directory config.json lives in (see the resolution order above).
 * Saves and states sit in subdirectories of it. */
void mynes_config_dir(char *out, int out_sz);

/* Resolve the directory MyNES uses for user-saved presets:
 *   <config-dir>/presets   — created lazily by callers via mkdir if missing.
 * Always writes a valid path into `out`. */
void mynes_user_presets_dir(char *out, int out_sz);

/* mkdir -p for `path`. Returns true on success (directory exists at end). */
bool mynes_mkdir_p(const char *path);

/* Load config from disk; returns true if a file was found and parsed.
 * On false, `cfg` holds the defaults and is safe to use: every field is
 * zero except gpu_low_latency (on) and gpu_render_scale (1, full size). */
bool mynes_config_load(MynesConfig *cfg);

/* Write config to disk, creating the parent directory if needed.
 * Returns true on success. */
bool mynes_config_save(const MynesConfig *cfg);

/* Add `path` to recent_roms[], made absolute by its directory with the file
 * name kept as given (saves are named after it). If already present, as that
 * path or as a relative entry naming the same file from the current
 * directory, it's moved to the front; otherwise it's prepended and the list
 * is capped at MYNES_RECENT_MAX. */
void mynes_config_add_recent(MynesConfig *cfg, const char *path);

/* Set last_preset (truncates if too long). */
void mynes_config_set_last_preset(MynesConfig *cfg, const char *slug);

#endif /* MYNES_CONFIG_H */
