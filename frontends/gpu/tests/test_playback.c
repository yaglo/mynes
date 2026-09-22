/* Exercise exclusive core ownership, picture queue bounds, low-latency
 * queueing, and ROM replacement. */
#define _POSIX_C_SOURCE 200809L
#include "playback.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"Playback FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)

/* Console visitors for playback_with_console. The delay is longer than two
 * frame periods, so a worker that ignored the hold would advance the frame. */
typedef struct { uint64_t frame_at_entry, frame_at_exit; bool complete; } Visit;
static void visit_console(NES *nes, void *user) {
    Visit *v = user;
    v->complete = nes->ppu.frame_complete;
    v->frame_at_entry = nes->ppu.frame;
    nes->ram[42] = 0x5a;
    SDL_Delay(40);
    v->frame_at_exit = nes->ppu.frame;
}
static void read_ram(NES *nes, void *user) { *(uint8_t *)user = nes->ram[42]; }

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
        controls.region=1; controls.controller[0]=0x81; controls.controller[1]=0x42;
        playback_controls(p,&controls); playback_resume(p);
        CHECK(next(p,&latest)); CHECK(latest.number>first.number);
        playback_pause(p);
        CHECK((latest.codes[120*256+128]&63)==0x21); // new cartridge reached the screen
        CHECK(nes->controller[0]==0x81 && nes->controller[1]==0x42); // both ports, independently
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
        CHECK(playback_queued(p)==3); /* low latency off: three pictures ahead */
        playback_pause(p);
        CHECK(!playback_read(p,&frame));
        playback_resume(p);
        SDL_Delay(100);
        playback_destroy(p);
    }
    /* Low latency under matched-refresh hold: the worker queues one picture
     * and waits for the renderer to take it, so the picture shown was
     * emulated right after the previous one was read. Consecutive numbers
     * prove no phase was dropped; the queue never exceeds one while the
     * renderer stalls, and the worker resumes as soon as it is read. */
    p=playback_create(nes,NULL,NULL,stream,0,0);
    CHECK(p!=NULL);
    if(p) {
        PlaybackControls controls={.analog=nes->apu.analog,.display_paced=true,.display_hz=60,.low_latency=true};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        SDL_Delay(150); /* Renderer stalled. */
        unsigned peak=0;
        for(int i=0;i<20;i++) { unsigned queued=playback_queued(p); if(queued>peak) peak=queued; SDL_Delay(5); }
        CHECK(peak==1);
        PlaybackFrame frame;
        unsigned previous=0;
        for(unsigned n=1;n<=6;n++) {
            CHECK(next(p,&frame));
            if(previous) CHECK(frame.number==previous+1);
            previous=frame.number;
            Uint64 timeout=SDL_GetTicks()+200;
            while(playback_queued(p)==0 && SDL_GetTicks()<timeout) SDL_Delay(1);
            CHECK(playback_queued(p)==1); /* resumed promptly after the read */
            SDL_Delay(25);
            CHECK(playback_queued(p)==1); /* and stopped again at one */
        }
        /* Switching low latency off mid-session releases the worker to refill
         * the FIFO without a pause/resume. */
        controls.low_latency=false;
        playback_controls(p,&controls);
        SDL_Delay(150);
        CHECK(playback_queued(p)==3);
        playback_pause(p); playback_destroy(p);
    }
    /* A window macOS no longer composites hands out drawables at about
     * 120 Hz. Reads that fast must not clock the emulation above the display
     * rate, with low latency or with the three-picture FIFO. */
    for(int low_latency=1;low_latency>=0;low_latency--) {
        p=playback_create(nes,NULL,NULL,stream,0,0);
        CHECK(p!=NULL);
        if(!p) continue;
        PlaybackControls controls={.analog=nes->apu.analog,.display_paced=true,.display_hz=60,
            .low_latency=low_latency};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        PlaybackFrame frame;
        CHECK(next(p,&frame));
        unsigned first=frame.number,last=first;
        Uint64 end=SDL_GetTicks()+1000;
        while(SDL_GetTicks()<end) {
            if(playback_read(p,&frame)) last=frame.number;
            SDL_Delay(8);
        }
        /* 60 a second, plus the queue and one frame started early. */
        CHECK(last-first<=66);
        playback_pause(p); playback_destroy(p);
    }
    /* Low latency without display pacing: the emulation clock runs freely and
     * a renderer that stalled gets the newest picture, discarding the stale
     * ones behind it. The frame limit makes the newest number exact. */
    p=playback_create(nes,NULL,NULL,stream,12,0);
    CHECK(p!=NULL);
    if(p) {
        PlaybackControls controls={.analog=nes->apu.analog,.low_latency=true};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        SDL_Delay(400); /* 12 frames take about 200 ms; the ring holds 10..12. */
        CHECK(playback_queued(p)==3);
        PlaybackFrame frame;
        CHECK(next(p,&frame)); CHECK(frame.number==12);
        CHECK(playback_queued(p)==0);
        CHECK(!playback_read(p,&frame));
        playback_pause(p); playback_destroy(p);
    }
    /* Fast-forward runs more emulated frames per wall-clock interval than
     * normal speed, never queues audio while active, and hands the stream back
     * to the resampler afterwards. Frame counts come from the worker itself. */
    p=playback_create(nes,NULL,NULL,stream,0,0);
    CHECK(p!=NULL);
    if(p) {
        PlaybackControls controls={.analog=nes->apu.analog,.speed=1};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        PlaybackFrame frame;
        CHECK(next(p,&frame));
        unsigned normal_start=frame.number;
        SDL_Delay(400);
        CHECK(next(p,&frame));
        unsigned normal_frames=frame.number-normal_start;
        playback_pause(p);
        CHECK(SDL_GetAudioStreamQueued(stream)==0);
        controls.speed=8;
        playback_controls(p,&controls); playback_resume(p);
        CHECK(next(p,&frame));
        unsigned fast_start=frame.number;
        bool queued_while_fast=false;
        for(int i=0;i<8;i++) {
            SDL_Delay(50);
            if(SDL_GetAudioStreamQueued(stream)!=0) queued_while_fast=true;
        }
        CHECK(next(p,&frame));
        unsigned fast_frames=frame.number-fast_start;
        CHECK(!queued_while_fast);
        CHECK(fast_frames>normal_frames*3/2); /* 8x requested; the host decides how close it gets */
        controls.speed=1;
        playback_controls(p,&controls);
        Uint64 timeout=SDL_GetTicks()+2000;
        while(SDL_GetAudioStreamQueued(stream)==0 && SDL_GetTicks()<timeout) SDL_Delay(1);
        CHECK(SDL_GetAudioStreamQueued(stream)>0); /* audio flows again after fast-forward */
        playback_pause(p);
        playback_destroy(p);
        printf("Playback speed: %u frames at 1x, %u at 8x over equal intervals\n",normal_frames,fast_frames);
    }
    /* A main-thread visit runs between frames, holds the next frame back for
     * as long as it lasts and may change the console; playback then goes on. */
    p=playback_create(nes,NULL,NULL,stream,0,0);
    CHECK(p!=NULL);
    if(p) {
        PlaybackControls controls={.analog=nes->apu.analog};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        PlaybackFrame frame;
        CHECK(next(p,&frame));
        unsigned before=frame.number;
        Visit v={0};
        playback_with_console(p,visit_console,&v);
        CHECK(v.complete);                          /* never inside nes_run_frame */
        CHECK(v.frame_at_exit==v.frame_at_entry);   /* no frame ran during the visit */
        uint8_t seen=0;
        playback_with_console(p,read_ram,&seen);
        CHECK(seen==0x5a);
        SDL_Delay(60);
        CHECK(next(p,&frame)); CHECK(frame.number>before);
        playback_pause(p); playback_destroy(p);
    }
    /* A restart discards the pictures queued before it: the next picture read
     * was produced afterwards. Matched-refresh hold fills the queue deterministically. */
    p=playback_create(nes,NULL,NULL,stream,0,0);
    CHECK(p!=NULL);
    if(p) {
        PlaybackControls controls={.analog=nes->apu.analog,.display_paced=true,.display_hz=60};
        audio_chain_init_preset(&controls.audio,0,0,0);
        playback_controls(p,&controls); playback_resume(p);
        SDL_Delay(150);
        PlaybackFrame frame;
        CHECK(next(p,&frame));
        unsigned k=frame.number;
        SDL_Delay(100);            /* k+1..k+3 queued, worker blocked on backpressure */
        playback_restart(p);
        CHECK(next(p,&frame)); CHECK(frame.number==k+4);
        CHECK(SDL_GetAudioStreamQueued(stream)>=0);
        playback_pause(p); playback_destroy(p);
    }
    /* Replay must hold buttons across frames and release at the exact event.
     * Captures and reviews are compared bit-for-bit, so the paired-frame
     * FIFO must survive a low-latency request: frames arrive in order. */
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
            PlaybackControls controls = {.analog = nes->apu.analog, .low_latency = true};
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
