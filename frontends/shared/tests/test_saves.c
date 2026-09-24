/* Battery and state file helpers against a throwaway config directory. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "saves.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Saves FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

int main(void) {
    char root[] = "/tmp/mynes-saves-XXXXXX";
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    setenv("XDG_CONFIG_HOME", root, 1);

    char dir[MYNES_PATH_MAX], name[MYNES_SAVE_NAME_MAX];
    mynes_saves_dir(dir, sizeof(dir));
    CHECK(ends_with(dir, "/mynes/saves"));
    CHECK(!exists(dir)); /* created only when something is written */
    mynes_states_dir(dir, sizeof(dir));
    CHECK(ends_with(dir, "/mynes/states"));

    /* Names: basename, no extension, CRC suffix; both separators; hidden files. */
    mynes_save_basename("/roms/Zelda (U).nes", 0xDEADBEEF, name, sizeof(name));
    CHECK(strcmp(name, "Zelda (U)-deadbeef") == 0);
    mynes_save_basename("C:\\games\\game.NES", 0x00000001, name, sizeof(name));
    CHECK(strcmp(name, "game-00000001") == 0);
    mynes_save_basename("noext", 0x12345678, name, sizeof(name));
    CHECK(strcmp(name, "noext-12345678") == 0);
    mynes_save_basename(".hidden", 0x12345678, name, sizeof(name));
    CHECK(strcmp(name, ".hidden-12345678") == 0);
    mynes_save_basename("/dir/", 0xABCDEF01, name, sizeof(name));
    CHECK(strcmp(name, "rom-abcdef01") == 0);
    char tiny[16];
    mynes_save_basename("averyveryverylongname.nes", 0xABCDEF01, tiny, sizeof(tiny));
    CHECK(strcmp(tiny, "averyv-abcdef01") == 0);

    /* Battery round trip. */
    static uint8_t ram[MYNES_PRG_RAM_SIZE], back[MYNES_PRG_RAM_SIZE];
    MynesSaves s;
    mynes_saves_open(&s, "/roms/Zelda (U).nes", 0xDEADBEEF, true, MYNES_PRG_RAM_SIZE);
    CHECK(s.battery && ends_with(s.sav_path, "/mynes/saves/Zelda (U)-deadbeef.sav"));
    CHECK(!mynes_saves_restore(&s, ram));          /* nothing on disk yet */
    CHECK(mynes_saves_flush(&s, ram, false));       /* unchanged: no write */
    CHECK(!exists(s.sav_path));
    for (int i = 0; i < MYNES_PRG_RAM_SIZE; i++) ram[i] = (uint8_t)(i * 7);
    CHECK(mynes_saves_flush(&s, ram, false));
    CHECK(exists(s.sav_path));
    char tmp[MYNES_PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.tmp", s.sav_path);
    CHECK(!exists(tmp));                             /* renamed into place */
    struct stat st;
    CHECK(stat(s.sav_path, &st) == 0 && st.st_size == MYNES_PRG_RAM_SIZE);

    MynesSaves t;
    mynes_saves_open(&t, "/elsewhere/Zelda (U).nes", 0xDEADBEEF, true, MYNES_PRG_RAM_SIZE);
    memset(back, 0, sizeof(back));
    CHECK(mynes_saves_restore(&t, back));
    CHECK(memcmp(back, ram, sizeof(ram)) == 0);
    CHECK(mynes_saves_flush(&t, back, false));       /* equal to disk: no rewrite needed */
    back[100] ^= 0xFF;
    CHECK(mynes_saves_flush(&t, back, false));
    memset(ram, 0, sizeof(ram));
    CHECK(mynes_saves_restore(&s, ram) && ram[100] == (uint8_t)((100 * 7) ^ 0xFF));

    /* A different dump of the same name gets its own file. */
    MynesSaves u;
    mynes_saves_open(&u, "/roms/Zelda (U).nes", 0x0BADF00D, true, MYNES_PRG_RAM_SIZE);
    CHECK(strcmp(u.sav_path, s.sav_path) != 0);
    CHECK(!mynes_saves_restore(&u, back));

    /* MMC5 keeps 64 KB of pages: an 8 KB save from before fills page 0
     * and leaves the rest, and the next write holds every page. */
    static uint8_t big[MYNES_PRG_RAM_MAX];
    MynesSaves b;
    mynes_saves_open(&b, "/roms/Zelda (U).nes", 0xDEADBEEF, true, MYNES_PRG_RAM_MAX);
    memset(big, 0x5A, sizeof(big));
    CHECK(mynes_saves_restore(&b, big));
    CHECK(big[100] == (uint8_t)((100 * 7) ^ 0xFF) && big[MYNES_PRG_RAM_SIZE] == 0x5A);
    CHECK(mynes_saves_flush(&b, big, false));        /* unchanged since the restore */
    CHECK(stat(b.sav_path, &st) == 0 && st.st_size == MYNES_PRG_RAM_SIZE);
    big[0xE000] = 0x42;
    CHECK(mynes_saves_flush(&b, big, false));
    CHECK(stat(b.sav_path, &st) == 0 && st.st_size == MYNES_PRG_RAM_MAX);
    memset(big, 0, sizeof(big));
    CHECK(mynes_saves_restore(&b, big) && big[0xE000] == 0x42 && big[100] == ram[100]);

    /* No battery: never writes, even when forced. */
    MynesSaves n;
    mynes_saves_open(&n, "/roms/Mario.nes", 0x11111111, false, MYNES_PRG_RAM_SIZE);
    CHECK(mynes_saves_flush(&n, ram, true));
    CHECK(!exists(n.sav_path));

    /* State slots. */
    char path[MYNES_PATH_MAX];
    mynes_state_path(&s, 3, path, sizeof(path));
    CHECK(ends_with(path, "/mynes/states/Zelda (U)-deadbeef.s3"));
    size_t size = 1;
    CHECK(mynes_state_read(&s, 3, &size) == NULL && size == 0);
    const char payload[] = "MYNESST\0state bytes";
    CHECK(mynes_state_write(&s, 3, payload, sizeof(payload)));
    void *data = mynes_state_read(&s, 3, &size);
    CHECK(data && size == sizeof(payload) && memcmp(data, payload, size) == 0);
    free(data);
    CHECK(mynes_state_write(&s, 3, "short", 5));    /* replaces, never appends */
    data = mynes_state_read(&s, 3, &size);
    CHECK(data && size == 5);
    free(data);
    CHECK(mynes_state_read(&s, 1, &size) == NULL);

    /* An unwritable destination fails cleanly and leaves no temp file. */
    CHECK(!mynes_write_file_atomic("/nonexistent-dir/x/y.sav", ram, sizeof(ram)));

    /* A config directory too long for the slot suffix gives no path at all,
     * rather than one truncated path shared by every slot. */
    char deep[MYNES_PATH_MAX];
    snprintf(deep, sizeof(deep), "%s/%0*d", root, (int)(490 - strlen(root)), 0);
    setenv("XDG_CONFIG_HOME", deep, 1);
    MynesSaves d;
    mynes_saves_open(&d, "/roms/Zelda (U).nes", 0xDEADBEEF, true, MYNES_PRG_RAM_SIZE);
    CHECK(d.sav_path[0] == '\0');
    char p1[MYNES_PATH_MAX], p2[MYNES_PATH_MAX];
    mynes_state_path(&d, 1, p1, sizeof(p1));
    mynes_state_path(&d, 2, p2, sizeof(p2));
    CHECK(p1[0] == '\0' && p2[0] == '\0');
    CHECK(mynes_state_read(&d, 1, &size) == NULL);
    CHECK(!mynes_write_file_atomic("", ram, sizeof(ram)));
    setenv("XDG_CONFIG_HOME", root, 1);

    /* Tidy up. */
    remove(path);
    remove(s.sav_path);
    mynes_states_dir(dir, sizeof(dir)); rmdir(dir);
    mynes_saves_dir(dir, sizeof(dir)); rmdir(dir);
    mynes_config_dir(dir, sizeof(dir)); rmdir(dir);
    rmdir(root);

    printf("Saves helper regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
