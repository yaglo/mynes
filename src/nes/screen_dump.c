#include "nes/screen_dump.h"
#include "ppu/ppu.h"
#include <string.h>
#include <stdlib.h>

void charmap_set_default(CharMap *cm) {
    /* Default: everything is '.' */
    memset(cm->tile_map, '.', 256);

    /* $00-$09 -> '0'-'9' */
    for (int i = 0; i <= 9; i++)
        cm->tile_map[i] = '0' + i;

    /* $0A-$23 -> 'A'-'Z' */
    for (int i = 0; i < 26; i++)
        cm->tile_map[0x0A + i] = 'A' + i;

    /* $24 = space */
    cm->tile_map[0x24] = ' ';
}

bool charmap_load(CharMap *cm, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return false;

    /* Start with default */
    charmap_set_default(cm);

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        /* Parse "HH=C" or "HH= " (space) */
        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        unsigned int tile = (unsigned int)strtoul(line, NULL, 16);
        if (tile > 255) continue;

        char ch = eq[1];
        /* Handle empty value as space */
        if (ch == '\n' || ch == '\r' || ch == '\0')
            ch = ' ';

        cm->tile_map[tile] = ch;
    }

    fclose(fp);
    return true;
}

int screen_dump_to_buffer(const PPU *ppu, const CharMap *cm,
                          char *buf, int buf_size) {
    int pos = 0;
    for (int row = 0; row < 30 && pos < buf_size - 2; row++) {
        for (int col = 0; col < 32 && pos < buf_size - 2; col++) {
            uint8_t tile = ppu->vram[0x2000 + row * 32 + col];
            buf[pos++] = cm->tile_map[tile];
        }
        buf[pos++] = '\n';
    }
    buf[pos] = '\0';
    return pos;
}

void screen_dump_to_file(const PPU *ppu, const CharMap *cm, FILE *fp) {
    char buf[30 * 33 + 1];
    screen_dump_to_buffer(ppu, cm, buf, sizeof(buf));
    fputs(buf, fp);
}

void screen_dump_print(const PPU *ppu, const CharMap *cm) {
    screen_dump_to_file(ppu, cm, stdout);
}
