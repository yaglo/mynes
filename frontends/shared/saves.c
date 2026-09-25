/*
 * saves.c — battery RAM and save-state files (see saves.h).
 */
#define _POSIX_C_SOURCE 200809L  /* fileno, fsync and lstat under strict C11 */
#define _DARWIN_C_SOURCE         /* and F_FULLFSYNC, which strict POSIX hides on macOS */
#include "saves.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
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

void mynes_saves_open(MynesSaves *s, const char *rom_path, uint32_t rom_crc, bool has_battery,
                      size_t ram_size) {
    memset(s, 0, sizeof(*s));
    s->battery = has_battery;
    s->rom_crc = rom_crc;
    s->ram_size = ram_size < MYNES_PRG_RAM_MAX ? ram_size : MYNES_PRG_RAM_MAX;
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
    memcpy(s->on_disk, prg_ram, s->ram_size);
    FILE *f = fopen(s->sav_path, "rb");
    if (!f) return false;
    /* A short file (from another emulator, an 8 KB save of an MMC5 game
     * from before its other pages were kept, or a truncated write that was
     * never renamed) still restores what it has; the rest stays as it was. */
    size_t n = fread(s->on_disk, 1, s->ram_size, f);
    fclose(f);
    (void)n;
    memcpy(prg_ram, s->on_disk, s->ram_size);
    return true;
}

bool mynes_saves_flush(MynesSaves *s, const uint8_t *prg_ram, bool force) {
    if (!s->battery) return true;
    if (!force && memcmp(prg_ram, s->on_disk, s->ram_size) == 0) return true;
    char dir[MYNES_PATH_MAX];
    mynes_saves_dir(dir, sizeof(dir));
    if (!mynes_mkdir_p(dir)) {
        fprintf(stderr, "Battery save: cannot create %s: %s\n", dir, strerror(errno));
        return false;
    }
    if (!mynes_write_file_atomic(s->sav_path, prg_ram, s->ram_size)) return false;
    memcpy(s->on_disk, prg_ram, s->ram_size);
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
/* On macOS fsync hands the data to the drive and stops there: it can sit in
 * the drive's cache and reach the medium out of order, so a power cut may
 * keep the rename and lose the data, or lose both. Apple's fsync(2) names
 * F_FULLFSYNC for that, which flushes the cache and costs several
 * milliseconds a call, so only callers that ask for it pay (see saves.h).
 * Filesystems that do not support it (some network and FAT volumes) fall
 * back to fsync. Elsewhere fsync already waits for stable storage. */
static int sync_fd(int fd, bool full_flush) {
#ifdef F_FULLFSYNC
    if (full_flush && fcntl(fd, F_FULLFSYNC) == 0) return 0;
#else
    (void)full_flush;
#endif
    return fsync(fd);
}

/* The directory part of `path`: "." when there is none. */
static void parent_dir(const char *path, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s", path);
    char *slash = strrchr(out, '/');
    if (!slash) snprintf(out, out_sz, ".");
    else if (slash == out) out[1] = '\0';
    else *slash = '\0';
}

/* A rename is durable only once the directory holding it is on disk. */
static void sync_parent_dir(const char *path, bool full_flush) {
    char dir[MYNES_PATH_MAX];
    parent_dir(path, dir, sizeof(dir));
    int fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    /* Some filesystems refuse to sync a directory; the file itself is
     * already in place by now, so that is not a failed write. */
    (void)sync_fd(fd, full_flush);
    close(fd);
}

/* Follow `path` through symlinks to the file they name, which need not exist
 * yet. Renaming onto the link itself would replace it with a regular file,
 * and a config.json linked in from elsewhere would silently stop being
 * shared. Only the last component is followed; a linked directory on the
 * way works as it is. */
static bool follow_links(const char *path, char *out, size_t out_sz) {
    if (snprintf(out, out_sz, "%s", path) >= (int)out_sz) {
        errno = ENAMETOOLONG;
        return false;
    }
    for (int depth = 0; depth < 32; depth++) {
        struct stat st;
        if (lstat(out, &st) != 0 || !S_ISLNK(st.st_mode)) return true;
        char target[MYNES_PATH_MAX], dir[MYNES_PATH_MAX];
        ssize_t n = readlink(out, target, sizeof(target));
        if (n < 0) return false;
        if ((size_t)n >= sizeof(target)) {
            errno = ENAMETOOLONG;
            return false;
        }
        target[n] = '\0';
        /* A relative target is relative to the link's own directory. */
        parent_dir(out, dir, sizeof(dir));
        if (target[0] == '/' ? snprintf(out, out_sz, "%s", target) >= (int)out_sz
                             : snprintf(out, out_sz, "%s/%s", dir, target) >= (int)out_sz) {
            errno = ENAMETOOLONG;
            return false;
        }
    }
    errno = ELOOP;
    return false;
}
#endif

static bool write_atomic(const char *path, const void *data, size_t size, bool full_flush) {
    char tmp[MYNES_PATH_MAX], target[MYNES_PATH_MAX];
    if (!*path) return false;
#ifndef _WIN32
    if (!follow_links(path, target, sizeof(target))) {
        fprintf(stderr, "Cannot write %s: %s\n", path, strerror(errno));
        return false;
    }
#else
    (void)full_flush;
    snprintf(target, sizeof(target), "%s", path);
#endif
    /* Beside the file the link names, since rename cannot cross filesystems. */
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", target) >= (int)sizeof(tmp)) {
        fprintf(stderr, "Cannot write %s: path too long\n", target);
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
    ok = ok && sync_fd(fileno(f), full_flush) == 0;
#endif
    ok = (fclose(f) == 0) && ok;
    if (!ok) fprintf(stderr, "Cannot write %s: %s\n", tmp, strerror(errno));
    if (ok && rename(tmp, target) != 0) {
        fprintf(stderr, "Cannot replace %s: %s\n", target, strerror(errno));
        ok = false;
    }
#ifndef _WIN32
    if (ok) sync_parent_dir(target, full_flush);
#endif
    if (!ok) remove(tmp);
    return ok;
}

bool mynes_write_file_atomic(const char *path, const void *data, size_t size) {
    return write_atomic(path, data, size, true);
}

bool mynes_write_file_atomic_cached(const char *path, const void *data, size_t size) {
    return write_atomic(path, data, size, false);
}
