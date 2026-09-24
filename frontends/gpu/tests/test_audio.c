#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "audio_gpu.h"
#include "audio_sync.h"

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define N 8192
static float source[N], whole[N], split[N], device[N], buzz[N];

static void streaming(SDL_GPUDevice *gpu) {
    for (int i=0; i<N; ++i) source[i] = .2f + .15f*sinf(i * .1424758573f) + ((i % 313) == 0 ? .2f : 0);
    for (int i=0; i<N; ++i) buzz[i] = .003f*sinf(i * .0085f) + ((i % 735) < 60 ? .004f : 0);
    for (int speaker=0; speaker<6; ++speaker) {
        AudioChain c;
        audio_chain_init_preset(&c, speaker % 4, speaker, speaker % 2);
        c.amp_saturation.enabled = true; c.amp_saturation.drive = 1.7f;
        c.rail_clip.enabled = true; c.rail_clip.window = .3f; c.rail_clip.knee = .15f;
        c.rf_deemphasis_us = (speaker & 1) ? 75.0f : 0.0f;
        c.rf_sound.enabled = (speaker & 1) != 0;
        audio_chain_prepare(&c);
        c.psu_hum.enabled = true; c.psu_hum.amplitude = .002f;
        c.psu_hum.harmonic_2 = .4f; c.psu_hum.harmonic_3 = .2f;
        c.noise_floor.enabled = true; c.noise_floor.amplitude = .001f;
        AudioState cpu = {0}, chunked = {0}, metal = {0};
        AudioGPUChain a;
        bool initialized = audio_gpu_init(&a, gpu, &c, "shaders/compute");
        CHECK(initialized); if (!initialized) continue;
        audio_chain_process_aux(&c, &cpu, source, buzz, whole, N);
        int offset=0, block=1;
        while (offset < N) {
            int count = 97 + (block++ * 317) % 1000;
            if (offset + count > N) count = N - offset;
            audio_chain_process_aux(&c, &chunked, source+offset, buzz+offset, split+offset, count);
            CHECK(audio_gpu_process(&a, gpu, &metal, source+offset, buzz+offset, device+offset, count));
            offset += count;
        }
        if (c.rf_sound.enabled) {
            /* The buzz reaches the output: without it the block differs. */
            static float nobuzz[N];
            AudioState plain = {0};
            audio_chain_process(&c, &plain, source, nobuzz, N);
            float diff = 0;
            for (int i=0; i<N; ++i) diff = fmaxf(diff, fabsf(nobuzz[i]-whole[i]));
            CHECK(diff > 1e-4f);
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
        CHECK(audio_gpu_begin(&a,gpu,&c,&fallback,source,NULL,1000));
        CHECK(!audio_gpu_begin(&a,gpu,&c,&fallback,source,NULL,1000));
        audio_chain_process(&c,&fallback,source,device,1000);
        audio_chain_process(&c,&expected,source,whole,1000);
        CHECK(SDL_WaitForGPUFences(gpu,true,&a.pending,1));
        CHECK(audio_gpu_poll(&a,gpu,NULL,NULL)==1);
        CHECK(memcmp(&fallback,&expected,sizeof(fallback))==0);
        CHECK(audio_gpu_process(&a,gpu,&fallback,source,NULL,device,1000));
        audio_chain_process(&c,&expected,source,whole,1000);
        for(int i=0;i<1000;i++) CHECK(fabsf(device[i]-whole[i])<.0001f);
        // Move the live state between backends without a reset or an extra frame.
        CHECK(audio_gpu_process(&a,gpu,&chunked,source,NULL,device,1000));
        audio_chain_process(&c,&cpu,source,whole,1000);
        for(int i=0;i<1000;++i) CHECK(fabsf(device[i]-whole[i]) < .0001f);
        CHECK(!audio_gpu_process(&a,gpu,&metal,source,NULL,device,AUDIO_BLOCK_CAPACITY+1));
        c.bypass_mask = (1u << 11) - 1;
        audio_chain_process_aux(&c,&cpu,source,buzz,whole,1000);
        CHECK(audio_gpu_process(&a,gpu,&metal,source,buzz,device,1000));
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
#include "../../../tools/circuits/golden/nes001_psu.h"

/* The supply's ripple and hum from the PSU block against the PSU deck:
 * every sweep row with the regulator in is met within 10%, the dropout
 * rows (a sharp onset) within a factor of two and with the dropout
 * flagged, and the nominal supply gives the documented jack hum. */
static void supply(void) {
    CHECK(fabsf(AUDIO_NES001_SUPPLY_GAIN_DB-NES001_AUDIO_GOLDEN_SUPPLY_GAIN_120HZ_DB)<0.01f);
    int rows=(int)(sizeof(nes001_psu_golden_rows)/sizeof(nes001_psu_golden_rows[0]));
    for(int i=0;i<rows;++i) {
        const float *r=nes001_psu_golden_rows[i];
        AudioPsuDerived d=audio_psu_derive(r[3],r[0],r[1]*1000,r[2],60,AUDIO_CONSOLE_NES_FRONT);
        float golden_mv=r[4], mv=d.rail_ripple_v*1e3f;
        bool deep=golden_mv>5.0f;
        printf("  PSU %4.0f uF %.1f A %2.0f dB %4.1f VAC: %.3f mV, trough %.2f V (deck %.3f%s)\n",r[0],r[1],r[2],r[3],mv,d.raw_min_v,golden_mv,deep?", dropout":"");
        if(!deep) CHECK(fabsf(mv-golden_mv)<0.1f*golden_mv);
        else CHECK(d.dropout && mv>0.5f*golden_mv && mv<2.0f*golden_mv);
    }
    AudioPsuDerived nominal=audio_psu_derive(0,0,0,0,60,AUDIO_CONSOLE_NES_FRONT);
    printf("nominal NES-001 supply: +5 V %.3f mV, jack %.3f mV at 120 Hz, harmonics %.2f %.3f, trough %.2f V\n",
           nominal.rail_ripple_v*1e3f,nominal.jack_v*1e3f,nominal.harmonic_2,nominal.harmonic_3,nominal.raw_min_v);
    CHECK(!nominal.dropout && nominal.jack_v>3e-4f && nominal.jack_v<6e-4f);
    CHECK(fabsf(nominal.harmonic_2-NES001_PSU_HARMONIC_2)<0.1f);
    CHECK(audio_psu_derive(0,0,0,0,60,AUDIO_CONSOLE_FAMICOM).jack_v==0);
    /* An adaptor sagging to 8 VAC puts the reservoir's trough under the
     * dropout and the raw ripple onto the jack: hum by itself. */
    AudioPsuDerived sagging=audio_psu_derive(8.0f,0,0,0,60,AUDIO_CONSOLE_NES_FRONT);
    CHECK(sagging.dropout && sagging.jack_v>0.1f);
    CHECK(!audio_psu_derive(9.0f,0,0,0,60,AUDIO_CONSOLE_NES_FRONT).dropout);
    /* A dried reservoir at a heavy load drops out on the nominal adaptor. */
    CHECK(audio_psu_derive(10.0f,1000.0f,800.0f,73.0f,60,AUDIO_CONSOLE_NES_FRONT).dropout);
}

/* The RF sound buzz from a frame: a flat half-white picture over the
 * blanking lines makes a field-rate step of the carrier envelope, and the
 * detector passes it at its AM rejection; the modulator's phase step at
 * the picture's edges is a tick sized by the deviation. */
static void rf_buzz(void) {
    AudioChain c; audio_chain_init_preset(&c,AUDIO_CONSOLE_NES_FRONT,AUDIO_SPEAKER_HEADPHONES,0);
    c.rf_sound.enabled=true; c.rf_sound.am_rejection=powf(10,-40.0f/20); c.rf_sound.icpm_rad=0;
    static uint16_t codes[256*240];
    for(int i=0;i<256*240;++i) codes[i]=0x20;   /* $20: luma row 2 flat, level (1100-312)/788 = 1 */
    AudioVideoFrame f={0};
    audio_video_frame_from_codes(&f,codes,0);
    CHECK(f.lines==262 && fabsf(f.level[0]-1.0f)<1e-4f && f.level[250]==0);
    for(int i=0;i<256*240;++i) codes[i]=0x1D;   /* $1D: black */
    audio_video_frame_from_codes(&f,codes,0);
    CHECK(fabsf(f.level[100])<1e-4f);
    for(int i=0;i<256*240;++i) codes[i]=(i%2) ? 0x30 : 0x0F;  /* white and black alternate: 0.5 */
    audio_video_frame_from_codes(&f,codes,0);
    CHECK(fabsf(f.level[10]-0.5f)<1e-3f);
    int count=735; static float aux[735];
    audio_chain_rf_buzz(&c,&f,aux,count);
    float env_pic=0.75f-0.625f*0.5f, env_blank=0.75f, mean=(240*env_pic+22*env_blank)/262;
    float k=powf(10,-40.0f/20)*0.7f;
    float expect_pic=k*(env_pic/mean-1), expect_blank=k*(env_blank/mean-1);
    /* The line-rate tone rides on top; compare the mean over a few lines. */
    double pic=0, blank=0; int np=0, nb=0;
    for(int n=0;n<count;++n) { int l=(int)((long long)n*262/count); if(l>20 && l<200) {pic+=aux[n]; ++np;} if(l>=245 && l<260) {blank+=aux[n]; ++nb;} }
    pic/=np; blank/=nb;
    printf("RF buzz: picture %.5f (expected %.5f), blanking %.5f (expected %.5f)\n",pic,expect_pic,blank,expect_blank);
    CHECK(fabs(pic-expect_pic)<2e-4 && fabs(blank-expect_blank)<2e-4);
    CHECK(expect_blank>3e-3f);  /* an audible buzz at 40 dB rejection, -47 dB re unit */
    c.rf_sound.icpm_rad=5.0f*3.14159265f/180; c.rf_sound.am_rejection=0;
    f.line_phase=0; audio_chain_rf_buzz(&c,&f,aux,count);
    float tick=0; for(int n=0;n<count;++n) tick=fmaxf(tick,fabsf(aux[n]));
    float expect_tick=0.7f*(5.0f*3.14159265f/180*0.5f*15734.264f)/(2*3.14159265f*25000);
    printf("RF ICPM tick: %.5f (expected %.5f)\n",tick,expect_tick);
    CHECK(fabsf(tick-expect_tick)<1e-4f);
}
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
    /* The set's 75 us sound de-emphasis on RF: -3 dB at 2.12 kHz relative to 100 Hz. */
    {
        AudioChain c; AudioState s={0};
        audio_chain_init_preset(&c,AUDIO_CONSOLE_NES_FRONT,AUDIO_SPEAKER_HEADPHONES,0);
        c.coupling_cap.enabled=c.feedback_network.enabled=c.amp_bandwidth.enabled=c.tv_input_coupling.enabled=false;
        c.rf_deemphasis_us=75.0f; audio_chain_prepare(&c);
        double e[2]={0,0};
        for(int k=0;k<2;++k) { float hz=k ? 2122.0f : 100.0f; double in=0,out=0; s=(AudioState){0};
            for(int b=0;b<32;++b) { for(int i=0;i<1024;++i) source[i]=.2f*sinf(2*M_PI*hz*(b*1024+i)/AUDIO_STREAM_RATE);
                audio_chain_process(&c,&s,source,whole,1024);
                if(b>16) for(int i=0;i<1024;++i) { in+=source[i]*source[i]; out+=whole[i]*whole[i]; } }
            e[k]=sqrt(out/in); }
        float deemph_db=20*log10f((float)(e[1]/e[0]));
        printf("RF de-emphasis at 2122 Hz: %.2f dB\n",deemph_db);
        CHECK(fabsf(deemph_db+3.0f)<0.3f);
    }
    /* The PVM-14L2's AN5278 output capacitor into its 7x5 cm speaker: 199 Hz. */
    {
        AudioChain c; AudioState s={0};
        audio_chain_init_preset(&c,AUDIO_CONSOLE_NES_FRONT,AUDIO_SPEAKER_PVM_14L2,0);
        CHECK(c.speaker_coupling.enabled);
        c.coupling_cap.enabled=c.feedback_network.enabled=c.amp_bandwidth.enabled=c.tv_input_coupling.enabled=false;
        c.speaker.enabled=false; audio_chain_prepare(&c);
        double e[2]={0,0};
        for(int k=0;k<2;++k) { float hz=k ? 199.0f : 5000.0f; double in=0,out=0; s=(AudioState){0};
            for(int b=0;b<32;++b) { for(int i=0;i<1024;++i) source[i]=.2f*sinf(2*M_PI*hz*(b*1024+i)/AUDIO_STREAM_RATE);
                audio_chain_process(&c,&s,source,whole,1024);
                if(b>16) for(int i=0;i<1024;++i) { in+=source[i]*source[i]; out+=whole[i]*whole[i]; } }
            e[k]=sqrt(out/in); }
        float cap_db=20*log10f((float)(e[1]/e[0]));
        printf("PVM-14L2 speaker capacitor at 199 Hz: %.2f dB\n",cap_db);
        CHECK(fabsf(cap_db+3.0f)<0.3f);
    }
    /* The gate's rail window: linear well inside it, held at the rail beyond it. */
    {
        AudioChain c; AudioState s={0};
        audio_chain_init_preset(&c,AUDIO_CONSOLE_NES_FRONT,AUDIO_SPEAKER_HEADPHONES,0);
        c.coupling_cap.enabled=c.feedback_network.enabled=c.amp_bandwidth.enabled=c.tv_input_coupling.enabled=false;
        c.rail_clip.enabled=true; c.rail_clip.window=.75f; c.rail_clip.knee=.15f; audio_chain_prepare(&c);
        for(int i=0;i<1024;++i) source[i]=(i%2 ? 1.f : -1.f)*(i<512 ? .5f : 3.f);
        audio_chain_process(&c,&s,source,whole,1024);
        CHECK(fabsf(whole[100]-source[100])<1e-6f);
        CHECK(fabsf(fabsf(whole[900])-.75f)<.01f);
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
    streaming(gpu); response(); supply(); rf_buzz(); queue();
    SDL_DestroyGPUDevice(gpu); SDL_Quit();
    printf("Audio regressions: %d failures\n",failures);
    return failures ? 1 : 0;
}
