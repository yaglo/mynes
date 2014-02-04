#include "fce.h"
#include "fce-internal.h"

#include <string.h>

#include "cpu.h"
#include "memory.h"
#include "ppu.h"

int FPS = 60;
const int SCREEN_WIDTH = 256;
const int SCREEN_HEIGHT = 240;

typedef struct {
	char signature[4];
	byte prg_block_count;
	byte chr_block_count;
	word rom_type;
	byte reserved[8];
} ines_header;

ines_header fce_rom_header;


// FCE Lifecycle

int fce_load_rom(char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "[FCE] Could not open ROM: %s\n", path);
        return -1;
    }

    fread(&fce_rom_header, 1, sizeof(fce_rom_header), f);

    if (memcmp(fce_rom_header.signature, "NES\x1A", 4)) {
        fclose(f);
        fprintf(stderr, "[FCE] ROM file format not supported: %s", path);
        return -1;
    }

    mmc_id = /*((fce_rom_header.rom_type & 0xF000) >> 8) | */((fce_rom_header.rom_type & 0xF0) >> 4);

    printf("[FCE] PRG ROM: %d 16KB Blocks\n", fce_rom_header.prg_block_count);
    printf("[FCE] CHR ROM: %d 8KB Blocks\n", fce_rom_header.chr_block_count);
    
    if ((fce_rom_header.rom_type & 9) == 9) {
        printf("[FCE] Mirroring: Four-screen\n");
    }
    else if (fce_rom_header.rom_type & 1) {
        printf("[FCE] Mirroring: Vertical\n");
    }
    else {
        printf("[FCE] Mirroring: Horizontal\n");
    }
    
    printf("[FCE] SRAM @ $6000-$7FFF: %s\n", fce_rom_header.rom_type & 2 ? "Yes" : "No");
    printf("[FCE] Trainer @ $7000-$71FF: %s\n", fce_rom_header.rom_type & 4 ? "Yes" : "No");
    printf("[FCE] Mapper: 0x%02X (%03d)\n", mmc_id, mmc_id);

    // Quick & Dirty way to load simple ROMs, TODO: rewrite
    int prg_size = fce_rom_header.prg_block_count * 0x4000;
    byte *buf = malloc(prg_size);
    fread(buf, prg_size, 1, f);

    if (mmc_id == 0 || mmc_id == 3) {
        // if there is only one PRG block, we must repeat it twice
        if (fce_rom_header.prg_block_count == 1) {
            mmc_copy(0x8000, buf, 0x4000);
            mmc_copy(0xC000, buf, 0x4000);
        }
        else {
            mmc_copy(0x8000, buf, 0x8000);
        }
    }
    else {
        fprintf(stderr, "[FCE] Mapper not supported\n");
        return -1;
    }

    free(buf);

    // Copying CHR pages into MMC and PPU
    for (int i = 0; i < fce_rom_header.chr_block_count; i++) {
        buf = malloc(0x2000);
        fread(buf, 0x2000, 1, f);
        mmc_append_chr_rom_page(buf);

        if (i == 0) {
            ppu_copy(0x0000, buf, 0x2000);
        }

        free(buf);
    }

    return 0;
}

void fce_init()
{
    if (!al_init()) {
        fprintf(stderr, "[FCE] Failed to initialize Allegro\n");
        return;
    }

    if (!al_init_primitives_addon()) {
        fprintf(stderr, "[FCE] Failed to initialize Allegro Primitives addon\n");
        return;
    }

    fce_display = al_create_display(SCREEN_WIDTH, SCREEN_HEIGHT);
    
    if (!fce_display) {
        fprintf(stderr, "[FCE] Failed to create display\n");
        return;
    }

    fce_timer = al_create_timer(1.0 / FPS);
    fce_event_queue = al_create_event_queue();
    al_install_keyboard();
    al_register_event_source(fce_event_queue, al_get_timer_event_source(fce_timer));
    al_register_event_source(fce_event_queue, al_get_display_event_source(fce_display));
    al_start_timer(fce_timer);

    cpu_init();
    ppu_init();
    ppu_set_mirroring(fce_rom_header.rom_type & 1);
    cpu_reset();
}

void fce_kill()
{
    al_destroy_display(fce_display);
}

void fce_run()
{
    while(1)
    {
        ALLEGRO_EVENT event;
        al_wait_for_event(fce_event_queue, &event);

        if (event.type == ALLEGRO_EVENT_TIMER) {
            int scanlines = 262;
            while (scanlines-- > 0)
            {
                ppu_run(1);
                cpu_run(1364 / 12); // 1 scanline
            }
            
            // if (ppu_shows_background()) {
            //      cpu_run(101);
            //      cpu_run(13);
            //  }
            //  else {
            //      cpu_run(114);
            //  }
            //  
            //  int scanlines = 240;
            //  while (scanlines-- > 0) {
            //      ppu_run(1);
            //      cpu_run(28);
            //  }
            // 
            //  ppu_set_in_vblank(true);
            //  ppu_set_sprite_0_hit(false);
            //  cpu_interrupt();
            // 
            //  for (int scanline = 241; scanline < 260; scanline++) {
            //      if (scanline % 3 == 0)
            //          cpu_run(114);
            //      else
            //          cpu_run(113);
            //  }
        }
        else if (event.type == ALLEGRO_EVENT_DISPLAY_CLOSE) {
            break;
        }
    }

    fce_kill();

    al_destroy_timer(fce_timer);
    al_destroy_event_queue(fce_event_queue);
}



void ppu_draw_sprite_grayscale(int x, int y, word address, bool vflip, bool hflip)
{
    for (int y1 = 0; y1 < 8; y1++) {
        int basey = vflip ? address + 7 - y1 : address + y1;
        byte l = PPU_RAM[basey];
        byte h = PPU_RAM[basey + 8];
        for (int x1 = 0; x1 < 8; x1++) {
            int realx = x + (hflip ? x1 : 7 - x1);

            byte c = (((h >> x1) & 1) << 1) | ((l >> x1) & 1);

            if (c != 0) {
                al_draw_pixel(realx, y + y1 + 1, al_map_rgb(c * 64, c * 64, c * 64));
            }
        }
    }
}

// Rendering

void fce_update_screen()
{
    pal background_color = palette[ppu_ram_read(0x3F00)];
    al_clear_to_color(al_map_rgb(background_color.r, background_color.g, background_color.b));
    // al_clear_to_color(al_map_rgb(0,0,0));
    
    if (ppu_shows_sprites()) {
        al_draw_prim(ppu_behind_background_sprite_pixels, NULL, NULL, 0, ppu_behind_background_sprite_pixels_number, ALLEGRO_PRIM_POINT_LIST);
    }
    
    if (ppu_shows_background()) {
        al_draw_prim(ppu_background_pixels, NULL, NULL, 0, ppu_background_pixels_number, ALLEGRO_PRIM_POINT_LIST);
    }
    
    if (ppu_shows_sprites()) {
        al_draw_prim(ppu_sprite_pixels, NULL, NULL, 0, ppu_sprite_pixels_number, ALLEGRO_PRIM_POINT_LIST);
    }

    // int ntrecordsize = 4;
    // for (int p = 0; p < 64; p++) {
    //     word addr = 0x23C1 + p;
    //     int x1 = p % 8;
    //     int y1 = p / 8;
    //     for (int x = 0; x < ntrecordsize; x++) {
    //         for (int y = 0; y < ntrecordsize; y++) {
    //             al_draw_pixel(264 + x1 * ntrecordsize + x, 1 + y1 * ntrecordsize + y, al_map_rgb(PPU_RAM[addr] + 32, PPU_RAM[addr] + 32, PPU_RAM[addr] + 32));
    //         }
    //     }
    // }
    // for (int p = 0; p < 64; p++) {
    //     word addr = 0x27C1 + p;
    //     int x1 = p % 8;
    //     int y1 = p / 8;
    //     for (int x = 0; x < ntrecordsize; x++) {
    //         for (int y = 0; y < ntrecordsize; y++) {
    //             al_draw_pixel(264 + 8 * ntrecordsize + x1 * ntrecordsize + x, 1 + y1 * ntrecordsize + y, al_map_rgb(PPU_RAM[addr] + 32, PPU_RAM[addr] + 32, PPU_RAM[addr] + 32));
    //         }
    //     }
    // }
    // for (int p = 0; p < 64; p++) {
    //     word addr = 0x2BC1 + p;
    //     int x1 = p % 8;
    //     int y1 = p / 8;
    //     for (int x = 0; x < ntrecordsize; x++) {
    //         for (int y = 0; y < ntrecordsize; y++) {
    //             al_draw_pixel(264 + x1 * ntrecordsize + x, 1 + 8 * ntrecordsize + y1 * ntrecordsize + y, al_map_rgb(PPU_RAM[addr] + 32, PPU_RAM[addr] + 32, PPU_RAM[addr] + 32));
    //         }
    //     }
    // }
    // for (int p = 0; p < 64; p++) {
    //     word addr = 0x2FC1 + p;
    //     int x1 = p % 8;
    //     int y1 = p / 8;
    //     for (int x = 0; x < ntrecordsize; x++) {
    //         for (int y = 0; y < ntrecordsize; y++) {
    //             al_draw_pixel(264 + 8 * ntrecordsize + x1 * ntrecordsize + x, 1 + 8 * ntrecordsize + y1 * ntrecordsize + y, al_map_rgb(PPU_RAM[addr] + 32, PPU_RAM[addr] + 32, PPU_RAM[addr] + 32));
    //         }
    //     }
    // }
    // 
    // for (int y = 0; y < 16; y++) {
    //     for (int x = 0; x < 32; x++) {
    //         ppu_draw_sprite_grayscale(256 + 8 + x * 8, 64 + y * 8, x * 16 + y * 512, false, false);
    //     }
    // }

    al_flip_display();

    // Resetting the pixel data
    ppu_behind_background_sprite_pixels_number = ppu_background_pixels_number = ppu_sprite_pixels_number = 0;
    memset(ppu_behind_background_sprite_pixels, 0, 61440 * sizeof(ALLEGRO_VERTEX));
    memset(ppu_background_pixels, 0, 61440 * sizeof(ALLEGRO_VERTEX));
    memset(ppu_sprite_pixels, 0, 61440 * sizeof(ALLEGRO_VERTEX));
}