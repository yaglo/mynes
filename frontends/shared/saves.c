/*
 * saves.c — battery RAM and save-state files (see saves.h).
 */
#define _POSIX_C_SOURCE 200809L  /* fileno and fsync under strict C11 */
#include "saves.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

/* Bounded append. Paths are joined this way rather than with one snprintf
 * so that GCC's format-truncation analysis has nothing to guess about.
 * A path that does not fit is an error, not a shorter path: cutting off
 * the ".s<N>" suffix would give every state slot the same file. */
static bool append(char *out, int out_sz, const char *s) {
    if (out_sz <= 0) return false;
    size_t used = strlen(out), len = strlen(s);
    if (len >= (size_t)out_sz - used) return false;
    memcpy(out + used, s, len + 1);
    return true;
}

/* Leave an empty path, which every reader and writer here refuses. */
static void path_too_long(char *out, int out_sz, const char *what) {
    fprintf(stderr, "%s: path too long\n", what);
    if (out_sz > 0) out[0] = '\0';
}

void mynes_saves_dir(char *out, int out_sz) {
    mynes_config_dir(out, out_sz);
    if (!append(out, out_sz, "/saves")) path_too_long(out, out_sz, "Battery saves");
}

void mynes_states_dir(char *out, int out_sz) {
    mynes_config_dir(out, out_sz);
    if (!append(out, out_sz, "/states")) path_too_long(out, out_sz, "Save states");
}

void mynes_save_basename(const char *rom_path, uint32_t rom_crc, char *out, int out_sz) {
    const char *base = rom_path ? rom_path : "";
    for (const char *p = base; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    /* Drop the extension, but a leading dot is part of a hidden file's name. */
    const char *dot = strrchr(base, '.');
    size_t len = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    if (!len) { base = "rom"; len = 3; }
    /* Leave room for "-xxxxxxxx" and the terminator. */
    int cap = out_sz - 10;
    if (cap < 1) { if (out_sz > 0) out[0] = '\0'; return; }
    if (len > (size_t)cap) len = (size_t)cap;
    snprintf(out, out_sz, "%.*s-%08x", (int)len, base, (unsigned)rom_crc);
}

void mynes_saves_open(MynesSaves *s, const char *rom_path, uint32_t rom_crc, bool has_battery) {
    memset(s, 0, sizeof(*s));
    s->battery = has_battery;
    s->rom_crc = rom_crc;
    mynes_save_basename(rom_path, rom_crc, s->name, sizeof(s->name));
    mynes_saves_dir(s->sav_path, sizeof(s->sav_path));
    if (!*s->sav_path || !append(s->sav_path, sizeof(s->sav_path), "/")
        || !append(s->sav_path, sizeof(s->sav_path), s->name)
        || !append(s->sav_path, sizeof(s->sav_path), ".sav"))
        path_too_long(s->sav_path, sizeof(s->sav_path), "Battery save");
}

bool mynes_saves_restore(MynesSaves *s, uint8_t *prg_ram) {
    if (!s->battery) return false;
    /* Whatever is in RAM now is the baseline a later flush compares against,
     * so a cartridge that never touches its RAM never creates a file. */
    memcpy(s->on_disk, prg_ram, MYNES_PRG_RAM_SIZE);
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return false;
    /* A short file (from another emulator, or a truncated write that was
     * never renamed) still restores what it has; the rest stays as it was. */
    size_t n = fread(s->on_disk, 1, MYNES_PRG_RAM_SIZE, f);
    fclose(f);
    (void)n;
    memcpy(prg_ram, s->on_disk, MYNES_PRG_RAM_SIZE);
    return true;
}

bool mynes_saves_flush(MynesSaves *s, const uint8_t *prg_ram, bool force) {
    if (!s->battery) return true;
    if (!force && memcmp(prg_ram, s->on_disk, MYNES_PRG_RAM_SIZE) == 0) return true;
    char dir[MYNES_PATH_MAX];
    mynes_saves_dir(dir, sizeof(dir));
    if (!mynes_mkdir_p(dir)) {
        fprintf(stderr, "Battery save: cannot create %s: %s\n", dir, strerror(errno));
        return false;
    }
    if (!mynes_write_file_atomic(s->sav_path, prg_ram, MYNES_PRG_RAM_SIZE)) return false;
    memcpy(s->on_disk, prg_ram, MYNES_PRG_RAM_SIZE);
    return true;
}

void mynes_state_path(const MynesSaves *s, int slot, char *out, int out_sz) {
    char suffix[16];
    snprintf(suffix, sizeof(suffix), ".s%d", slot);
    mynes_states_dir(out, out_sz);
    if (!*out || !append(out, out_sz, "/") || !append(out, out_sz, s->name)
        || !append(out, out_sz, suffix))
        path_too_long(out, out_sz, "Save state");
}

bool mynes_state_write(const MynesSaves *s, int slot, const void *data, size_t size) {
    char dir[MYNES_PATH_MAX], path[MYNES_PATH_MAX];
    mynes_states_dir(dir, sizeof(dir));
    if (!mynes_mkdir_p(dir)) {
        fprintf(stderr, "Save state: cannot create %s: %s\n", dir, strerror(errno));
        return false;
    }
    mynes_state_path(s, slot, path, sizeof(path));
    return mynes_write_file_atomic(path, data, size);
}

void *mynes_state_read(const MynesSaves *s, int slot, size_t *size) {
    char path[MYNES_PATH_MAX];
    mynes_state_path(s, slot, path, sizeof(path));
    *size = 0;
    FILE *f = *path ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long length = ftell(f);
    /* States are a few hundred KB; anything past 16 MB is not one of ours. */
    if (length <= 0 || length > 16 << 20 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    void *data = malloc((size_t)length);
    if (!data) { fclose(f); return NULL; }
    size_t n = fread(data, 1, (size_t)length, f);
    fclose(f);
    if (n != (size_t)length) { free(data); return NULL; }
    *size = n;
    return data;
}

#ifndef _WIN32
/* A rename is durable only once the directory holding it is on disk. */
static void sync_parent_dir(const char *path) {
    char dir[MYNES_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) snprintf(dir, sizeof(dir), ".");
    else if (slash == dir) dir[1] = '\0';
    else *slash = '\0';
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    /* Some filesystems refuse to sync a directory; the file itself is
     * already in place by now, so that is not a failed write. */
    (void)fsync(fd);
    close(fd);
}
#endif

bool mynes_write_file_atomic(const char *path, const void *data, size_t size) {
    char tmp[MYNES_PATH_MAX];
    if (!*path) return false;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        fprintf(stderr, "Cannot write %s: path too long\n", path);
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "Cannot write %s: %s\n", tmp, strerror(errno));
        return false;
    }
    bool ok = fwrite(data, 1, size, f) == size && fflush(f) == 0;
#ifndef _WIN32
    /* Otherwise the rename can reach the disk before the data, and a crash
     * leaves an empty file where the previous one used to be. */
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = (fclose(f) == 0) && ok;
    if (!ok) fprintf(stderr, "Cannot write %s: %s\n", tmp, strerror(errno));
    if (ok && rename(tmp, path) != 0) {
        fprintf(stderr, "Cannot replace %s: %s\n", path, strerror(errno));
        ok = false;
    }
#ifndef _WIN32
    if (ok) sync_parent_dir(path);
#endif
    if (!ok) remove(tmp);
    return ok;
}
