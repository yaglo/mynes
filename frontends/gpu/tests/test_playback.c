/* Exercise exclusive core ownership, mailbox bounds, and ROM replacement. */
#include "playback.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"Playback FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)

static bool next(Playback *p, PlaybackFrame *frame) {
    Uint64 timeout=SDL_GetTicks()+2000;
    do {
        if(playback_read(p,frame)) return true;
        SDL_Delay(1);
    } while(SDL_GetTicks()<timeout);
    return false;
}

int main(void) {
    if(!SDL_Init(SDL_INIT_AUDIO)) return 1;
    NES *nes=calloc(1,sizeof(*nes));
    uint8_t *prg=calloc(32768,1), *chr=calloc(8192,1);
    prg[0]=0x4c; prg[1]=0; prg[2]=0x80; // JMP $8000, a stable synthetic cartridge.
    prg[0x7ffc]=0; prg[0x7ffd]=0x80;
    nes_init(nes); nes_load_mapper(nes,0,prg,32768,chr,8192,0); nes_reset(nes);
    SDL_AudioSpec spec={.format=SDL_AUDIO_F32,.channels=1,.freq=44100};
    SDL_AudioStream *stream=SDL_CreateAudioStream(&spec,&spec);
    Playback *p=playback_create(nes,NULL,NULL,stream,0,0);
    CHECK(p!=NULL);
    if(p) {
        PlaybackControls controls={.analog=nes->apu.analog};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        PlaybackFrame first,latest;
        CHECK(next(p,&first));
        SDL_Delay(150); // Main thread stalls; emulation/audio keep running.
        CHECK(next(p,&latest)); CHECK(latest.number>first.number+2);
        playback_pause(p);
        CHECK(SDL_GetAudioStreamQueued(stream)==0);
        CHECK(!playback_read(p,&latest));
        uint64_t tick=nes->ppu.next_dot_master_tick;
        SDL_Delay(40); CHECK(nes->ppu.next_dot_master_tick==tick);
        // The caller can replace cartridge memory only after pause acknowledges.
        uint8_t *replacement=malloc(32768); memcpy(replacement,prg,32768);
        nes_load_mapper(nes,0,replacement,32768,chr,8192,0); nes_reset(nes);
        free(prg); prg=replacement;
        controls.region=1; controls.controller=0x81;
        playback_controls(p,&controls); playback_resume(p);
        CHECK(next(p,&latest)); CHECK(latest.number>first.number);
        playback_pause(p);
        CHECK(nes->controller[0]==0x81);
        CHECK(nes->ppu.region==PPU_REGION_PAL);
        CHECK(SDL_GetAudioStreamQueued(stream)==0);
        playback_destroy(p);
    }
    SDL_DestroyAudioStream(stream); free(nes); free(prg); free(chr); SDL_Quit();
    printf("Playback ownership regressions: %d failures\n",failures);
    return failures ? 1 : 0;
}
