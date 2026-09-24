/* GPU regressions for the signal receiver and CRT light model. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video_gpu.h"
#include "waveform_gen.h"
#include "gpu_half.h"

extern bool dispatch_beam_profile_public(VideoGPUChain *, SDL_GPUCommandBuffer *);
extern bool dispatch_video_amp_public(VideoGPUChain *, SDL_GPUCommandBuffer *);
extern bool dispatch_temporal_blit_public(VideoGPUChain *, SDL_GPUCommandBuffer *);
extern bool dispatch_gun_current_public(VideoGPUChain *, SDL_GPUCommandBuffer *);
extern bool dispatch_h_blur_rgb_public(VideoGPUChain *, SDL_GPUCommandBuffer *);
extern int test_display_fidelity(SDL_GPUDevice *gpu);
extern int test_crt_load(SDL_GPUDevice *gpu);
extern int test_osd(SDL_GPUDevice *gpu);
extern int test_vhs_fidelity(SDL_GPUDevice *gpu);
extern int test_encoder(SDL_GPUDevice *gpu);
extern int test_border(SDL_GPUDevice *gpu);
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

/* Exercise the FIR workgroup halo, both mirror rules, partial groups, even
 * and asymmetric taps, and the direct path for long/decimating filters. */
static void fir_boundaries(SDL_GPUDevice *gpu) {
    const struct { int count, line, taps, decimation; } cases[] = {
        {1,0,65,1}, {17,0,31,1}, {85,17,65,1}, {765,255,64,1},
        {771,257,65,1}, {1021,0,32,1}, {5456,2728,49,1},
        {6820,3410,63,1}, {1021,0,97,1}, {771,257,31,3},
        {1021,0,65,4}, {513,0,1,1}, {513,0,0,1}
    };
    for (unsigned c=0;c<sizeof(cases)/sizeof(cases[0]);c++) {
        int n=cases[c].count, spl=cases[c].line, nt=cases[c].taps;
        int dec=cases[c].decimation, out_n=(n+dec-1)/dec;
        float *in=malloc(n*sizeof(float)), *out=malloc(out_n*sizeof(float));
        float taps[97];
        for(int i=0;i<n;i++) in[i]=(float)((i*37+11)%251-125)/128;
        for(int k=0;k<97;k++) taps[k]=(float)((k*13+7)%31-15)/128;
        SignalChain sc; CHECK(chain_init(&sc,gpu,n,"shaders/compute"));
        int ti=chain_upload_taps(&sc,gpu,taps,nt ? nt : 1);
        GpuFIRParams p={n,out_n,nt,dec,spl};
        int st=chain_add_stage(&sc,"FIR boundaries",CHAIN_KERNEL_FIR,&p,sizeof(p),(out_n+255)/256,1);
        sc.stages[st].taps_index=ti;
        CHECK(chain_upload_input(&sc,gpu,in,n*sizeof(float)));
        CHECK(chain_run(&sc,gpu));
        CHECK(chain_download_output(&sc,gpu,out,out_n*sizeof(float)));
        for(int i=0;i<out_n;i++) {
            int base=i*dec, start=spl ? base/spl*spl : 0, end=spl ? start+spl : n;
            float expected=0;
            for(int k=0;k<nt;k++) {
                int idx=base-nt/2+k;
                if(idx<start) idx=2*start-idx;
                if(idx>=end) idx=2*end-idx-1;
                if(idx<start) idx=start;
                if(idx>=end) idx=end-1;
                expected+=taps[k]*in[idx];
            }
            /* Binary-fraction fixtures make the dot products exact, so even
             * a one-bit difference exposes indexing or accumulation errors. */
            CHECK(out[i]==expected);
        }
        chain_destroy(&sc,gpu);free(in);free(out);
    }
}

/* Line-oriented kernels must cover multiple small workgroups, including the
 * last partial group, and keep independent per-line history between frames. */
static void scanline_dispatch(SDL_GPUDevice *gpu) {
    for(int region=0;region<2;region++) {
        int spp=region ? 10 : 8, width=341*spp, lines=region ? 312 : 262, n=width*lines;
        float *in=malloc(n*sizeof(float)), *out=malloc(n*sizeof(float));
        float *carry=calloc(lines*2,sizeof(float));
        SignalChain sc; CHECK(chain_init(&sc,gpu,n,"shaders/compute"));
        sc.samples_per_line=width;
        GpuRCFilterParams rp={.a=.5f,.b=.5f,.total_count=n,.samples_per_line=width,.num_lines=lines};
        int rc=chain_add_stage(&sc,"Line recurrence",CHAIN_KERNEL_RC_FILTER,&rp,sizeof(rp),1,1);
        for(int i=0;i<n;i++) in[i]=(float)((i*37+11)%127)/128;
        CHECK(chain_upload_input(&sc,gpu,in,n*sizeof(float)));CHECK(chain_run(&sc,gpu));
        CHECK(chain_download_output(&sc,gpu,out,n*sizeof(float)));
        for(int y=0;y<lines;y++) {
            float prev=in[y*width];
            for(int x=0;x<width;x++) {
                int i=y*width+x;prev=.5f*prev+.5f*in[i];
                CHECK(fabsf(out[i]-prev)<1e-7f);
            }
        }
        chain_set_stage_enabled(&sc,rc,false);
        /* The keyed top-sync loop against a CPU walk of the same law over
         * three frames: a set switching on takes the first tip outright,
         * a weaker sync releases at the discharge slew line by line and
         * across the frame boundary, and the strong sync back charges by
         * half the excess per line down to the slew limit. */
        GpuAGCParams ap={n,width,lines,.25f,.5f,.5f,.25f,4,3.0f,1.0f,0,0};
        int agc_loop=chain_add_stage(&sc,"Line gain loop",CHAIN_KERNEL_AGC_LOOP,&ap,sizeof(ap),1,1);
        ChainStage *lp=&sc.stages[agc_loop];lp->io_typed=true;lp->ro_count=1;lp->ro[0]=CBR_BUF_SRC;lp->rw_count=1;lp->rw[0]=CBR_AUX0;
        int agc=chain_add_stage(&sc,"Line gain history",CHAIN_KERNEL_AGC,&ap,sizeof(ap),1,1);
        ChainStage *s=&sc.stages[agc];s->io_typed=true;s->ro_count=0;s->rw_count=2;
        s->rw[0]=CBR_BUF_SRC;s->rw[1]=CBR_AUX0;
        float *state=calloc((lines+1)*4,sizeof(float));
        CHECK(gpu_buffer_upload(gpu,sc.aux[0],state,(lines+1)*4*sizeof(float)));
        double gain_db=0; bool fresh=true;
        for(int frame=0;frame<3;frame++) {
            float depth=frame==1 ? .25f : .5f;
            for(int y=0;y<lines;y++) for(int x=0;x<width;x++)
                in[y*width+x]=(x>=8*spp && x<20*spp) ? -depth :
                             (x>=46*spp && x<49*spp) ? 0 : (float)(y%13+1)/16;
            CHECK(chain_upload_input(&sc,gpu,in,n*sizeof(float)));CHECK(chain_run(&sc,gpu));
            CHECK(chain_download_output(&sc,gpu,out,n*sizeof(float)));
            CHECK(gpu_buffer_download(gpu,sc.aux[0],state,(lines+1)*4*sizeof(float)));
            printf("AGC loop frame %d: line 0 gain %.5f depth %.4f sigma %.4f, line %d gain %.5f, state %.3f dB flag %.0f\n",
                   frame,state[0],state[1],state[2],lines-1,state[(lines-1)*4],state[lines*4],state[lines*4+3]);
            for(int y=0;y<lines;y++) {
                double measured=20*log10(depth/ap.target_level);
                if(fresh) { gain_db=-measured; fresh=false; }
                double excess=measured+gain_db;
                if(excess>0) gain_db-=fmin(ap.attack_coeff*excess,ap.attack_slew_db);
                else gain_db+=fmin(ap.release_coeff,-excess);
                gain_db=fmin(fmax(gain_db,20*log10(ap.min_gain)),20*log10(ap.max_gain));
                float expected=(float)pow(10,gain_db/20);
                CHECK(fabsf(state[y*4]-expected)<5e-4f);   /* float dB arithmetic over hundreds of lines */
                for(int x=0;x<width;x+=37) CHECK(fabsf(out[y*width+x]-in[y*width+x]*state[y*4])<1e-6f);
            }
            if(frame==0) CHECK(fabsf(state[0]-.5f)<1e-4f && fabsf(state[(lines-1)*4]-.5f)<1e-4f);
            if(frame==1) CHECK(state[(lines-1)*4]>state[0]);
            if(frame==2) CHECK(state[(lines-1)*4]<state[0] && state[0]<1.0f);
        }
        chain_destroy(&sc,gpu);free(in);free(out);free(carry);free(state);
    }
}

static void temporal(SDL_GPUDevice *gpu, VideoGPUChain *v, uint32_t drive, float expected) {
    uint32_t pixels[16 * 16 * 2];
    for (int i = 0; i < 16 * 16 * 2; i++) pixels[i] = drive;
    CHECK(gpu_buffer_upload(gpu, v->buf_beam_rgba, pixels, sizeof(pixels)));
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(cmd != NULL);
    CHECK(dispatch_temporal_blit_public(v, cmd));
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu, v->buf_phosphor_history, pixels, sizeof(pixels)));
    for (int i = 0; i < 16 * 16; i++)
        CHECK(fabsf(gpu_half_to_float((uint16_t)pixels[i*2]) - expected) < 0.001f);
}

static float temporal_output(SDL_GPUDevice *gpu,VideoGPUChain *v) {
    SDL_GPUTransferBufferCreateInfo info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=v->beam_rgba_size};
    SDL_GPUTransferBuffer *b=SDL_CreateGPUTransferBuffer(gpu,&info);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *pass=SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src={.texture=v->tex_beam,.w=v->beam_out_w,.h=v->beam_out_h,.d=1};
    SDL_GPUTextureTransferInfo dst={.transfer_buffer=b,.pixels_per_row=v->beam_out_w,.rows_per_layer=v->beam_out_h};
    SDL_DownloadFromGPUTexture(pass,&src,&dst); SDL_EndGPUCopyPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd)); CHECK(SDL_WaitForGPUIdle(gpu));
    uint16_t *p=SDL_MapGPUTransferBuffer(gpu,b,false);
    float value=p ? gpu_half_to_float(p[0]) : -1;
    if(p) SDL_UnmapGPUTransferBuffer(gpu,b);
    SDL_ReleaseGPUTransferBuffer(gpu,b); return value;
}

/* Probe sidebands through the actual complex GPU IF and envelope detector. */
static void rf_sidebands(SDL_GPUDevice *gpu) {
    enum {N=2048};
    SignalChain sc; CHECK(chain_init(&sc,gpu,N,"shaders/compute"));
    SDL_GPUBuffer *carrier=gpu_buffer_create(gpu,2*N*sizeof(float),GPU_BUF_READWRITE);
    float taps[2*SIGNAL_RF_IF_TAPS];
    signal_design_rf_if(taps,42954540,4.2e6f,1,0);
    int ti=chain_upload_taps(&sc,gpu,taps,2*SIGNAL_RF_IF_TAPS);
    GpuRFIFParams p={N,N,SIGNAL_RF_IF_TAPS,0};
    int st=chain_add_stage(&sc,"IF",CHAIN_KERNEL_RF_IF,&p,sizeof(p),(N+255)/256,1);
    sc.stages[st].external[0]=carrier; sc.stages[st].taps_index=ti;
    float z[2*N],out[N],amplitude[2];
    for(int side=0;side<2;side++) {
        for(int i=0;i<N;i++) {
            double a=2*M_PI*128*i/N;
            z[2*i]=.8f+.01f*cos(a);z[2*i+1]=.01f*sin(a)*(side ? -1 : 1);
        }
        CHECK(gpu_buffer_upload(gpu,carrier,z,sizeof(z)));
        CHECK(chain_run(&sc,gpu));CHECK(chain_download_output(&sc,gpu,out,sizeof(out)));
        double re=0,im=0;
        for(int i=256;i<N-256;i++) { double a=2*M_PI*128*i/N; re+=out[i]*cos(a); im+=out[i]*sin(a); }
        amplitude[side]=(float)(2*hypot(re,im)/(N-512));
    }
    printf("RF IF sideband amplitudes: %.6f %.6f\n",amplitude[0],amplitude[1]);
    CHECK(amplitude[0]>.025f && amplitude[0]<.035f);
    CHECK(amplitude[1]<.001f);
    for(int i=0;i<N;i++) { z[2*i]=.8f; z[2*i+1]=0; }
    CHECK(gpu_buffer_upload(gpu,carrier,z,sizeof(z)));CHECK(chain_run(&sc,gpu));
    CHECK(chain_download_output(&sc,gpu,out,sizeof(out)));
    float gain=.875f/(1+264.0f/788),bias=.125f+gain;
    for(int i=0;i<N;i++) CHECK(fabsf(out[i]-(bias-.8f)/gain)<1e-5f);
    /* The two detectors on a real modulated carrier: a dark grey ($0x row,
     * carrier 0.6) carrying a double-sideband chroma tone of 0.15 at
     * fsc. The Nyquist slope leaves only the upper sideband, so the
     * envelope detector (a diode) reads the chroma's own power as a level
     * shift and a second harmonic, the vestigial-sideband quadrature
     * distortion; the synchronous detector (a PLL VIF) recovers the level
     * and the tone exactly. */
    {
        const double fsc=3579545.0, fs=42954540.0;
        for(int i=0;i<N;i++) { z[2*i]=.6f+.15f*(float)cos(2*M_PI*fsc*i/fs); z[2*i+1]=0; }
        CHECK(gpu_buffer_upload(gpu,carrier,z,sizeof(z)));
        float level[2],tone[2],second[2];
        for(int det=0;det<2;det++) {
            GpuRFIFParams q={N,N,SIGNAL_RF_IF_TAPS,(uint32_t)det};
            chain_update_params(&sc,st,&q,sizeof(q));
            CHECK(chain_run(&sc,gpu));CHECK(chain_download_output(&sc,gpu,out,sizeof(out)));
            double mean=0,re=0,im=0,re2=0,im2=0; int n=0;
            for(int i=256;i<N-256;i++) {
                double a=2*M_PI*fsc*i/fs; float c=bias-out[i]*gain;   /* back to carrier units */
                mean+=c; re+=c*cos(a); im+=c*sin(a); re2+=c*cos(2*a); im2+=c*sin(2*a); n++;
            }
            level[det]=(float)(mean/n); tone[det]=(float)(2*hypot(re,im)/n); second[det]=(float)(2*hypot(re2,im2)/n);
            printf("RF %s detector on dark grey with chroma: level %.4f (sent 0.6), tone %.4f (sent 0.15), second harmonic %.4f\n",
                   det ? "synchronous" : "envelope",level[det],tone[det],second[det]);
        }
        /* The tone reads 3% low on both: the IF's own edge at 3.58 MHz. */
        CHECK(fabsf(level[1]-.6f)<.002f && fabsf(tone[1]-.15f)<.01f && second[1]<.001f);
        CHECK(level[0]>level[1]+.005f && second[0]>.005f);
    }
    SDL_ReleaseGPUBuffer(gpu,carrier);chain_destroy(&sc,gpu);
}

static void rf(SDL_GPUDevice *gpu) {
    SignalChain sc;
    CHECK(chain_init(&sc, gpu, 256, "shaders/compute"));
    GpuRFParams p = {.count=256, .samples_per_line=128, .noise_amplitude=0.1f,
        .sample_rate=42954540.0f, .full_line_samples=2728, .hum_hz=60.0f};
    int stage = chain_add_stage(&sc, "RF test", CHAIN_KERNEL_RF, &p, sizeof(p), 1, 1);
    SDL_GPUBuffer *carrier=gpu_buffer_create(gpu,256*2*sizeof(float),GPU_BUF_READWRITE);
    CHECK(carrier!=NULL);
    sc.stages[stage].external[0]=carrier;
    float unit_tap[]={1,0};
    int ti=chain_upload_taps(&sc,gpu,unit_tap,2);
    GpuRFIFParams ip={256,128,1,0};
    int detector=chain_add_stage(&sc,"Envelope",CHAIN_KERNEL_RF_IF,&ip,sizeof(ip),1,1);
    sc.stages[detector].external[0]=carrier; sc.stages[detector].taps_index=ti;
    float input[256] = {0}, a[256], b[256], repeat[256];
    CHECK(chain_upload_input(&sc, gpu, input, sizeof(input)));
    CHECK(chain_run(&sc, gpu));
    CHECK(chain_download_output(&sc, gpu, a, sizeof(a)));
    p.frame_seed = 1;
    chain_update_params(&sc, stage, &p, sizeof(p));
    CHECK(chain_upload_input(&sc, gpu, input, sizeof(input)));
    CHECK(chain_run(&sc, gpu));
    CHECK(chain_download_output(&sc, gpu, b, sizeof(b)));
    CHECK(memcmp(a, b, sizeof(a)) != 0);
    CHECK(chain_upload_input(&sc, gpu, input, sizeof(input)));
    CHECK(chain_run(&sc, gpu));
    CHECK(chain_download_output(&sc, gpu, repeat, sizeof(repeat)));
    CHECK(memcmp(b, repeat, sizeof(b)) == 0);
    double correlation = 0, energy = 0;
    for (int i = 0; i < 256; i++) { correlation += a[i]*b[i]; energy += a[i]*a[i]; }
    CHECK(fabs(correlation / energy) < 0.2);
    p.noise_amplitude = 0; p.hum_amplitude = 0.2f; p.hum_phase = 0.7f;
    chain_update_params(&sc, stage, &p, sizeof(p));
    CHECK(chain_upload_input(&sc, gpu, input, sizeof(input)));
    CHECK(chain_run(&sc, gpu));
    CHECK(chain_download_output(&sc, gpu, b, sizeof(b)));
    for (int i = 0; i < 256; i++) {
        float t = (float)((i/128)*2728 + i%128) / p.sample_rate;
        CHECK(fabsf(b[i] - 0.2f*sinf(0.7f + 6.28318530718f*60*t)) < 1e-5f);
    }
    chain_destroy(&sc, gpu);
    SDL_ReleaseGPUBuffer(gpu,carrier);
}

/* The snow's level through the real RF stage, IF and detector against the
 * analytic value. Complex white noise of per-axis sigma through the IF's
 * complex taps h leaves sigma * sqrt(sum |h|^2) on each axis; the
 * synchronous detector reads the in-phase axis, so the detected video's
 * sigma is that over the detector's gain, at every level. The envelope
 * detector agrees above a few sigma of carrier and reads the Rician mean
 * and a smaller spread near zero carrier (white on an overmodulated set). */
static void rf_snow_level(SDL_GPUDevice *gpu) {
    enum {LINE=2048, LINES=32, N=LINE*LINES};
    SignalChain sc; CHECK(chain_init(&sc,gpu,N,"shaders/compute"));
    RFModulatorParams link={.modulator_dbmv=9.5f,.link_loss_db=21.4f,.tuner_nf_db=7};
    video_rf_link_budget(&link,42954540.0f);
    GpuRFParams p={.count=N,.samples_per_line=LINE,.noise_amplitude=video_rf_noise_rms(&link),
        .sample_rate=42954540.0f,.full_line_samples=2728,.hum_hz=60.0f};
    int stage=chain_add_stage(&sc,"RF snow",CHAIN_KERNEL_RF,&p,sizeof(p),(N+255)/256,1);
    SDL_GPUBuffer *carrier=gpu_buffer_create(gpu,N*2*sizeof(float),GPU_BUF_READWRITE);
    sc.stages[stage].external[0]=carrier;
    float taps[2*SIGNAL_RF_IF_TAPS];
    signal_design_rf_if(taps,42954540.0f,4.2e6f,1,0);
    double h2=0; for(int k=0;k<2*SIGNAL_RF_IF_TAPS;k++) h2+=(double)taps[k]*taps[k];
    int ti=chain_upload_taps(&sc,gpu,taps,2*SIGNAL_RF_IF_TAPS);
    GpuRFIFParams ip={N,LINE,SIGNAL_RF_IF_TAPS,1};
    int det=chain_add_stage(&sc,"IF",CHAIN_KERNEL_RF_IF,&ip,sizeof(ip),(N+255)/256,1);
    sc.stages[det].external[0]=carrier; sc.stages[det].taps_index=ti;
    const float gain=.875f/(1+264.0f/788),predicted=(float)(p.noise_amplitude*sqrt(h2)/gain);
    float *in=malloc(N*sizeof(float)),*out=malloc(N*sizeof(float));
    const float levels[]={0.0f,0.4f,1.0f};   /* blanking, a mid grey, NES white */
    for(int d=1;d>=0;d--) {
        ip.detector=(uint32_t)d; chain_update_params(&sc,det,&ip,sizeof(ip));
        for(int l=0;l<3;l++) {
            for(int i=0;i<N;i++) in[i]=levels[l];
            p.frame_seed=(uint32_t)(7+l); chain_update_params(&sc,stage,&p,sizeof(p));
            CHECK(chain_upload_input(&sc,gpu,in,N*sizeof(float)));CHECK(chain_run(&sc,gpu));
            CHECK(chain_download_output(&sc,gpu,out,N*sizeof(float)));
            double s=0,s2=0; int n=0;
            for(int y=0;y<LINES;y++) for(int x=100;x<LINE-100;x++) { double v=out[y*LINE+x]; s+=v; s2+=v*v; n++; }
            double mean=s/n, sigma=sqrt(s2/n-mean*mean);
            printf("RF snow at %.1f dB CNR, %s detector, level %.1f: sigma %.5f (analytic %.5f), mean %.4f\n",
                   link.cnr_db,d ? "synchronous" : "envelope",levels[l],sigma,predicted,mean);
            CHECK(fabs(sigma/predicted-1)<0.05);
            CHECK(fabs(mean-levels[l])<0.01);
        }
    }
    free(in);free(out);SDL_ReleaseGPUBuffer(gpu,carrier);chain_destroy(&sc,gpu);
}

/* Exercise the actual DAC -> RF -> receiver path over multiple colour-phase
 * cycles. A constant grey input isolates snow from legitimate dot crawl. */
static void rf_temporal_continuity(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp,SIGNAL_REGION_NTSC);
    video_chain_init_preset(&c,VIDEO_CONN_RF,VIDEO_COMB_NONE,SIGNAL_REGION_NTSC);
    c.rf.noise_floor_dbm=-45; c.rf.carrier_level_dbm=-20;
    c.tv.noise_level=0; c.console_psu_hum=0; c.cable.shield_effectiveness=1;
    CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
    CHECK(video_gpu_upload_signal_table(&v,gpu,(float *)sp.table,NULL,SIG_TABLE_ENTRIES,SIG_TABLE_STRIDE));
    uint16_t codes[256*240]; for(int i=0;i<256*240;i++) codes[i]=0x10;
    float *rgb=malloc(v.rgb_size);
    enum {FRAMES=48,POINTS=2048};
    float (*samples)[POINTS]=calloc(FRAMES,sizeof(*samples));
    /* Include frame numbers beyond float's exact-integer range. */
    v.signal_frame_counter=16777216u;
    for(int f=0;f<FRAMES;f++) {
        int phase=signal_frame_phase(&sp,(unsigned)f);
        video_gpu_set_demod(&v,(phase+sp.demod_rotate)*6.28318530718f/12,6.28318530718f/12);
        CHECK(video_gpu_process_full(&v,gpu,codes,phase,sp.phase_line_adv,0,rgb));
        CHECK(((GpuRFParams *)v.sig_chain.stages[v.stage_rf].params)->frame_seed==16777216u+(unsigned)f);
        double mean=0;
        for(int i=0;i<POINTS;i++) {
            int x=96+(i%64)*28,y=40+(i/64)*5;
            samples[f][i]=rgb[decode_window_rgb_index(&v.window,y,x)+1]; mean+=samples[f][i];
        }
        mean/=POINTS;
        for(int i=0;i<POINTS;i++) samples[f][i]-=(float)mean;
    }
    double max_corr=0;
    for(int lag=1;lag<=12;lag++) {
        double xy=0,xx=0,yy=0;
        for(int f=lag;f<FRAMES;f++) for(int i=0;i<POINTS;i++) {
            double x=samples[f][i],y=samples[f-lag][i];xy+=x*y;xx+=x*x;yy+=y*y;
        }
        double corr=xy/sqrt(xx*yy);
        max_corr=fmax(max_corr,fabs(corr));
        CHECK(xx>1e-6 && yy>1e-6);
        CHECK(fabs(corr)<.08);
    }
    printf("RF decoded-grey noise: maximum absolute lag 1..12 correlation %.5f\n",max_corr);
    free(samples);free(rgb);video_gpu_destroy(&v,gpu);
}

static void dac_equivalence(SDL_GPUDevice *gpu, int region) {
    /* The receiver's loops carry state across frames, so each path gets
     * its own chain and sees the same history. */
    SignalPrecompute sp; VideoChain c; VideoGPUChain v, w;
    signal_precompute_init(&sp, region);
    video_chain_init_preset(&c, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE, region);
    CHECK(video_gpu_init(&v, gpu, &c, "shaders/compute", sp.fir_y, sp.fir_y_n, sp.fir_c, sp.fir_c_n, sp.fir_q, sp.fir_q_n));
    CHECK(video_gpu_init(&w, gpu, &c, "shaders/compute", sp.fir_y, sp.fir_y_n, sp.fir_c, sp.fir_c_n, sp.fir_q, sp.fir_q_n));
    CHECK(video_gpu_upload_signal_table(&v, gpu, (float *)sp.table,
        region == SIGNAL_REGION_PAL ? (float *)sp.table_alt : NULL, SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE));
    CHECK(video_gpu_upload_signal_table(&w, gpu, (float *)sp.table,
        region == SIGNAL_REGION_PAL ? (float *)sp.table_alt : NULL, SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE));
    uint16_t indices[256*240];
    for (int i=0;i<256*240;i++) indices[i]=(uint16_t)((i/7)%512);
    size_t count=(size_t)sp.samples_per_line*240, rgb_count=v.rgb_size/sizeof(float);
    float *wave=calloc(count,sizeof(float)), *cpu=calloc(rgb_count,sizeof(float)), *full=calloc(rgb_count,sizeof(float));
    for (unsigned frame=0;frame<3;frame++) {
        waveform_generate(wave,indices,&sp,frame);
        int phase=signal_frame_phase(&sp,frame);
        v.signal_phase_base=phase; w.signal_phase_base=phase;
        video_gpu_set_demod(&v,(phase+sp.demod_rotate)*6.28318530718f/12,6.28318530718f/12);
        video_gpu_set_demod(&w,(phase+sp.demod_rotate)*6.28318530718f/12,6.28318530718f/12);
        CHECK(video_gpu_process(&v,gpu,wave,cpu));
        CHECK(video_gpu_process_full(&w,gpu,indices,phase,sp.phase_line_adv,0,full));
        float max_error=0;
        for(size_t i=0;i<rgb_count;i++) {
            CHECK(isfinite(full[i]));
            max_error=fmaxf(max_error,fabsf(cpu[i]-full[i]));
        }
        CHECK(max_error<0.0001f);
    }
    video_gpu_destroy(&w,gpu);
    free(wave);free(cpu);free(full);
    video_gpu_destroy(&v,gpu);
}

/* Sharpness must act on recovered Y. Probe the actual two GPU passes:
 * a rejected colour carrier must remain rejected, DC must be unchanged,
 * and retained detail must increase. Exercise live tap/enable updates. */
static void luma_sharpness(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);
    video_chain_init_preset(&c, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE, SIGNAL_REGION_NTSC);
    c.tv.luma_bandwidth = 4.2e6f;
    c.tv.aperture_max_db = 6.0f;
    c.tv.h_afc_tau_ms = 1.0f;
    signal_design_fir_notch(sp.fir_y, sp.fir_y_n, 4.2e6f/42954540, 1.0f/12, 1);
    CHECK(video_gpu_init(&v, gpu, &c, "shaders/compute", sp.fir_y, sp.fir_y_n,
                        sp.fir_c, sp.fir_c_n, sp.fir_q, sp.fir_q_n));
    GpuReceiverPLLParams *afc=(GpuReceiverPLLParams *)v.sig_chain.stages[v.stage_receiver_pll].params;
    CHECK(fabsf(afc->h_response-(-expm1f(-1000.0f*v.raster_fmt.samples_per_line /
        signal_region_sample_rate_hz(SIGNAL_REGION_NTSC))))<1e-6f);
    c.tv.h_afc_tau_ms=2.0f;
    video_gpu_update_rc_params(&v);
    CHECK(fabsf(afc->h_response-(-expm1f(-500.0f*v.raster_fmt.samples_per_line /
        signal_region_sample_rate_hz(SIGNAL_REGION_NTSC))))<1e-6f);
    for (int s=0; s<v.sig_chain.num_stages; s++) chain_set_stage_enabled(&v.sig_chain,s,false);
    chain_set_stage_enabled(&v.sig_chain,v.stage_luma_fir,true);
    int w=v.raster_fmt.samples_per_line, count=v.raster_fmt.total_samples;
    float *input=malloc(count*sizeof(float)), *output=malloc(count*sizeof(float));
    float amplitude[2][3], dc[2][3];
    const float frequency[]={.04f,1.0f/12,.35f};
    for (int setting=0; setting<2; setting++) {
        c.tv.luma_peaking=(float)setting;
        CHECK(video_gpu_update_fir_taps(&v,gpu,sp.fir_y,sp.fir_y_n,
                                      sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
        for (int f=0; f<3; f++) {
            for (int i=0; i<count; i++) input[i]=.4f+.1f*cosf(2*M_PI*frequency[f]*(i%w));
            CHECK(chain_upload_input(&v.sig_chain,gpu,input,count*sizeof(float)));
            CHECK(chain_run(&v.sig_chain,gpu));
            CHECK(chain_download_output(&v.sig_chain,gpu,output,count*sizeof(float)));
            /* 1200 samples contain integer cycles of every probe tone. */
            double re=0,im=0,sum=0;
            for (int x=600; x<1800; x++) {
                double p=2*M_PI*frequency[f]*x, y=output[100*w+x];
                re+=y*cos(p); im+=y*sin(p); sum+=y;
            }
            amplitude[setting][f]=(float)(2*hypot(re,im)/1200);
            dc[setting][f]=(float)(sum/1200);
            CHECK(fabsf(dc[setting][f]-.4f)<.00001f);
        }
    }
    printf("Luma sharpness: detail %.6f -> %.6f, rejected carrier %.8f -> %.8f\n",
        amplitude[0][0],amplitude[1][0],amplitude[0][1],amplitude[1][1]);
    CHECK(amplitude[1][0]>amplitude[0][0]*1.05f);
    CHECK(amplitude[0][1]<.00001f && amplitude[1][1]<.00001f);
    CHECK(amplitude[1][2]<.001f);
    c.connection=VIDEO_CONN_RGB;
    CHECK(video_gpu_update_fir_taps(&v,gpu,sp.fir_y,sp.fir_y_n,
                                  sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
    CHECK(!v.sig_chain.stages[v.stage_luma_peaking].enabled);
    free(input); free(output); video_gpu_destroy(&v,gpu);
}

/* The second Y pass must not replace the pre-separation input used by
 * chroma demod. Compare the actual filtered I output across hot sharpness changes
 * for horizontal separation, line comb, separated Y/C and PAL. */
static void sharpness_chroma_routing(SDL_GPUDevice *gpu) {
    for (int route=0; route<4; route++) {
        int region=route==3 ? SIGNAL_REGION_PAL : SIGNAL_REGION_NTSC;
        SignalPrecompute sp; VideoChain c; VideoGPUChain v;
        signal_precompute_init(&sp,region);
        video_chain_init_preset(&c,route==2 ? VIDEO_CONN_SVIDEO : VIDEO_CONN_COMPOSITE,
            route==1 ? VIDEO_COMB_1LINE : VIDEO_COMB_NONE,region);
        c.cable.shield_effectiveness=1; c.tv.noise_level=0;
        CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,
                            sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
        CHECK(video_gpu_upload_signal_table(&v,gpu,(float *)sp.table,
            region==SIGNAL_REGION_PAL ? (float *)sp.table_alt : NULL,SIG_TABLE_ENTRIES,SIG_TABLE_STRIDE));
        uint16_t codes[256*240];
        for(int i=0;i<256*240;i++) codes[i]=(i%256<128) ? 0x16 : 0x21;
        size_t bytes=v.raster_fmt.total_samples*sizeof(float);
        float *before=malloc(bytes),*after=malloc(bytes);
        video_gpu_set_demod(&v,sp.demod_rotate*2*(float)M_PI/12,2*(float)M_PI/12);
        /* The receiver's loops settle over a few frames of the same picture;
         * compare the chroma from a settled state on both sides. */
        for(int warm=0;warm<4;warm++) CHECK(video_gpu_process_full(&v,gpu,codes,0,sp.phase_line_adv,0,NULL));
        ChromaAuxLayout aux=video_chain_chroma_aux_layout(route==1 || route==2);
        CHECK(gpu_buffer_download(gpu,v.sig_chain.aux[aux.i_filt],before,(Uint32)bytes));
        c.tv.luma_peaking=1;
        CHECK(video_gpu_update_fir_taps(&v,gpu,sp.fir_y,sp.fir_y_n,
                                      sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
        for(int warm=0;warm<4;warm++) CHECK(video_gpu_process_full(&v,gpu,codes,0,sp.phase_line_adv,0,NULL));
        CHECK(gpu_buffer_download(gpu,v.sig_chain.aux[aux.i_filt],after,(Uint32)bytes));
        float error=0;
        for(size_t i=0;i<bytes/sizeof(float);i++) error=fmaxf(error,fabsf(after[i]-before[i]));
        CHECK(error<.00001f);
        free(before);free(after);video_gpu_destroy(&v,gpu);
    }
}

/* A stationary composite colour edge may alternate with the carrier phase,
 * but must not develop a slower beat in the decoder itself. Keep both phases
 * distinct: freezing/averaging them would hide a presentation regression. */
static void composite_edge_phase(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp,SIGNAL_REGION_NTSC);
    video_chain_init_preset(&c,VIDEO_CONN_COMPOSITE,VIDEO_COMB_NONE,SIGNAL_REGION_NTSC);
    c.console_psu_hum=0; c.tv.noise_level=0; c.cable.shield_effectiveness=1;
    CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
    CHECK(video_gpu_upload_signal_table(&v,gpu,(float *)sp.table,NULL,SIG_TABLE_ENTRIES,SIG_TABLE_STRIDE));
    uint16_t codes[256*240];
    for(int i=0;i<256*240;i++) codes[i]=(i%256<128) ? 0x0f : 0x21;
    float *rgb=malloc(v.rgb_size), edges[2][48*3];
    float repeat_error=0, phase_difference=0;
    for(unsigned f=0;f<120;f++) {
        int phase=signal_frame_phase(&sp,f);
        video_gpu_set_demod(&v,(phase+sp.demod_rotate)*6.28318530718f/12,6.28318530718f/12);
        CHECK(video_gpu_process_full(&v,gpu,codes,phase,sp.phase_line_adv,0,rgb));
        float *edge=rgb+decode_window_rgb_index(&v.window,120,128*sp.samples_per_pixel-24);
        /* The references come after the receiver's loops have settled into
         * their two-frame steady state. */
        if(f>=8 && f<10) memcpy(edges[f-8],edge,sizeof(edges[0]));
        else if(f>=10) for(int i=0;i<48*3;i++) repeat_error=fmaxf(repeat_error,fabsf(edge[i]-edges[f%2][i]));
    }
    for(int i=0;i<48*3;i++) phase_difference=fmaxf(phase_difference,fabsf(edges[0][i]-edges[1][i]));
    printf("Composite edge: phase difference %.7f, same-phase drift %.7f\n",phase_difference,repeat_error);
    CHECK(phase_difference>.001f); CHECK(repeat_error<.0001f);
    free(rgb); video_gpu_destroy(&v,gpu);
}

/* RGB sources skip reception, but still drive the complete CRT. Check
 * the source against the 2C03's palette, three bits per gun, and verify a
 * phase change cannot introduce composite crawl into RGB. */
static const unsigned short rgb_2c03[64]={
    0x333,0x014,0x006,0x326,0x403,0x503,0x510,0x420,0x320,0x120,0x031,0x040,0x022,0x000,0x000,0x000,
    0x555,0x036,0x027,0x407,0x507,0x704,0x700,0x630,0x430,0x140,0x040,0x053,0x044,0x000,0x000,0x000,
    0x777,0x357,0x447,0x637,0x707,0x737,0x740,0x750,0x660,0x360,0x070,0x276,0x077,0x000,0x000,0x000,
    0x777,0x567,0x657,0x757,0x747,0x755,0x764,0x772,0x773,0x572,0x473,0x276,0x467,0x000,0x000,0x000};
static void rgb_source(SDL_GPUDevice *gpu) {
    for (int region=0;region<2;region++) for(int connection=VIDEO_CONN_COMPONENT;connection<=VIDEO_CONN_DIRECT;connection++) {
        SignalPrecompute sp; VideoChain c; VideoGPUChain v;
        signal_precompute_init(&sp,region);
        video_chain_init_preset(&c,(VideoConnectionType)connection,VIDEO_COMB_NONE,region);
        c.tv.noise_level=0; c.cable.shield_effectiveness=1;
        CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
        CHECK(v.sig_chain.first_stage==v.stage_osd);
        CHECK(!chain_get_stage_enabled(&v.sig_chain,v.stage_raster));
        CHECK(video_gpu_upload_signal_table(&v,gpu,(float *)sp.table,region ? (float *)sp.table_alt : NULL,SIG_TABLE_ENTRIES,SIG_TABLE_STRIDE));
        CHECK(video_gpu_set_beam_params(&v,gpu,32,960,4,.2f,.3f));
        // An RGB source takes each row's luma column as its gun's gain, as
        // the decoder matrix keeps the white drive and contrast there.
        float matrix[3][3]={{1,0,0},{1,0,0},{1,0,0}},bias[3]={0};
        video_gpu_set_color_matrix(&v,matrix,bias);
        uint16_t indices[256*240];
        for(int i=0;i<256*240;i++) indices[i]=(uint16_t)((i/256)%64);
        float *rgb=malloc(v.rgb_size), *other=malloc(v.rgb_size);
        CHECK(video_gpu_process_full(&v,gpu,indices,0,sp.phase_line_adv,0,rgb));
        CHECK(video_gpu_process_full(&v,gpu,indices,8,sp.phase_line_adv,0,other));
        /* Lines 64 to 127 carry codes 0 to 63; NTSC line 0 is in the
         * vertical blanking. */
        for(int code=0;code<64;code++) for(int channel=0;channel<3;channel++) {
            double expected=((rgb_2c03[code]>>(8-4*channel))&15)/7.0;
            size_t i=decode_window_rgb_index(&v.window,64+code,128*sp.samples_per_pixel)+channel;
            CHECK(fabs(rgb[i]-expected)<.0001);
            CHECK(fabs(rgb[i]-other[i])<.000001);
        }
        uint16_t beam[32*960*4];
        CHECK(gpu_buffer_download(gpu,v.buf_beam_rgba,beam,sizeof(beam)));
        float peak=0;
        for(int i=0;i<32*960;i++) peak=fmaxf(peak,gpu_half_to_float(beam[i*4]));
        CHECK(peak>.1f); // A real CRT dispatch, not the raw pixel fallback.
        c.connection=VIDEO_CONN_COMPOSITE; video_gpu_reinit_stages(&v,&c);
        CHECK(v.sig_chain.first_stage==0);
        c.connection=(VideoConnectionType)connection; video_gpu_reinit_stages(&v,&c);
        CHECK(v.sig_chain.first_stage==v.stage_osd);
        CHECK(video_gpu_process_full(&v,gpu,indices,4,sp.phase_line_adv,0,other));
        for(size_t i=0;i<v.rgb_size/sizeof(float);i++) CHECK(fabsf(rgb[i]-other[i])<.00001f);
        free(rgb);free(other);video_gpu_destroy(&v,gpu);
    }
}

static void independent_guns(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    enum { W=8,H=960 };
    c->tv.black_floor=0; c->tv.hum_bar_amplitude=0;
    c->console_psu_hum=0;c->cable.shield_effectiveness=1;
    CHECK(video_gpu_set_beam_params(v,gpu,W,H,4,.12f,.6f));
    float *dx=calloc(W*H*4,sizeof(float)),*dy=calloc(W*H*4,sizeof(float));
    float *rgb=malloc(v->rgb_size);
    uint16_t a[W*H*4],b[W*H*4];
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        int i=(y*W+x)*4; dx[i]=dx[i+1]=dx[i+2]=500;dx[i+3]=1;
        dy[i]=dy[i+1]=dy[i+2]=(y+.5f)*240/H+v->window.picture_row;dy[i+3]=1;
    }
    CHECK(gpu_buffer_upload(gpu,v->buf_deflection_x,dx,W*H*16));
    CHECK(gpu_buffer_upload(gpu,v->buf_deflection_y,dy,W*H*16));
    for(int red=0;red<2;red++) {
        for(unsigned i=0;i<v->rgb_size/sizeof(float);i+=3) {
            rgb[i]=(float)red;rgb[i+1]=.25f;rgb[i+2]=0;
        }
        CHECK(gpu_buffer_upload(gpu,v->buf_rgb2,rgb,v->rgb_size));
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        CHECK(dispatch_beam_profile_public(v,cmd));CHECK(SDL_SubmitGPUCommandBuffer(cmd));
        CHECK(gpu_buffer_download(gpu,v->buf_beam_rgba,red ? b : a,sizeof(a)));
    }
    for(int i=0;i<W*H;i++) CHECK(a[i*4+1]==b[i*4+1]);
    CHECK(a[(H/2*W)*4]!=b[(H/2*W)*4]);
    free(dx);free(dy);free(rgb);
}

static void receiver(SDL_GPUDevice *gpu) {
    for(int region=0;region<2;region++) {
        unsigned spp=region ? 10 : 8, width=341*spp, lines=region ? 312 : 262, count=width*lines;
        SignalChain sc;
        CHECK(chain_init(&sc,gpu,(int)count,"shaders/compute"));
        float *input=calloc(count,sizeof(float)), *raster=calloc(count,sizeof(float));
        for(unsigned i=0;i<256*spp*2;i++) input[i]=0.4f;
        GpuRasterParams ep={.count=count,.full_width=width,.active_width=256*spp,.samples_per_dot=spp,
            .phase_base=3,.line_phase=(float)signal_region_line_phase(region),.region=(uint32_t)region,.lines=lines,
            .sync_level=-264.0f/788.0f};
        int encode=chain_add_stage(&sc,"Raster test",CHAIN_KERNEL_RASTER,&ep,sizeof(ep),(count+255)/256,1);
        ChainStage *e=&sc.stages[encode]; e->io_typed=true;
        e->ro_count=2; e->ro[0]=CBR_BUF_SRC; e->ro[1]=CBR_AUX3;
        e->rw_count=2; e->rw[0]=CBR_BUF_DST; e->rw[1]=CBR_AUX2;
        CHECK(chain_upload_input(&sc,gpu,input,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
        CHECK(chain_download_output(&sc,gpu,raster,count*sizeof(float)));
        CHECK(fabsf(raster[10*spp]+264.0f/788.0f)<1e-6f);
        CHECK(fabsf(raster[width+70*spp]-0.4f)<1e-6f);
        CHECK(raster[46*spp]==0);
        unsigned vsync=region ? 270 : 245;
        CHECK(fabsf(raster[vsync*width+100*spp]+264.0f/788.0f)<1e-6f);
        CHECK(raster[vsync*width+330*spp]==0);
        CHECK(raster[(vsync+3)*width+100*spp]==0);
        CHECK(raster[243*width+100*spp]==0);
        if(region) CHECK(raster[70*spp]==0 && raster[width+65*spp]==0);
        uint32_t rp[]={lines,width,spp,(uint32_t)region};
        int lock=chain_add_stage(&sc,"Receiver test",CHAIN_KERNEL_RECEIVER,rp,sizeof(rp),
                                 gpu_workgroup_count(lines,CHAIN_SCANLINE_WORKGROUP_SIZE),1);
        ChainStage *l=&sc.stages[lock]; l->io_typed=true;
        l->ro_count=2;l->ro[0]=CBR_BUF_SRC;l->ro[1]=CBR_AUX1;l->rw_count=1;l->rw[0]=CBR_AUX0;
        /* The previous frame's loop state: a set that has never locked. */
        float *flywheel=calloc((lines+2)*4,sizeof(float));
        CHECK(gpu_buffer_upload(gpu,sc.aux[1],flywheel,(lines+2)*4*sizeof(float)));
        chain_set_stage_enabled(&sc,encode,false);
        for(unsigned i=0;i<count;i++) raster[i]+=0.125f;
        CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
        float ref[8]; CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        float *all_ref=malloc(lines*4*sizeof(float));
        CHECK(gpu_buffer_download(gpu,sc.aux[0],all_ref,lines*4*sizeof(float)));
        for(unsigned line=0;line<240;line++) CHECK(all_ref[line*4+2]>.3f);
        CHECK(all_ref[vsync*4+2]==-1);
        CHECK(all_ref[(lines-1)*4+2]>.3f);
        free(all_ref);
        for(int line=0;line<2;line++) {
            float expected=(3+line*signal_region_line_phase(region))*6.28318530718f/12;
            CHECK(fabsf(remainderf(ref[line*4]-expected,6.28318530718f))<1e-4f);
            CHECK(fabsf(ref[line*4+1]-0.125f)<1e-5f);
            CHECK(ref[line*4+2]>0.3f);
        }
        /* Snow: white Gaussian noise of 0.14 per sample is what a 15 dB
         * carrier-to-noise RF link leaves on the video. A 50% slicer keyed
         * from the porch must still find the sync edge within a dot and
         * the burst gate on nearly every picture line; a slicer that sets
         * its level from the noisy tip minimum false-triggers inside the
         * pulse, misplaces the burst and clamp gates and kills the colour. */
        {
            float *noisy=malloc(count*sizeof(float)); uint32_t s=12345u;
            for(unsigned i=0;i<count;i++) {
                float u=0; for(int k=0;k<12;k++) { s=s*1664525u+1013904223u; u+=(float)(s>>8)/16777216.0f; }
                noisy[i]=raster[i]+0.14f*(u-6.0f);
            }
            CHECK(chain_upload_input(&sc,gpu,noisy,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
            float *noisy_ref=malloc(lines*4*sizeof(float));
            CHECK(gpu_buffer_download(gpu,sc.aux[0],noisy_ref,lines*4*sizeof(float)));
            int found=0, burst=0; double edge_err=0;
            for(unsigned line=0;line<240;line++) {
                float offset=noisy_ref[line*4+3], amplitude=noisy_ref[line*4+2];
                if(fabsf(offset)<1.0f*spp) { found++; edge_err+=fabs(offset); }
                if(amplitude>0.2f) burst++;
            }
            printf("Receiver under 0.14 noise (%s): sync within a dot on %d of 240 lines (mean edge error %.2f samples), burst on %d\n",
                   region ? "PAL" : "NTSC",found,found ? edge_err/found : 0.0,burst);
            /* The few lines a dot or more out are what a set's flywheel
             * absorbs; before the porch-side slice this found 49 and 27. */
            CHECK(found>=232 && burst>=236 && edge_err/found<1.5);
            /* Twice the noise, a 10 dB link: with the loop locked (the
             * previous frame's state says so) the lines whose edge is lost
             * take their burst and black from the flywheel's position and
             * report it, so the colour survives what the separator cannot. */
            s=777u;
            for(unsigned i=0;i<count;i++) {
                float u=0; for(int k=0;k<12;k++) { s=s*1664525u+1013904223u; u+=(float)(s>>8)/16777216.0f; }
                noisy[i]=raster[i]+0.25f*(u-6.0f);
            }
            flywheel[lines*4+2]=0.3f;   /* state z: a burst has been seen */
            CHECK(gpu_buffer_upload(gpu,sc.aux[1],flywheel,(lines+2)*4*sizeof(float)));
            CHECK(chain_upload_input(&sc,gpu,noisy,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
            CHECK(gpu_buffer_download(gpu,sc.aux[0],noisy_ref,lines*4*sizeof(float)));
            int keyed=0; found=0; burst=0;
            for(unsigned line=0;line<240;line++) {
                float offset=noisy_ref[line*4+3], amplitude=noisy_ref[line*4+2];
                if(offset>4e5f && offset<6e5f) keyed++; else if(fabsf(offset)<1.0f*spp) found++;
                if(amplitude>0.2f) burst++;
            }
            printf("Receiver under 0.25 noise, locked (%s): edge on %d lines, flywheel-gated %d, burst on %d of 240\n",
                   region ? "PAL" : "NTSC",found,keyed,burst);
            /* The lines with an edge more than a dot off are the horizontal
             * loop's to smooth; the gates and the burst are what matter here. */
            CHECK(found+keyed>=200 && burst>=236);
            flywheel[lines*4+2]=0;
            CHECK(gpu_buffer_upload(gpu,sc.aux[1],flywheel,(lines+2)*4*sizeof(float)));
            free(noisy); free(noisy_ref);
            CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
            CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        }
        // A delayed cable moves both sync and burst. The receiver must find
        // the shifted gate, not mistake that delay for a change in hue.
        for(unsigned line=0;line<2;line++) {
            for(unsigned x=width-1;x>=3*spp;x--) raster[line*width+x]=raster[line*width+x-3*spp];
            for(unsigned x=0;x<3*spp;x++) raster[line*width+x]=0.125f;
        }
        CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        for(int line=0;line<2;line++) CHECK(fabsf(ref[line*4+3]-3*spp)<0.01f);
        for(unsigned line=0;line<2;line++) for(unsigned x=29*spp;x<47*spp;x++) raster[line*width+x]=0.125f;
        CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        CHECK(ref[2]<1e-5f && ref[6]<1e-5f);
        chain_set_stage_enabled(&sc,lock,false);
        // Two very different pictures, the same attenuated sync: gain must agree.
        for(unsigned line=0;line<2;line++) for(unsigned x=0;x<width;x++)
            raster[line*width+x]=(x<25*spp) ? -132.0f/788.0f : (x>=65*spp && x<321*spp ? (line ? 0.4f : 0.05f) : 0);
        GpuAGCParams ap={count,width,2,264.0f/788.0f,1,1,0.5f,4,60,1,0,0};
        int agc_loop=chain_add_stage(&sc,"Sync AGC loop test",CHAIN_KERNEL_AGC_LOOP,&ap,sizeof(ap),1,1);
        ChainStage *al=&sc.stages[agc_loop];al->io_typed=true;al->ro_count=1;al->ro[0]=CBR_BUF_SRC;al->rw_count=1;al->rw[0]=CBR_AUX0;
        int agc=chain_add_stage(&sc,"Sync AGC test",CHAIN_KERNEL_AGC,&ap,sizeof(ap),1,1);
        ChainStage *a=&sc.stages[agc];a->io_typed=true;a->ro_count=0;a->rw_count=2;
        a->rw[0]=CBR_BUF_SRC;a->rw[1]=CBR_AUX0;
        float zeros[12]={0};CHECK(gpu_buffer_upload(gpu,sc.aux[0],zeros,sizeof(zeros)));
        CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float)));CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        CHECK(fabsf(ref[0]-2)<1e-5f && fabsf(ref[4]-2)<1e-5f);
        chain_set_stage_enabled(&sc,agc_loop,false);
        chain_set_stage_enabled(&sc,agc,false);
        // Feed noisy burst measurements and a dropout through the loop.
        float measurements[32*4]={0}, locked[33*4]={0};
        float advance=signal_region_line_phase(region)*6.28318530718f/12;
        for(int line=0;line<32;line++) {
            measurements[line*4]=line*advance + (line==0 ? 0 : (line%2 ? .2f : -.2f));
            measurements[line*4+1]=.125f;
            measurements[line*4+2]=line>=10 && line<=13 ? 0 : .3f;
            measurements[line*4+3]=3*spp;
        }
        measurements[20*4+2]=-1; // vertical retrace mutes chroma
        GpuReceiverPLLParams pp={32,width,spp,(uint32_t)region};
        int loop=chain_add_stage(&sc,"PLL test",CHAIN_KERNEL_RECEIVER_PLL,&pp,sizeof(pp),1,1);
        ChainStage *pll=&sc.stages[loop]; pll->io_typed=true;
        pll->ro_count=1;pll->ro[0]=CBR_AUX0;pll->rw_count=1;pll->rw[0]=CBR_AUX1;
        CHECK(gpu_buffer_upload(gpu,sc.aux[0],measurements,sizeof(measurements)));
        CHECK(gpu_buffer_upload(gpu,sc.aux[1],locked,sizeof(locked)));
        CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[1],locked,sizeof(locked)));
        CHECK(locked[13*4+2]<.1f && locked[13*4+2]>0);
        /* No reacquire after retrace: the ACC keeps recovering from the
         * dropout at its 0.05-per-line rate through the vertical line. */
        CHECK(locked[20*4+2]==0 && locked[21*4+2]>.12f && locked[21*4+2]<.14f);
        for(int line=3;line<10;line++) {
            CHECK(fabsf(remainderf(locked[line*4]-line*advance,6.28318530718f))<.09f);
            CHECK(fabsf(locked[line*4+3]-3*spp)<.01f);
        }
        /* Spec-constrained horizontal AFC: 63.2% of a phase step in 1 ms,
         * continuous across dispatches, and held during missing sync/retrace.
         * Colour acquisition must not override the horizontal state. */
        double line_ms=1000.0*width/signal_region_sample_rate_hz(region);
        pp.h_response=(float)-expm1(-line_ms);
        chain_update_params(&sc,loop,&pp,sizeof(pp));
        memset(locked,0,sizeof(locked));
        CHECK(gpu_buffer_upload(gpu,sc.aux[1],locked,sizeof(locked)));
        for(int line=0;line<32;line++) {
            measurements[line*4]=line*advance;
            measurements[line*4+2]=.3f;
            measurements[line*4+3]=3*spp;
        }
        CHECK(gpu_buffer_upload(gpu,sc.aux[0],measurements,sizeof(measurements)));
        for(int frame=0;frame<2;frame++) {
            CHECK(chain_run(&sc,gpu));
            CHECK(gpu_buffer_download(gpu,sc.aux[1],locked,sizeof(locked)));
            for(int line=0;line<32;line++) {
                double expected=3*spp*(1-exp(-(frame*32+line+1)*line_ms));
                CHECK(fabs(locked[line*4+3]-expected)<.00005);
            }
        }
        printf("AFC %s: 1 ms step response and frame continuity verified\n",region ? "PAL" : "NTSC");
        float held=locked[32*4+3];
        for(int line=0;line<32;line++) {
            measurements[line*4+2]=line<16 ? -1 : .3f;
            measurements[line*4+3]=99*spp; // invalid sync must not drag the oscillator
        }
        CHECK(gpu_buffer_upload(gpu,sc.aux[0],measurements,sizeof(measurements)));
        CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[1],locked,sizeof(locked)));
        for(int line=0;line<33;line++) CHECK(fabsf(locked[line*4+3]-held)<.00001f);
        for(int line=0;line<32;line++) {
            measurements[line*4+2]=line<4 ? -1 : .3f;
            measurements[line*4+3]=-2.0f*spp;
        }
        CHECK(gpu_buffer_upload(gpu,sc.aux[0],measurements,sizeof(measurements)));
        CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[1],locked,sizeof(locked)));
        for(int line=0;line<32;line++) {
            double expected=line<4 ? held : -2.0f*spp+(held+2*spp)*exp(-(line-3)*line_ms);
            CHECK(fabs(locked[line*4+3]-expected)<.00005);
        }
        free(input);free(raster);free(flywheel);chain_destroy(&sc,gpu);
    }
}

/* Read back the real Metal/SPIR-V shader against NIDL grille measurements.
 * A sparse 16x16 dispatch samples the nine measurement sites with a 1/8
 * monitor-pixel footprint, without allocating a supersampled full screen. */
static void fw900_measurements(SDL_GPUDevice *gpu, VideoGPUChain *v) {
    enum { W=15360,H=9600,N=W*16,SW=1920,SH=1200 };
    float *rgb=malloc(SW*SH*3*sizeof(float));
    float *dx=calloc(N*4,sizeof(float)),*dy=calloc(N*4,sizeof(float));
    uint16_t *out=calloc(N*4,sizeof(uint16_t));
    SDL_GPUBuffer *input=gpu_buffer_create(gpu,SW*SH*3*sizeof(float),GPU_BUF_READWRITE);
    SDL_GPUBuffer *bx=gpu_buffer_create(gpu,N*16,GPU_BUF_READWRITE);
    SDL_GPUBuffer *by=gpu_buffer_create(gpu,N*16,GPU_BUF_READWRITE);
    SDL_GPUBuffer *result=gpu_buffer_create(gpu,N*8,GPU_BUF_READWRITE);
    const float cm[2][9]={{.56f,.63f,.51f,.52f,.50f,.47f,.58f,.60f,.55f},
                         {.39f,.46f,.54f,.71f,.67f,.68f,.57f,.38f,.48f}};
    const float uniformity[9]={28.7f,30.5f,29.1f,28.2f,31.1f,28.5f,28.3f,30.1f,29.3f};
    // Binary grilles, uniform white, middle grey, black and scaler bars.
    for(int test=0;test<6;test++) {
        bool scaler=test==5;
        int source_h=scaler ? 240 : SH;
        for(int y=0;y<source_h;y++) for(int x=0;x<SW;x++) {
            float value=test==0 ? ((x&1)==0) : test==1 ? ((y&1)==0) :
                        test==3 ? 128.0f/255 : test==4 ? 0 : 1;
            for(int ch=0;ch<3;ch++) rgb[(y*SW+x)*3+ch]=value;
        }
        for(int y=0;y<16;y++) for(int x=0;x<16;x++) {
            int site=y%9,ix=site%3,iy=site/3,i=(y*W+x)*4;
            float px=192+768*ix,py=120+480*iy;
            if(test==0) px+=(x&1);
            if(test==1) py+=(x&1);
            if(scaler) px=x<8 ? 80 : 960;
            // Uniform fields average exactly one full vertical raster period.
            dx[i]=dx[i+1]=dx[i+2]=(px+.5f)/SW*SW;dx[i+3]=1;
            dy[i]=dy[i+1]=dy[i+2]=(py+.5f)/SH*source_h;dy[i+3]=1; // in source lines
        }
        CHECK(gpu_buffer_upload(gpu,input,rgb,SW*source_h*3*sizeof(float)));
        CHECK(gpu_buffer_upload(gpu,bx,dx,N*16));CHECK(gpu_buffer_upload(gpu,by,dy,N*16));
        /* beam_profile's uniforms: the monitor's input fills a decode
         * window of its own, SW samples by source_h rows. */
        struct {
            uint32_t width,out_w,out_h,rows;
            float narrow,wide;uint32_t frame;float hum,bloom,gamma,r,g,b;
            uint32_t monitor,lines;
            int32_t picture_x,picture_row;
            uint32_t picture_w,picture_h;
            float lines_per_row;
        } p={SW,W,test>=2 ? SH : H,1,.2f,.7f,0,0,1.8f,2.4f,0,0,0,1,(uint32_t)source_h,0,0,SW,(uint32_t)source_h,
             (float)source_h/(test>=2 ? SH : H)};
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        SDL_GPUStorageBufferReadWriteBinding rw={.buffer=result};
        SDL_GPUComputePass *pass=SDL_BeginGPUComputePass(cmd,NULL,0,&rw,1);
        CHECK(pass!=NULL);
        SDL_BindGPUComputePipeline(pass,v->sig_chain.pipelines[CHAIN_KERNEL_BEAM].pipeline);
        SDL_GPUBuffer *ro[]={input,bx,by};SDL_BindGPUComputeStorageBuffers(pass,0,ro,3);
        SDL_PushGPUComputeUniformData(cmd,0,&p,sizeof(p));
        SDL_DispatchGPUCompute(pass,1,1,1);SDL_EndGPUComputePass(pass);
        CHECK(SDL_SubmitGPUCommandBuffer(cmd));CHECK(gpu_buffer_download(gpu,result,out,N*8));
        for(int site=0;site<9;site++) {
            float a=gpu_half_to_float(out[(site*W)*4]);
            float b=gpu_half_to_float(out[(site*W+1)*4]);
            if(test<2) {
                float measured=(a-b)/(a+b);
                printf("FW900 %c grille site %d: Cm %.4f (report %.2f)\n",test ? 'H' : 'V',site,measured,cm[test][site]);
                CHECK(fabsf(measured-cm[test][site])<.012f);
            } else if(test<5) {
                // NIDL Table II.6-1: code 128 = 7.397 fL, black .102, white 31.12.
                float tone=test==2 ? 1 : test==3 ? (7.397f-.102f)/(31.12f-.102f) : 0;
                CHECK(fabsf(a-tone*uniformity[site]/31.1f)<.003f);
            } else {
                CHECK(a==0); // black pillarbox, no repeated edge texels
                CHECK(gpu_half_to_float(out[(site*W+8)*4])>.89f);
            }
        }
    }
    SDL_ReleaseGPUBuffer(gpu,input);SDL_ReleaseGPUBuffer(gpu,bx);
    SDL_ReleaseGPUBuffer(gpu,by);SDL_ReleaseGPUBuffer(gpu,result);
    free(rgb);free(dx);free(dy);free(out);
}

/* Test energy over pixel area, including fractional scanline scaling. */
static void beam_energy(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    c->tv.noise_level=0; c->tv.black_floor=0; c->tv.hum_bar_amplitude=0;
    c->console_psu_hum=0; c->cable.shield_effectiveness=1;
    float *rgb=malloc(v->rgb_size);
    for(unsigned i=0;i<v->rgb_size/sizeof(float);i++) rgb[i]=powf(0.5f,c->tv.gamma);
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb2,rgb,v->rgb_size)); free(rgb);
    const int heights[]={240,480,721,960};
    for(int k=0;k<4;k++) for(int wide=0;wide<2;wide++) {
        int w=8,h=heights[k]; float sigma=wide ? 0.9f : 0.08f;
        CHECK(video_gpu_set_beam_params(v,gpu,w,h,h/240,sigma,sigma));
        float *dx=calloc((size_t)w*h*4,sizeof(float)), *dy=calloc((size_t)w*h*4,sizeof(float));
        uint16_t *out=malloc((size_t)w*h*8);
        for(int y=0;y<h;y++) for(int x=0;x<w;x++) {
            int i=(y*w+x)*4;
            dx[i]=dx[i+1]=dx[i+2]=500; dx[i+3]=1;
            dy[i]=dy[i+1]=dy[i+2]=(y+0.5f)*240/h+v->window.picture_row; dy[i+3]=1;
        }
        CHECK(gpu_buffer_upload(gpu,v->buf_deflection_x,dx,w*h*16));
        CHECK(gpu_buffer_upload(gpu,v->buf_deflection_y,dy,w*h*16));
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        CHECK(dispatch_beam_profile_public(v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
        CHECK(gpu_buffer_download(gpu,v->buf_beam_rgba,out,w*h*8));
        double mean=0; int n=0;
        // Whole scanlines away from the raster boundary.
        for(int y=h/4;y<3*h/4;y++) for(int x=0;x<w;x++) {
            float r=gpu_half_to_float(out[(y*w+x)*4]); CHECK(isfinite(r)); mean+=r; n++;
        }
        CHECK(fabs(mean/n-pow(0.5,c->tv.gamma))<0.003);
        free(dx);free(dy);free(out);
    }
}

/* Measure the PVM nominal vertical spot before mask/glass. TVL specifies
 * horizontal resolution, not scanline thickness: test the actual deposition
 * across drive levels and report its energy-equivalent Gaussian width. */
static void beam_height_response(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    enum { W=2,H=3840,LINE=120 };
    c->tv.gamma=2.4f; c->tv.bloom_gamma=1.8f;
    c->tv.black_floor=0;c->tv.hum_bar_amplitude=0;
    CHECK(video_gpu_set_beam_params(v,gpu,W,H,16,.38f/2.35482f,.90f/2.35482f));
    float *dx=calloc(W*H*4,sizeof(float)), *dy=calloc(W*H*4,sizeof(float));
    float *rgb=calloc(1,v->rgb_size); uint16_t *out=malloc(W*H*8);
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        int i=(y*W+x)*4; dx[i]=dx[i+1]=dx[i+2]=500;dx[i+3]=1;
        dy[i]=dy[i+1]=dy[i+2]=(y+.5f)/16+v->window.picture_row;dy[i+3]=1;
    }
    CHECK(gpu_buffer_upload(gpu,v->buf_deflection_x,dx,W*H*16));
    CHECK(gpu_buffer_upload(gpu,v->buf_deflection_y,dy,W*H*16));
    const float drive[]={.1f,.25f,.5f,.75f,1,1.2f};double last=0;
    for(int k=0;k<6;k++) {
        float current=powf(drive[k],c->tv.gamma);
        for(int x=0;x<v->window.width;x++) for(int ch=0;ch<3;ch++)
            rgb[((size_t)(LINE+v->window.picture_row)*v->window.width+x)*3+ch]=current;
        CHECK(gpu_buffer_upload(gpu,v->buf_rgb2,rgb,v->rgb_size));
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        CHECK(dispatch_beam_profile_public(v,cmd));CHECK(SDL_SubmitGPUCommandBuffer(cmd));
        CHECK(gpu_buffer_download(gpu,v->buf_beam_rgba,out,W*H*8));
        double sum=0,variance=0;
        for(int y=(LINE-4)*16;y<(LINE+5)*16;y++) {
            float light=gpu_half_to_float(out[y*W*4]);
            double distance=(y+.5)/16-(LINE+.5);
            sum+=light/16;variance+=light/16*distance*distance;
        }
        double sigma=sqrt(fmax(variance/sum-1.0/(12*16*16),0));
        double expected=(.38+(.90-.38)*pow(drive[k],1.8))/2.35482;
        CHECK(fabs(sum/current-1)<.002); CHECK(fabs(sigma-expected)<.002);
        CHECK(sigma>last);last=sigma;
        printf("PVM beam drive %.2f: equivalent FWHM %.3f lines, energy %.5f\n",drive[k],sigma*2.35482,sum/current);
    }
    free(dx);free(dy);free(rgb);free(out);
}

/* Flyback blanking on a made-up trace: the receiver forces blanked samples
 * to zero drive before the RGB amplifiers and cuts the beam off there, so
 * neither a bright retrace nor the black floor lights them, and the sample
 * the trace edge crosses carries its covered fraction. */
static void flyback_blanking(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    DecodeWindow saved=v->window;
    v->window.trace_x0=100.25f; v->window.trace_x1=1900.75f;
    v->window.trace_row0=10; v->window.trace_row1=200;
    c->tv.black_floor=.05f; c->tv.gamma=2.2f; c->tv.noise_level=0; c->tv.apl_black_lift=0;
    c->tv.phosphor_gamma_offset_r=c->tv.phosphor_gamma_offset_g=c->tv.phosphor_gamma_offset_b=0;
    c->cable.shield_effectiveness=1;
    size_t floats=v->rgb_size/sizeof(float);
    int width=v->window.width;
    float *rgb=malloc(v->rgb_size), *out=malloc(v->rgb_size);
    for(size_t i=0;i<floats;i++) rgb[i]=.5f;
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb,rgb,v->rgb_size));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_gun_current_public(v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v->buf_gun_current,out,v->rgb_size));
    float full=powf(.5f,2.2f);
    for(int row=0;row<v->window.lines;row++) for(int x=0;x<width;x++) {
        float light=out[((size_t)row*width+x)*3];
        bool on=row>=10 && row<200;
        if(!on || x<100 || x>1901) CHECK(light==0);
        else if(x==100 || x==1901) CHECK(fabsf(light-.25f*full)<1e-6f);
        else CHECK(fabsf(light-full)<1e-6f);
    }
    /* A bright retrace around a black raster (the two edge samples included,
     * as their covered parts are black): nothing reaches the amplifier
     * output, through its taps, its derivative terms or the previous line. */
    c->tv.velocity_mod=.5f; c->tv.asym_rise_fall=.5f; c->tv.vertical_smear=1;
    video_gpu_reinit_stages(v,c);
    for(int row=0;row<v->window.lines;row++) for(int x=0;x<width;x++) for(int ch=0;ch<3;ch++)
        rgb[((size_t)row*width+x)*3+ch]=(row>=10 && row<200 && x>=100 && x<=1901) ? 0 : 5;
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb,rgb,v->rgb_size));
    cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_video_amp_public(v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v->buf_rgb,out,v->rgb_size));
    float peak=0;
    for(size_t i=0;i<floats;i++) peak=fmaxf(peak,fabsf(out[i]));
    printf("Flyback blanking: amplifier output under a 5x white retrace peaks at %g\n",peak);
    CHECK(peak==0);
    c->tv.velocity_mod=c->tv.asym_rise_fall=c->tv.vertical_smear=0;
    c->tv.black_floor=0;
    video_gpu_reinit_stages(v,c);
    v->window=saved;
    free(rgb);free(out);
}

/* A raised gun cutoff follows the raster spot; it is not room illumination. */
static void black_floor_deposition(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    enum { W=8,H=1920 };
    c->tv.black_floor=.1f; c->tv.gamma=2.4f; c->tv.noise_level=0;
    c->tv.phosphor_gamma_offset_r=0; c->tv.phosphor_gamma_offset_g=-.2f;
    c->tv.phosphor_gamma_offset_b=.2f; c->tv.hum_bar_amplitude=0;
    c->console_psu_hum=0; c->cable.shield_effectiveness=1;
    CHECK(video_gpu_set_beam_params(v,gpu,W,H,8,.16f,.16f));
    float *rgb=malloc(v->rgb_size), *current=malloc(v->rgb_size);
    for(unsigned i=0;i<v->rgb_size/sizeof(float);i++) rgb[i]=-.05f;
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb,rgb,v->rgb_size));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_gun_current_public(v,cmd));
    CHECK(dispatch_h_blur_rgb_public(v,cmd));
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v->buf_gun_current,current,v->rgb_size));
    float expected[]={powf(.1f,2.4f),powf(.1f,2.2f),powf(.1f,2.6f)};
    size_t probe=decode_window_rgb_index(&v->window,120,300);
    for(int ch=0;ch<3;ch++) CHECK(fabsf(current[probe+ch]-expected[ch])<1e-6f);
    float *dx=calloc(W*H*4,sizeof(float)), *dy=calloc(W*H*4,sizeof(float));
    uint16_t *out=malloc(W*H*8);
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        int i=(y*W+x)*4; dx[i]=dx[i+1]=dx[i+2]=500; dx[i+3]=x>0 ? 1 : 0;
        dy[i]=dy[i+1]=dy[i+2]=(y+.5f)*240/H+v->window.picture_row; dy[i+3]=1;
    }
    CHECK(gpu_buffer_upload(gpu,v->buf_deflection_x,dx,W*H*16));
    CHECK(gpu_buffer_upload(gpu,v->buf_deflection_y,dy,W*H*16));
    cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_beam_profile_public(v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v->buf_beam_rgba,out,W*H*8));
    for(int ch=0;ch<3;ch++) {
        double sum=0; float lo=1,hi=0;
        for(int y=H/4;y<3*H/4;y++) {
            float light=gpu_half_to_float(out[(y*W+1)*4+ch]);
            sum+=light; lo=fminf(lo,light); hi=fmaxf(hi,light);
            CHECK(out[y*W*4+ch]==0); // Blanked raster stays unlit.
        }
        CHECK(fabs(sum/(H/2)/expected[ch]-1)<.002);
        CHECK(lo<expected[ch]*.1f && hi>expected[ch]*2);
    }
    // DC-restoration drift changes voltage before gamma and spot deposition.
    // A negative drift can extinguish the residual gun current altogether.
    c->tv.apl_black_lift=2;
    for(int high=0;high<2;high++) {
        video_gpu_set_dynamic_state(v,0,high ? 1 : 0,0);
        cmd=SDL_AcquireGPUCommandBuffer(gpu);
        CHECK(dispatch_gun_current_public(v,cmd));
        CHECK(dispatch_h_blur_rgb_public(v,cmd));
        CHECK(dispatch_beam_profile_public(v,cmd));
        CHECK(SDL_SubmitGPUCommandBuffer(cmd));
        CHECK(gpu_buffer_download(gpu,v->buf_gun_current,current,v->rgb_size));
        CHECK(fabsf(current[probe]-(high ? powf(.25f,2.4f) : 0))<1e-6f);
        CHECK(gpu_buffer_download(gpu,v->buf_beam_rgba,out,W*H*8));
        float lo=1,hi=0;
        for(int y=H/4;y<3*H/4;y++) {
            float light=gpu_half_to_float(out[(y*W+1)*4]);
            lo=fminf(lo,light);hi=fmaxf(hi,light);
            CHECK(out[y*W*4]==0);
        }
        if(high) CHECK(lo<hi*.1f && hi>.05f); else CHECK(hi==0);
    }
    c->tv.apl_black_lift=0; v->apl_smoothed=.5f;
    c->tv.black_floor=0;
    c->tv.phosphor_gamma_offset_g=c->tv.phosphor_gamma_offset_b=0;
    free(rgb);free(current);free(dx);free(dy);free(out);
}

/* Below and above the picture the beam finds no picture line: the raster
 * ends where the last line's spot does instead of repeating that line. A
 * PAL set scans 19.5 lines above the picture and 27.5 below it. */
static void beam_below_picture(SDL_GPUDevice *gpu) {
    enum { W=8,H=1200 };
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp,SIGNAL_REGION_PAL);
    video_chain_init_preset(&c,VIDEO_CONN_COMPOSITE,VIDEO_COMB_NONE,SIGNAL_REGION_PAL);
    memset(&c.tv,0,sizeof(c.tv)); c.tv.gamma=1; c.tv.h_size=c.tv.v_size=1;
    c.console_psu_hum=0; c.cable.shield_effectiveness=1;
    CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
    CHECK(video_gpu_set_beam_params(&v,gpu,W,H,5,.3f,.3f));
    for(int i=0;i<v.sig_chain.num_stages;i++) chain_set_stage_enabled(&v.sig_chain,i,i==v.stage_deflection);
    CHECK(chain_run(&v.sig_chain,gpu));
    float *dy=malloc(W*H*16), *rgb=calloc(1,v.rgb_size);
    uint16_t *out=malloc(W*H*8);
    CHECK(gpu_buffer_download(gpu,v.buf_deflection_y,dy,W*H*16));
    for(int line=0;line<240;line++) for(int x=0;x<v.window.width;x++) for(int ch=0;ch<3;ch++)
        rgb[((size_t)(line+v.window.picture_row)*v.window.width+x)*3+ch]=1;
    CHECK(gpu_buffer_upload(gpu,v.buf_rgb2,rgb,v.rgb_size));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_beam_profile_public(&v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v.buf_beam_rgba,out,W*H*8));
    /* The spot reaches at most four lines (beam_profile's radius). */
    int dark=0,lit=0;
    for(int y=0;y<H;y++) {
        float line=dy[(y*W+W/2)*4+1]-v.window.picture_row;
        float light=gpu_half_to_float(out[(y*W+W/2)*4+1]);
        if(line>=244 || line<-4) { CHECK(light==0); dark++; }
        if(line>=2 && line<238) { CHECK(light>.1f); lit++; }
    }
    printf("Beam past the picture: %d dark rows beyond the spot, %d lit rows inside\n",dark,lit);
    CHECK(dark>50 && lit>900);
    free(dy);free(rgb);free(out);video_gpu_destroy(&v,gpu);
}

static void separated_yc(SDL_GPUDevice *gpu) {
    for(int region=0;region<2;region++) {
        SignalPrecompute sp; VideoChain c; VideoGPUChain v;
        signal_precompute_init(&sp,region);
        video_chain_init_preset(&c,VIDEO_CONN_SVIDEO,VIDEO_COMB_NONE,region);
        CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
        CHECK(video_gpu_upload_signal_table(&v,gpu,(float *)sp.table,
            region ? (float *)sp.table_alt : NULL,SIG_TABLE_ENTRIES,SIG_TABLE_STRIDE));
        uint16_t indices[256*240];
        for(int i=0;i<256*240;i++) indices[i]=(i&1) ? 0x20 : 0x0f;
        float *rgb=malloc(v.rgb_size);
        float matrix[3][3]={{1,0,0},{0,1,0},{0,0,1}},bias[3]={0};
        video_gpu_set_color_matrix(&v,matrix,bias);
        video_gpu_set_demod(&v,sp.demod_rotate*6.28318530718f/12,6.28318530718f/12);
        c.cable.ghost_level = .2f;
        c.cable.ghost_delay = 17;
        video_gpu_reinit_stages(&v,&c);
        CHECK(video_gpu_process_full(&v,gpu,indices,0,sp.phase_line_adv,0,rgb));
        CHECK(v.sig_chain.stages[v.stage_yc_route].enabled);
        double false_color=0; float luma_min=1,luma_max=0;
        for(int y=10;y<230;y++) for(int x=64*sp.samples_per_pixel;x<192*sp.samples_per_pixel;x++) {
            size_t i=decode_window_rgb_index(&v.window,y,x);
            false_color=fmax(false_color,fmax(fabsf(rgb[i+1]),fabsf(rgb[i+2])));
            luma_min=fminf(luma_min,rgb[i]);luma_max=fmaxf(luma_max,rgb[i]);
        }
        CHECK(false_color<0.0001); CHECK(luma_max-luma_min>0.1f);
        free(rgb);video_gpu_destroy(&v,gpu);
    }
}

static void gun_bandwidth(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    c->tv.r_bandwidth=1.5e6f; c->tv.g_bandwidth=4e6f; c->tv.b_bandwidth=7e6f;
    video_gpu_reinit_stages(v,c);
    /* An impulse in the middle of a line inside the picture. */
    float *rgb=calloc(1,v->rgb_size), *out=malloc(v->rgb_size);
    int width=v->window.width, x=width/2;
    float *row=out+(size_t)(120+v->window.picture_row)*width*3;
    rgb[(size_t)(120+v->window.picture_row)*width*3+x*3]=1;
    rgb[(size_t)(120+v->window.picture_row)*width*3+x*3+1]=1;
    rgb[(size_t)(120+v->window.picture_row)*width*3+x*3+2]=1;
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb,rgb,v->rgb_size));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_video_amp_public(v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v->buf_rgb,out,v->rgb_size));
    CHECK(row[x*3]<row[x*3+1] && row[x*3+1]<row[x*3+2]);
    for(int channel=0;channel<3;channel++) {
        double energy=0; for(int i=0;i<width;i++) energy+=row[i*3+channel];
        CHECK(fabs(energy-1)<0.0001);
    }
    /* Recover the actual GPU amplifier frequency response from an impulse. */
    c->tv.rgb_bandwidth_3db=1;
    c->tv.r_bandwidth=c->tv.g_bandwidth=c->tv.b_bandwidth=10e6f;
    c->tv.velocity_mod=c->tv.asym_rise_fall=c->tv.vertical_smear=0;
    video_gpu_reinit_stages(v,c);
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb,rgb,v->rgb_size));
    cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_video_amp_public(v,cmd));CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v->buf_rgb,out,v->rgb_size));
    for(int channel=0;channel<3;channel++) {
        double re=0,im=0,dc=0;
        for(int i=0;i<width;i++) {
            double phase=2*M_PI*10e6/signal_format_sample_rate_hz(&v->signal_fmt)*(i-x);
            double voltage=row[i*3+channel];
            re+=voltage*cos(phase);im+=voltage*sin(phase);dc+=voltage;
        }
        double db=20*log10(hypot(re,im)/dc);
        CHECK(fabs(db+3)<.001);
        printf("PVM RGB amplifier channel %d: %.6f dB at 10 MHz\n",channel,db);
    }
    c->tv.rgb_bandwidth_3db=0;
    free(rgb);free(out);
}

static void horizontal_beam_boundaries(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    /* Different flat fields on adjacent lines must remain flat, even when
     * the halo exceeds a line or the last workgroup is mostly inactive. */
    DecodeWindow saved=v->window;
    float saved_sigma=v->beam_h_blur_sigma, saved_growth=c->tv.beam_spot_growth;
    const int widths[]={1,17,255,257};
    float in[2*257*3], out[2*257*3];
    v->window.lines=2;
    for(int w=0;w<4;w++) for(int wide=0;wide<2;wide++) {
        int width=widths[w], count=width*2*3;
        v->window.width=width;
        v->beam_h_blur_sigma=wide ? 8 : .5f;
        c->tv.beam_spot_growth=wide ? .6f : 0;
        for(int y=0;y<2;y++) for(int x=0;x<width;x++) for(int gun=0;gun<3;gun++)
            in[(y*width+x)*3+gun]=.125f*(1+y*3+gun*7);
        CHECK(gpu_buffer_upload(gpu,v->buf_gun_current,in,count*sizeof(float)));
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        CHECK(dispatch_h_blur_rgb_public(v,cmd));
        CHECK(SDL_SubmitGPUCommandBuffer(cmd));
        CHECK(gpu_buffer_download(gpu,v->buf_rgb2,out,count*sizeof(float)));
        for(int i=0;i<count;i++) CHECK(fabsf(out[i]-in[i])<2e-6f);
    }
    v->window=saved;v->beam_h_blur_sigma=saved_sigma;c->tv.beam_spot_growth=saved_growth;
}

static void horizontal_beam_energy(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    c->tv.noise_level=0; c->tv.black_floor=0; c->cable.shield_effectiveness=1;
    /* A single lit sample in the middle of a line inside the picture. */
    float *voltage_buf=calloc(1,v->rgb_size), *light_buf=malloc(v->rgb_size);
    int width=v->window.width, center=width/2;
    size_t row=(size_t)(120+v->window.picture_row)*width*3;
    float *voltage=voltage_buf+row, *light=light_buf+row;
    voltage[center*3]=voltage[center*3+1]=voltage[center*3+2]=.5f;
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb,voltage_buf,v->rgb_size));
    const float sigma[]={.5f,2.5f,8}; float last_peak=1;
    for(int k=0;k<3;k++) {
        v->beam_h_blur_sigma=sigma[k];
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        CHECK(dispatch_gun_current_public(v,cmd));
        CHECK(dispatch_h_blur_rgb_public(v,cmd));
        CHECK(SDL_SubmitGPUCommandBuffer(cmd));
        CHECK(gpu_buffer_download(gpu,v->buf_rgb2,light_buf,v->rgb_size));
        double sum=0;
        for(int x=0;x<width;x++) sum+=light[x*3];
        CHECK(fabs(sum-pow(.5,c->tv.gamma))<1e-5);
        CHECK(light[center*3]<last_peak); last_peak=light[center*3];
        int radius=(int)fminf(ceilf(3*sigma[k]),24);
        double normalizer=0;
        for(int dx=-radius;dx<=radius;dx++) normalizer+=exp(-dx*dx/(2.0*sigma[k]*sigma[k]));
        for(int dx=-radius;dx<=radius;dx++) {
            double reference=pow(.5,c->tv.gamma)*exp(-dx*dx/(2.0*sigma[k]*sigma[k]))/normalizer;
            CHECK(fabs(light[(center+dx)*3]-reference)<1e-6);
        }
    }
    // Space-charge broadening depends on the source gun's current. Check
    // energy and second moment, including red next to an unchanged green.
    c->tv.beam_spot_growth=.6f; v->beam_h_blur_sigma=3;
    double previous=0, green_moment=0;
    const float levels[]={.15f,.5f,1,1.2f};
    for(int k=0;k<4;k++) {
        memset(voltage_buf,0,v->rgb_size);
        voltage[center*3]=levels[k];voltage[center*3+1]=.4f;
        CHECK(gpu_buffer_upload(gpu,v->buf_rgb,voltage_buf,v->rgb_size));
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        CHECK(dispatch_gun_current_public(v,cmd)); CHECK(dispatch_h_blur_rgb_public(v,cmd));
        CHECK(SDL_SubmitGPUCommandBuffer(cmd)); CHECK(gpu_buffer_download(gpu,v->buf_rgb2,light_buf,v->rgb_size));
        double energy[2]={0},moment[2]={0};
        for(int x=0;x<width;x++) for(int gun=0;gun<2;gun++) {
            energy[gun]+=light[x*3+gun];moment[gun]+=(x-center)*(x-center)*light[x*3+gun];
        }
        CHECK(fabs(energy[0]-pow(levels[k],c->tv.gamma))<1e-5);
        CHECK(moment[0]/energy[0]>previous);previous=moment[0]/energy[0];
        if(k) CHECK(fabs(moment[1]-green_moment)<1e-7);
        green_moment=moment[1];
    }
    c->tv.beam_spot_growth=0;
    free(voltage_buf);free(light_buf);
}

/* Exercise the actual BPF + comb kernels: carrier rejection, conservation,
 * and single-line luminance detail that a full-band line average destroys. */
static void source_phase(SDL_GPUDevice *gpu) {
    enum { W=1200,N=W*2 };
    SignalChain sc; CHECK(chain_init(&sc,gpu,N,"shaders/compute"));
    GpuRCFilterParams p={.a=0,.b=1,.total_count=N,.samples_per_line=W,.num_lines=2,
        .nonlinear_tau_samples=30e-9f*42954540};
    chain_add_stage(&sc,"Source impedance",CHAIN_KERNEL_RC_FILTER,&p,sizeof(p),1,1);
    float input[N],out[N];
    for(int row=0;row<2;row++) for(int x=0;x<W;x++)
        input[row*W+x]=((x%12<6 ? (row ? 1100.f : 616.f) : (row ? 880.f : 228.f))-312)/788;
    CHECK(chain_upload_input(&sc,gpu,input,sizeof(input)));CHECK(chain_run(&sc,gpu));
    CHECK(chain_download_output(&sc,gpu,out,sizeof(out)));
    double shift[2];
    for(int row=0;row<2;row++) {
        double re=0,im=0;
        for(int x=120;x<W;x++) {
            re+=out[row*W+x]*cos(6.283185307179586*x/12);
            im-=out[row*W+x]*sin(6.283185307179586*x/12);
        }
        shift[row]=atan2(im,re);
    }
    double degrees=(shift[1]-shift[0])*180/3.141592653589793;
    printf("Source differential phase, row 3 minus 0: %.2f degrees\n",degrees);
    CHECK(degrees < -11 && degrees > -17); // published 2C02G estimate ~14 degrees
    chain_destroy(&sc,gpu);
}

/* NES-001 output follower against the ngspice run of the traced schematic
 * (tools/circuits/nes001_video_chain.cir, golden/nes001_video.h): the four
 * palette rows' chroma gain, phase and cycle-mean shift relative to row 1,
 * and the 10-90% rise of an eight-pixel white pulse. The GPU model is the
 * R2-into-C5 charge towards +5 V with an instantaneous pull-down; the
 * generic transistors, PPU source and ferrite in the deck account for the
 * remaining few per cent and degrees. */
#include "../../../tools/circuits/golden/nes001_video.h"
static void console_follower(SDL_GPUDevice *gpu) {
    enum { W=1200, ROWS=4, N=W*(ROWS+1) };
    const float fs=42954540.0f, tau_ns=168.0f;
    SignalChain sc; CHECK(chain_init(&sc,gpu,N,"shaders/compute"));
    GpuRCFilterParams p={.a=0,.b=1,.total_count=N,.samples_per_line=W,.num_lines=ROWS+1,
        .follower_k=expf(-1e9f/(tau_ns*fs)),.follower_headroom=2.0f};
    chain_add_stage(&sc,"Output follower",CHAIN_KERNEL_RC_FILTER,&p,sizeof(p),1,1);
    static const float terminated[ROWS][2]={{228,616},{312,840},{552,1100},{880,1100}};
    static float input[N],out[N];
    for(int row=0;row<ROWS;row++) for(int x=0;x<W;x++)
        input[row*W+x]=((x%12<6 ? terminated[row][1] : terminated[row][0])-312)/788;
    for(int x=0;x<W;x++) input[ROWS*W+x]=(x>=200 && x<200+8*8) ? 1.0f : 0.0f; /* eight-pixel white pulse */
    CHECK(chain_upload_input(&sc,gpu,input,sizeof(input)));CHECK(chain_run(&sc,gpu));
    CHECK(chain_download_output(&sc,gpu,out,sizeof(out)));
    double gain[ROWS],phase[ROWS],luma[ROWS];
    for(int row=0;row<ROWS;row++) {
        double re_i=0,im_i=0,re_o=0,im_o=0,mean_i=0,mean_o=0; int n=0;
        for(int x=120;x<W;x++) {
            double c=cos(6.283185307179586*x/12),s=sin(6.283185307179586*x/12);
            re_i+=input[row*W+x]*c; im_i-=input[row*W+x]*s; re_o+=out[row*W+x]*c; im_o-=out[row*W+x]*s;
            mean_i+=input[row*W+x]; mean_o+=out[row*W+x]; n++;
        }
        gain[row]=hypot(re_o,im_o)/hypot(re_i,im_i);
        phase[row]=(atan2(im_o,re_o)-atan2(im_i,re_i))*180/3.141592653589793;
        luma[row]=(mean_o-mean_i)/n;
    }
    printf("Console follower vs ngspice (gain, phase deg, luma of white; model / golden):\n");
    for(int row=0;row<ROWS;row++) {
        double g=gain[row]/gain[1], ph=phase[row]-phase[1];
        while(ph>180) ph-=360; while(ph<-180) ph+=360;
        printf("  row %d: %.3f / %.3f, %+.1f / %+.1f, %+.3f / %+.3f\n",row,g,nes001_golden_row_gain_rel_row1[row],
               ph,nes001_golden_row_phase_rel_row1_deg[row],luma[row],nes001_golden_row_luma_error[row]);
        CHECK(fabs(g-nes001_golden_row_gain_rel_row1[row])<0.05);
        CHECK(fabs(ph-nes001_golden_row_phase_rel_row1_deg[row])<3.0);
        CHECK(fabs(luma[row]-nes001_golden_row_luma_error[row])<0.04);
    }
    int r10=-1,r90=-1;
    for(int x=200;x<W;x++) { if(r10<0 && out[ROWS*W+x]>=0.1f) r10=x; if(r90<0 && out[ROWS*W+x]>=0.9f) r90=x; }
    double rise_ns=(r90-r10)*1e9/fs;
    printf("  eight-pixel white rise 10-90%%: %.0f ns (ngspice %.0f ns)\n",rise_ns,(double)NES001_GOLDEN_RISE_10_90_NS);
    CHECK(fabs(rise_ns-NES001_GOLDEN_RISE_10_90_NS)<25);
    CHECK(out[ROWS*W+200+8*8+2]<0.05f); /* the step down is followed at once */
    chain_destroy(&sc,gpu);
}

static void comb_separation(SDL_GPUDevice *gpu) {
    enum { WIDTH=2728, COUNT=WIDTH*6 };
    SignalChain sc; CHECK(chain_init(&sc,gpu,COUNT,"shaders/compute"));
    float *input=calloc(COUNT,sizeof(float)),*y=calloc(COUNT,sizeof(float)),*c=calloc(COUNT,sizeof(float));
    float taps[49]; signal_design_chroma_bandpass(taps,49,42954540,.75e6f);
    int ti=chain_upload_taps(&sc,gpu,taps,49);
    GpuFIRParams fp={COUNT,COUNT,49,1,WIDTH};
    int bp=chain_add_stage(&sc,"Chroma band",CHAIN_KERNEL_FIR,&fp,sizeof(fp),(COUNT+255)/256,1);
    sc.stages[bp].taps_index=ti; sc.stages[bp].rw[0]=CBR_AUX1;
    GpuCombParams cp={COUNT,WIDTH,1,1,2730};
    int ci=chain_add_stage(&sc,"Comb",CHAIN_KERNEL_COMB,&cp,sizeof(cp),(COUNT+255)/256,1);
    for(int pattern=0;pattern<2;pattern++) {
        for(int i=0;i<COUNT;i++) input[i]=pattern ? (i/WIDTH==3 ? .8f : .2f) : .5f+.2f*cosf((float)(i%12)*6.28318530718f/12);
        for(int mode=1;mode<=3;mode++) {
            cp.mode=(uint32_t)mode; chain_update_params(&sc,ci,&cp,sizeof(cp));
            CHECK(chain_upload_input(&sc,gpu,input,COUNT*sizeof(float))); CHECK(chain_run(&sc,gpu));
            CHECK(chain_download_output(&sc,gpu,y,COUNT*sizeof(float)));
            CHECK(gpu_buffer_download(gpu,sc.aux[0],c,COUNT*sizeof(float)));
            for(int row=1;row<5;row++) for(int x=40;x<WIDTH-40;x++) {
                int i=row*WIDTH+x;
                CHECK(fabsf(y[i]+c[i]-input[i])<1e-6f);
                CHECK(fabsf(y[i]-(pattern ? input[i] : .5f))<.00002f);
            }
        }
    }
    /* Fine monochrome detail correlated across the actual 1H delay must
     * not be classified as chroma merely because its band energy is high.
     * Shift the chart by two samples/line to isolate the classifier from
     * the NES/broadcast line-length mismatch. */
    cp.mode=2; chain_update_params(&sc,ci,&cp,sizeof(cp));
    for(int i=0;i<COUNT;i++)
        input[i]=.5f+.2f*cosf((float)((i%WIDTH)-2*(i/WIDTH))*6.28318530718f/12);
    CHECK(chain_upload_input(&sc,gpu,input,COUNT*sizeof(float))); CHECK(chain_run(&sc,gpu));
    CHECK(gpu_buffer_download(gpu,sc.aux[0],c,COUNT*sizeof(float)));
    double false_color=0;
    for(int x=60;x<WIDTH-60;x++) false_color+=c[3*WIDTH+x]*c[3*WIDTH+x];
    false_color=sqrt(false_color/(WIDTH-120));
    printf("Adaptive comb monochrome false-color RMS: %.6f\n",false_color);
    CHECK(false_color<.002);
    /* An isoluminant hue boundary is not monochrome correlation. With
     * neither neighbor matching, retain the current line's chroma band. */
    for(int i=0;i<COUNT;i++) {
        float phase=(i/WIDTH==3 ? 1.57079632679f : 0);
        input[i]=.5f+.2f*cosf((float)(i%12)*6.28318530718f/12+phase);
    }
    CHECK(chain_upload_input(&sc,gpu,input,COUNT*sizeof(float))); CHECK(chain_run(&sc,gpu));
    CHECK(gpu_buffer_download(gpu,sc.aux[0],c,COUNT*sizeof(float)));
    double color_error=0;
    for(int x=60;x<WIDTH-60;x++) {
        int i=3*WIDTH+x;
        double error=c[i]-(input[i]-.5f);
        color_error+=error*error;
    }
    color_error=sqrt(color_error/(WIDTH-120));
    printf("Adaptive comb hue-boundary error RMS: %.6f\n",color_error);
    CHECK(color_error<.002);
    cp.mode=3; chain_update_params(&sc,ci,&cp,sizeof(cp));
    // A chroma impulse in the centre line reaches only its immediate neighbours.
    for(int i=0;i<COUNT;i++) input[i]=.5f+(i/WIDTH==3 ? .2f*cosf((float)(i%12)*6.28318530718f/12) : 0);
    CHECK(chain_upload_input(&sc,gpu,input,COUNT*sizeof(float))); CHECK(chain_run(&sc,gpu));
    CHECK(gpu_buffer_download(gpu,sc.aux[0],c,COUNT*sizeof(float)));
    CHECK(fabsf(c[WIDTH+120])<1e-6f);
    CHECK(fabsf(c[3*WIDTH+120]-.1f)<.00002f);
    free(input);free(y);free(c);chain_destroy(&sc,gpu);
}

static void decoder_gain(SDL_GPUDevice *gpu) {
    SignalChain sc; CHECK(chain_init(&sc,gpu,240,"shaders/compute"));
    // A burst at the standard amplitude (40 IRE peak to peak, 0.20) gives
    // unity gain; a halved one doubles it.
    float input[240]; const float nominal=0.20f;
    GpuModulatorParams p={.count=240,.mode=3,.dp=6.28318530718f/12,.param_a=1,.samples_per_line=240,.burst_reference=nominal};
    int stage=chain_add_stage(&sc,"Detector gain",CHAIN_KERNEL_RECEIVER_DEMOD,&p,sizeof(p),1,1);
    ChainStage *d=&sc.stages[stage];d->io_typed=true;d->ro_count=2;d->ro[0]=CBR_BUF_SRC;d->ro[1]=CBR_AUX3;
    d->rw_count=2;d->rw[0]=CBR_AUX0;d->rw[1]=CBR_AUX1;
    for(int attenuation=0;attenuation<2;attenuation++) {
        float gain=attenuation ? 0.5f : 1.0f;
        float reference[4]={0,0.125f,nominal*gain,0};
        for(int i=0;i<240;i++) input[i]=0.125f+gain*(0.1f*cosf(i*p.dp)+0.2f*sinf(i*p.dp));
        CHECK(chain_upload_input(&sc,gpu,input,sizeof(input)));
        CHECK(gpu_buffer_upload(gpu,sc.aux[3],reference,sizeof(reference))); CHECK(chain_run(&sc,gpu));
        float iq[240];
        for(int channel=0;channel<2;channel++) {
            CHECK(gpu_buffer_download(gpu,sc.aux[channel],iq,sizeof(iq)));
            float mean=0;for(int i=0;i<240;i++) mean+=iq[i]/240;
            CHECK(fabsf(mean-(channel ? 0.2f : 0.1f))<0.0001f);
        }
    }
    chain_destroy(&sc,gpu);
}

/* Decoder output is voltage, so nominal white is not a storage ceiling.
 * Exercise the real matrix and gun stages with superwhite and undershoot. */
static void decoder_voltage_range(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp,SIGNAL_REGION_NTSC);
    video_chain_init_preset(&c,VIDEO_CONN_COMPOSITE,VIDEO_COMB_NONE,SIGNAL_REGION_NTSC);
    c.tv.noise_level=0; c.tv.black_floor=0; c.tv.apl_black_lift=0;
    c.tv.gamma=2; c.tv.phosphor_gamma_offset_r=c.tv.phosphor_gamma_offset_g=c.tv.phosphor_gamma_offset_b=0;
    c.cable.shield_effectiveness=1;
    CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
    CHECK(video_gpu_set_beam_params(&v,gpu,16,16,1,.2f,.7f));
    for(int i=0;i<v.sig_chain.num_stages;i++) chain_set_stage_enabled(&v.sig_chain,i,i==v.stage_matrix);
    float matrix[3][3]={{1,0,0},{.5f,0,0},{.25f,0,0}},bias[3]={-.2f,-.2f,-.2f};
    video_gpu_set_color_matrix(&v,matrix,bias);
    float *wave=calloc(1,v.sig_chain.buf_size),*rgb=malloc(v.rgb_size),*current=malloc(v.rgb_size);
    ChromaAuxLayout chroma=video_chain_chroma_aux_layout(false);
    CHECK(gpu_buffer_upload(gpu,v.sig_chain.aux[chroma.i_filt],wave,v.sig_chain.buf_size));
    CHECK(gpu_buffer_upload(gpu,v.sig_chain.aux[chroma.q_filt],wave,v.sig_chain.buf_size));
    for(int i=0;i<v.raster_fmt.total_samples;i++)
        wave[i]=1.8f*(float)(i%v.raster_fmt.samples_per_line)/(v.raster_fmt.samples_per_line-1);
    float reference[240*4]={0};
    CHECK(gpu_buffer_upload(gpu,v.buf_receiver,reference,sizeof(reference)));
    CHECK(chain_upload_input(&v.sig_chain,gpu,wave,v.sig_chain.buf_size));
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(gpu_buffer_download(gpu,v.buf_rgb,rgb,v.rgb_size));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_gun_current_public(&v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v.buf_gun_current,current,v.rgb_size));
    /* Every line carries the same ramp; check one inside the picture. */
    for(int x=0;x<sp.samples_per_line;x++) for(int ch=0;ch<3;ch++) {
        float y=wave[65*sp.samples_per_pixel+x];
        float voltage=y*matrix[ch][0]+bias[ch];
        size_t i=decode_window_rgb_index(&v.window,120,x)+ch;
        CHECK(fabsf(rgb[i]-voltage)<.00001f);
        CHECK(fabsf(current[i]-powf(fmaxf(voltage,0),2))<.00001f);
    }
    CHECK(rgb[decode_window_rgb_index(&v.window,120,sp.samples_per_line-1)]>1.4f);
    CHECK(rgb[decode_window_rgb_index(&v.window,120,0)+2]<0);
    free(wave);free(rgb);free(current);video_gpu_destroy(&v,gpu);
}

int main(void) {
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!gpu) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    fir_boundaries(gpu);
    scanline_dispatch(gpu);
    failures += test_display_fidelity(gpu);
    failures += test_crt_load(gpu);
    failures += test_osd(gpu);
    failures += test_border(gpu);
    source_phase(gpu);
    console_follower(gpu);
    comb_separation(gpu);
    {
        SignalPrecompute pal;VideoChain c;VideoGPUChain v;
        signal_precompute_init(&pal,SIGNAL_REGION_PAL);
        video_chain_init_preset(&c,VIDEO_CONN_COMPOSITE,VIDEO_COMB_3LINE,SIGNAL_REGION_PAL);
        CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",pal.fir_y,pal.fir_y_n,pal.fir_c,pal.fir_c_n,pal.fir_q,pal.fir_q_n));
        CHECK(!chain_get_stage_enabled(&v.sig_chain,v.stage_comb));
        CHECK(!chain_get_stage_enabled(&v.sig_chain,v.stage_comb_bandpass));
        CHECK(chain_get_stage_enabled(&v.sig_chain,v.stage_pal_chroma));
        video_gpu_destroy(&v,gpu);
    }
    receiver(gpu);
    decoder_gain(gpu);
    decoder_voltage_range(gpu);
    separated_yc(gpu);
    beam_below_picture(gpu);
    rgb_source(gpu);
    rf(gpu);
    rf_snow_level(gpu);
    rf_sidebands(gpu);
    failures += test_vhs_fidelity(gpu);
    failures += test_encoder(gpu);
    rf_temporal_continuity(gpu);
    dac_equivalence(gpu, SIGNAL_REGION_NTSC);
    dac_equivalence(gpu, SIGNAL_REGION_PAL);
    composite_edge_phase(gpu);
    luma_sharpness(gpu);
    sharpness_chroma_routing(gpu);
    SignalPrecompute sp;
    VideoChain c;
    VideoGPUChain v;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);
    video_chain_init_preset(&c, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE, SIGNAL_REGION_NTSC);
    CHECK(video_gpu_init(&v, gpu, &c, "shaders/compute", sp.fir_y, sp.fir_y_n, sp.fir_c, sp.fir_c_n, sp.fir_q, sp.fir_q_n));
    CHECK(video_gpu_set_beam_params(&v, gpu, 16, 16, 1, 0.2f, 0.7f));
    c.tv.gamma = 2.2f;
    gun_bandwidth(gpu,&v,&c);
    horizontal_beam_boundaries(gpu,&v,&c);
    horizontal_beam_energy(gpu,&v,&c);
    beam_energy(gpu,&v,&c);
    fw900_measurements(gpu,&v);
    independent_guns(gpu,&v,&c);
    beam_height_response(gpu,&v,&c);
    black_floor_deposition(gpu,&v,&c);
    flyback_blanking(gpu,&v,&c);
    CHECK(video_gpu_set_beam_params(&v,gpu,16,16,1,0.2f,0.7f));
    v.blend_r = v.blend_g = v.blend_b = 0.5f;
    temporal(gpu, &v, 0x3c003c00, 1.0f); /* first frame must ignore uninitialised history */
    temporal(gpu, &v, 0, 0.5f);
    temporal(gpu, &v, 0, 0.25f); /* must retain accumulated history beyond one frame */
    v.blend_r = v.blend_g = v.blend_b = 0;
    temporal(gpu, &v, 0, 0); /* zero means off, not 95% history */
    v.temporal_blend = 0.5f;
    temporal(gpu, &v, 0x38003800, 0.5f);
    CHECK(v.smoothing_history_valid);
    v.temporal_blend = 0;
    video_gpu_reset_temporal_state(&v, gpu);
    temporal(gpu, &v, 0x38003800, 0.5f);
    CHECK(video_gpu_set_beam_params(&v, gpu, 32, 16, 1, 0.2f, 0.7f));
    CHECK(!v.temporal_history_valid);
    v.temporal_history_valid=true;
    CHECK(video_gpu_set_beam_params(&v,gpu,16,32,1,0.2f,0.7f));
    CHECK(!v.temporal_history_valid); // Equal pixel area, different texture shape.
    v.tail_weight=.1f;v.tail_r=v.tail_g=v.tail_b=.8f;
    v.blend_r=v.blend_g=v.blend_b=.5f;
    CHECK(video_gpu_set_beam_params(&v,gpu,16,16,1,.2f,.7f));
    temporal(gpu,&v,0x3c003c00,1);CHECK(fabsf(temporal_output(gpu,&v)-1)<.001f);
    temporal(gpu,&v,0,.5f);CHECK(fabsf(temporal_output(gpu,&v)-.53f)<.001f);
    temporal(gpu,&v,0,.25f);CHECK(fabsf(temporal_output(gpu,&v)-.289f)<.001f);
    v.elapsed_frames=2;
    temporal(gpu,&v,0,.0625f);CHECK(fabsf(temporal_output(gpu,&v)-.09721f)<.001f);
    video_gpu_reset_temporal_state(&v,gpu);
    temporal(gpu,&v,0x3c003c00,1);CHECK(fabsf(temporal_output(gpu,&v)-1)<.001f);
    v.tail_weight=0;
    CHECK(video_gpu_set_beam_params(&v,gpu,16,16,1,.2f,.7f));
    CHECK(v.phosphor_history_size==v.beam_rgba_size && !v.temporal_history_valid);
    video_gpu_destroy(&v,gpu);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    printf("Fidelity regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
