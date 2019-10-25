#include <allegro5/allegro.h>
#include <allegro5/allegro_primitives.h>

#ifndef FCE_INTERNAL_H
#define FCE_INTERNAL_H

ALLEGRO_DISPLAY *fce_display;
ALLEGRO_EVENT_QUEUE *fce_event_queue;
ALLEGRO_TIMER *fce_timer = NULL;

#endif