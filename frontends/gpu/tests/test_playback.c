/* Exercise exclusive core ownership, picture queue bounds, and ROM replacement. */
#define _POSIX_C_SOURCE 200809L
#include "playback.h"
#include <unistd.h>
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
        const uint8_t paint[]={0xa9,0x3f,0x8d,0x06,0x20,0xa9,0x00,0x8d,0x06,0x20,
                               0xa9,0x21,0x8d,0x07,0x20,
                               0xa9,0x00,0x8d,0x06,0x20,0x8d,0x06,0x20,
                               0xa9,0x08,0x8d,0x01,0x20,0x4c,0x1c,0x80};
        memcpy(replacement,paint,sizeof(paint));
        nes->cpu.uPC=cpu_entry[0x02]; // KIL: the old reload path could not escape.
        nes_reset(nes);
        for(int i=0;i<16;i++) nes_step(nes);
        CHECK(nes->cpu.reset_pending); // proves reset was never serviced
        nes->dma.oam_active=true; nes->prev_nmi=true; nes->ram[42]=0x73;
        ROM cartridge={.mapper=0,.prg_rom=replacement,.prg_size=32768,.chr_rom=chr,.chr_size=8192};
        playback_load_cartridge(p,&cartridge,1);
        CHECK(nes->cpu.uPC==0 && nes->master_tick==0 && nes->ram[42]==0);
        CHECK(!nes->dma.oam_active && !nes->prev_nmi);
        CHECK(!playback_read(p,&latest));
        free(prg); prg=replacement;
        controls.region=1; controls.controller=0x81;
        playback_controls(p,&controls); playback_resume(p);
        CHECK(next(p,&latest)); CHECK(latest.number>first.number);
        playback_pause(p);
        CHECK((latest.codes[120*256+128]&63)==0x21); // new cartridge reached the screen
        CHECK(nes->controller[0]==0x81);
        CHECK(nes->ppu.region==PPU_REGION_PAL);
        CHECK(SDL_GetAudioStreamQueued(stream)==0);
        nes->ram[42]=0x73; nes->mapper.prg_ram[23]=0x51; nes->ppu.vram[19]=0x62;
        nes->cpu.uPC=cpu_entry[0x02]; nes->dma.oam_active=true;
        tick=nes->master_tick;
        playback_reset_console(p);
        CHECK(nes->prg_rom==replacement && nes->ram[42]==0x73);
        CHECK(nes->mapper.prg_ram[23]==0x51 && nes->ppu.vram[19]==0x62);
        CHECK(nes->master_tick==tick && nes->cpu.reset_pending && !nes->dma.oam_active);
        CHECK(!playback_read(p,&latest));
        playback_resume(p); CHECK(next(p,&latest)); playback_pause(p);
        CHECK(!nes->cpu.reset_pending && nes->cpu.PC>=0x801c && nes->cpu.PC<=0x801f);
        CHECK(nes->apu.audio_callback!=NULL && nes->ppu.region==PPU_REGION_PAL);
        playback_destroy(p);
    }
    /* A short render pause must retain consecutive phases. A longer pause
     * evicts oldest pictures without blocking the independent emulation clock. */
    for(unsigned limit=3;limit<=6;limit+=3) {
        p=playback_create(nes,NULL,NULL,stream,limit,0);
        CHECK(p!=NULL);
        if(!p) continue;
        PlaybackControls controls={.analog=nes->apu.analog};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        SDL_Delay(250);
        PlaybackFrame frame;
        for(unsigned n=limit-2;n<=limit;n++) {
            CHECK(next(p,&frame)); CHECK(frame.number==n);
        }
        CHECK(!playback_read(p,&frame));
        playback_pause(p); playback_destroy(p);
    }
    /* Matched-refresh hold must not throw away alternating phases while the
     * renderer waits for vblank. A full queue still permits pause and shutdown. */
    p=playback_create(nes,NULL,NULL,stream,0,0);
    CHECK(p!=NULL);
    if(p) {
        PlaybackControls controls={.analog=nes->apu.analog,.display_paced=true,.display_hz=60};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        SDL_Delay(150);
        PlaybackFrame frame;
        for(unsigned n=1;n<=8;n++) {
            CHECK(next(p,&frame)); CHECK(frame.number==n);
            SDL_Delay(25); /* Deliberately consume slower than production. */
        }
        SDL_Delay(100); /* Leave the worker blocked on a full queue. */
        playback_pause(p);
        CHECK(!playback_read(p,&frame));
        playback_resume(p);
        SDL_Delay(100);
        playback_destroy(p);
    }
    /* Replay must hold buttons across frames and release at the exact event. */
    char path[] = "/tmp/mynes-playback-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    if (fd >= 0) {
        FILE *file = fdopen(fd, "w");
        fputs("1 80\n3 00\n", file); fclose(file);
        SDL_setenv_unsafe("MYNES_REVIEW_INPUT_SCRIPT", path, 1);
        for (unsigned limit = 2; limit <= 3; limit++) {
            p = playback_create(nes, NULL, NULL, stream, limit, 1);
            CHECK(p != NULL);
            if (!p) continue;
            PlaybackControls controls = {.analog = nes->apu.analog};
            audio_chain_init_preset(&controls.audio, 0, 0, 0);
            playback_controls(p, &controls); playback_resume(p);
            PlaybackFrame frame;
            for (unsigned n = 1; n <= limit; n++) {
                CHECK(next(p, &frame)); CHECK(frame.number == n);
            }
            playback_pause(p);
            CHECK(nes->controller[0] == (limit == 2 ? 0x80 : 0));
            playback_destroy(p);
        }
        file = fopen(path, "w");
        fputs("3 01\n2 00\n", file); fclose(file);
        p = playback_create(nes, NULL, NULL, stream, 3, 1);
        CHECK(p == NULL);
        if (p) playback_destroy(p);
        SDL_unsetenv_unsafe("MYNES_REVIEW_INPUT_SCRIPT");
        unlink(path);
    }
    SDL_DestroyAudioStream(stream); free(nes); free(prg); free(chr); SDL_Quit();
    printf("Playback ownership regressions: %d failures\n",failures);
    return failures ? 1 : 0;
}
