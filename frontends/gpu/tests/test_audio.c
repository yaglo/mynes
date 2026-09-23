#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio_gpu.h"
#include "audio_sync.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define N 8192
static float source[N], whole[N], split[N], device[N];

static void streaming(SDL_GPUDevice *gpu) {
    for (int i=0; i<N; ++i) source[i] = .2f + .15f*sinf(i * .1424758573f) + ((i % 313) == 0 ? .2f : 0);
    for (int speaker=0; speaker<6; ++speaker) {
        AudioChain c;
        audio_chain_init_preset(&c, speaker % 4, speaker, speaker % 2);
        c.amp_saturation.enabled = true; c.amp_saturation.drive = 1.7f;
        c.psu_hum.enabled = true; c.psu_hum.amplitude = .002f;
        c.psu_hum.harmonic_2 = .4f; c.psu_hum.harmonic_3 = .2f;
        c.noise_floor.enabled = true; c.noise_floor.amplitude = .001f;
        AudioState cpu = {0}, chunked = {0}, metal = {0};
        AudioGPUChain a;
        bool initialized = audio_gpu_init(&a, gpu, &c, "shaders/compute");
        CHECK(initialized); if (!initialized) continue;
        audio_chain_process(&c, &cpu, source, whole, N);
        int offset=0, block=1;
        while (offset < N) {
            int count = 97 + (block++ * 317) % 1000;
            if (offset + count > N) count = N - offset;
            audio_chain_process(&c, &chunked, source+offset, split+offset, count);
            CHECK(audio_gpu_process(&a, gpu, &metal, source+offset, device+offset, count));
            offset += count;
        }
        float max_error=0;
        for (int i=0; i<N; ++i) {
            CHECK(whole[i] == split[i]);
            CHECK(isfinite(device[i]));
            max_error=fmaxf(max_error,fabsf(device[i]-whole[i]));
        }
        printf("Speaker %d CPU/GPU max error: %.8f\n",speaker,max_error);
        CHECK(max_error < .0001f);
        // A missed playback deadline discards the GPU result, then starts
        // the next block from the CPU fallback state without replaying it.
        AudioState fallback=cpu, expected=cpu;
        CHECK(audio_gpu_begin(&a,gpu,&c,&fallback,source,1000));
        CHECK(!audio_gpu_begin(&a,gpu,&c,&fallback,source,1000));
        audio_chain_process(&c,&fallback,source,device,1000);
        audio_chain_process(&c,&expected,source,whole,1000);
        CHECK(SDL_WaitForGPUFences(gpu,true,&a.pending,1));
        CHECK(audio_gpu_poll(&a,gpu,NULL,NULL)==1);
        CHECK(memcmp(&fallback,&expected,sizeof(fallback))==0);
        CHECK(audio_gpu_process(&a,gpu,&fallback,source,device,1000));
        audio_chain_process(&c,&expected,source,whole,1000);
        for(int i=0;i<1000;i++) CHECK(fabsf(device[i]-whole[i])<.0001f);
        // Move the live state between backends without a reset or an extra frame.
        CHECK(audio_gpu_process(&a,gpu,&chunked,source,device,1000));
        audio_chain_process(&c,&cpu,source,whole,1000);
        for(int i=0;i<1000;++i) CHECK(fabsf(device[i]-whole[i]) < .0001f);
        CHECK(!audio_gpu_process(&a,gpu,&metal,source,device,AUDIO_BLOCK_CAPACITY+1));
        c.bypass_mask = (1u << 9) - 1;
        audio_chain_process(&c,&cpu,source,whole,1000);
        CHECK(audio_gpu_process(&a,gpu,&metal,source,device,1000));
        for(int i=0;i<1000;++i) CHECK(whole[i] == source[i] && device[i] == source[i]);
        audio_gpu_destroy(&a,gpu);
    }
}

static float gain(float hz, int speaker) {
    AudioChain c; AudioState s={0};
    audio_chain_init_preset(&c,AUDIO_CONSOLE_NES_FRONT,speaker,0);
    double input_energy=0, output_energy=0;
    for (int block=0;block<32;++block) {
        for(int i=0;i<1024;++i) source[i]=.2f*sinf(2*M_PI*hz*(block*1024+i)/AUDIO_STREAM_RATE);
        audio_chain_process(&c,&s,source,whole,1024);
        if(block>16) for(int i=0;i<1024;++i) {input_energy+=source[i]*source[i]; output_energy+=whole[i]*whole[i];}
    }
    return sqrt(output_energy/input_energy);
}

#include "../../../tools/circuits/golden/nes001_audio.h"
/* The NES-001 console filter against the ngspice run of its schematic
 * (tools/circuits/nes001_audio.cir, pulse pin, TV input 47k): the chain's
 * coupling, amplifier and TV-input stages on headphones (no speaker) must
 * follow the golden response relative to 1 kHz within 0.5 dB up to 12 kHz.
 * Above that the 44.1 kHz first-order stages and the deck's second-order
 * shunts part ways, so the two highest points are only printed. */
static void response(void) {
    float mid=gain(1000,AUDIO_SPEAKER_HEADPHONES);
    float speaker_high=gain(15000,AUDIO_SPEAKER_SMALL_TV);
    printf("Gain: 1 kHz %.4f, TV 15 kHz %.4f\n",mid,speaker_high);
    CHECK(mid > .75f && mid < 1); CHECK(speaker_high < .1f);
    printf("NES-001 console filter vs ngspice (dB relative to 1 kHz, chain / golden):\n");
    for(unsigned i=0;i<sizeof(nes001_audio_golden_hz)/sizeof(nes001_audio_golden_hz[0]);++i) {
        float hz=nes001_audio_golden_hz[i];
        float db=20*log10f(gain(hz,AUDIO_SPEAKER_HEADPHONES)/mid);
        printf("  %6.0f Hz: %+6.2f / %+6.2f\n",hz,db,nes001_audio_golden_db[i]);
        if(hz<=12000) CHECK(fabsf(db-nes001_audio_golden_db[i])<0.5f);
    }
    AudioChain c; AudioState s={0};
    audio_chain_init_preset(&c,AUDIO_CONSOLE_NES_FRONT,AUDIO_SPEAKER_SMALL_TV,0);
    for(int i=0;i<1024;++i) source[i]=.5f;
    for(int b=0;b<100;++b) audio_chain_process(&c,&s,source,whole,1024);
    for(int i=0;i<1024;++i) CHECK(fabsf(whole[i]) < .00001f);
}

static void queue(void) {
    for(int region=0;region<2;++region) {
        double fps=region ? 50.007 : 60.0988;
        double host_dt=region ? 1/50.0 : 1/60.0;
        double fill=AUDIO_QUEUE_TARGET, produced=0;
        AudioRateCtrl ctrl={0};
        for(int frame=0;frame<36000;++frame) {
            float ratio=audio_sync_ratio(&ctrl,(int)fill,host_dt);
            produced+=AUDIO_STREAM_RATE/fps;
            int count=(int)produced; produced-=count;
            CHECK(!audio_sync_stale((int)fill,count));
            fill+=count-AUDIO_STREAM_RATE*host_dt*ratio;
            CHECK(fill>=0 && fill<AUDIO_QUEUE_LIMIT);
        }
        CHECK(fabs(fill-AUDIO_QUEUE_TARGET)<10);
    }
    CHECK(audio_sync_stale(AUDIO_STREAM_RATE,735));
    CHECK(!audio_sync_stale(0,735));
    // Queue units must remain input mono frames on a stereo 48 kHz device.
    SDL_AudioSpec in={.format=SDL_AUDIO_F32,.channels=1,.freq=AUDIO_STREAM_RATE};
    SDL_AudioSpec out={.format=SDL_AUDIO_F32,.channels=2,.freq=48000};
    SDL_AudioStream *stream=SDL_CreateAudioStream(&in,&out);
    CHECK(stream != NULL);
    if(stream) {
        CHECK(SDL_PutAudioStreamData(stream,source,735*sizeof(float)));
        CHECK(SDL_GetAudioStreamQueued(stream)==735*sizeof(float));
        CHECK(SDL_GetAudioStreamAvailable(stream)>735*sizeof(float));
        CHECK(SDL_ClearAudioStream(stream));
        CHECK(SDL_GetAudioStreamQueued(stream)==0);
        SDL_DestroyAudioStream(stream);
    }
}
int main(void) {
    if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return 1;
    SDL_GPUDevice *gpu=SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV|SDL_GPU_SHADERFORMAT_MSL,true,NULL);
    if(!gpu) {fprintf(stderr,"GPU unavailable: %s\n",SDL_GetError());return 1;}
    streaming(gpu); response(); queue();
    SDL_DestroyGPUDevice(gpu); SDL_Quit();
    printf("Audio regressions: %d failures\n",failures);
    return failures ? 1 : 0;
}
