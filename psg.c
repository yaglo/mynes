#include "psg.h"

#include <allegro5/allegro.h>

byte prev_write;
int p = 10;
unsigned char psg_joy1[8];
long x;

inline byte psg_io_read(word address)
{
// Automatic key presses to enter tests
#ifdef CPU_TEST
    x++;
    // SELECT, SELECT, START
    if (x > 2000 && x < 2500) {
        psg_joy1[2] = 1;
    }
    if (x > 3000 && x < 3500) {
        psg_joy1[2] = 1;
    }
    if (x > 4000) {
        psg_joy1[3] = 1;
    }
#endif

#ifdef PPU_TEST
    x++;
    // START
    if (x > 3000 && x < 3500) {
        psg_joy1[3] = 1;
    }
#endif

    // Joystick 1
    if (address == 0x4016) {
        if (p++ < 9) {
            ALLEGRO_KEYBOARD_STATE state;
            al_get_keyboard_state(&state);
            switch (p) {
                case 0: // On / Off
                    return 1;
                case 1: // A
                    return al_key_down(&state, ALLEGRO_KEY_X);
                case 2: // B
                    return al_key_down(&state, ALLEGRO_KEY_Z);
                case 3: // SELECT
                    return al_key_down(&state, ALLEGRO_KEY_A);
                case 4: // START
                    return al_key_down(&state, ALLEGRO_KEY_ENTER) || al_key_down(&state, ALLEGRO_KEY_S);
                case 5: // UP
                    return al_key_down(&state, ALLEGRO_KEY_UP);
                case 6: // DOWN
                    return al_key_down(&state, ALLEGRO_KEY_DOWN);
                case 7: // LEFT
                    return al_key_down(&state, ALLEGRO_KEY_LEFT);
                case 8: // RIGHT
                    return al_key_down(&state, ALLEGRO_KEY_RIGHT);
                default:
                    return 1;
            }
        }
    }
    return 0;
}

inline void psg_io_write(word address, byte data)
{
    if (address == 0x4016) {
        if ((data & 1) == 0 && prev_write == 1) {
            // strobe
            p = 0;
            // for (int i = 0; i < 8; i++) {
            //     // psg_joy1[i] = 0;
            // }
        }
    }
    prev_write = data & 1;
}