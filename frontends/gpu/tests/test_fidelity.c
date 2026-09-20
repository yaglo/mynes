/* GPU regressions for temporal history and RF; optional end-to-end benchmark. */
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
extern int test_display_fidelity(SDL_GPUDevice *gpu);
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); failures++; } } while (0)

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

static void rf(SDL_GPUDevice *gpu) {
    SignalChain sc;
    CHECK(chain_init(&sc, gpu, 256, "shaders/compute"));
    GpuRFParams p = {.count=256, .samples_per_line=128, .noise_amplitude=0.1f,
        .sample_rate=42954540.0f, .full_line_samples=2728, .hum_hz=60.0f};
    int stage = chain_add_stage(&sc, "RF test", CHAIN_KERNEL_RF, &p, sizeof(p), 1, 1);
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
}

static void dac_equivalence(SDL_GPUDevice *gpu, int region) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp, region);
    video_chain_init_preset(&c, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE, region);
    CHECK(video_gpu_init(&v, gpu, &c, "shaders/compute", sp.fir_y, sp.fir_y_n, sp.fir_c, sp.fir_c_n, sp.fir_q, sp.fir_q_n));
    CHECK(video_gpu_upload_signal_table(&v, gpu, (float *)sp.table,
        region == SIGNAL_REGION_PAL ? (float *)sp.table_alt : NULL, SIG_TABLE_ENTRIES, SIG_TABLE_STRIDE));
    uint16_t indices[256*240];
    for (int i=0;i<256*240;i++) indices[i]=(uint16_t)((i/7)%512);
    size_t count=(size_t)sp.samples_per_line*240;
    float *wave=calloc(count,sizeof(float)), *cpu=calloc(count*3,sizeof(float)), *full=calloc(count*3,sizeof(float));
    for (unsigned frame=0;frame<3;frame++) {
        waveform_generate(wave,indices,&sp,frame);
        int phase=signal_frame_phase(&sp,frame);
        v.signal_phase_base=phase;
        video_gpu_set_demod(&v,(phase+sp.demod_rotate)*6.28318530718f/12,6.28318530718f/12);
        CHECK(video_gpu_process(&v,gpu,wave,cpu));
        CHECK(video_gpu_process_full(&v,gpu,indices,phase,sp.phase_line_adv,0,full));
        float max_error=0;
        for(size_t i=0;i<count*3;i++) {
            CHECK(isfinite(full[i]));
            max_error=fmaxf(max_error,fabsf(cpu[i]-full[i]));
        }
        CHECK(max_error<0.0001f);
    }
    free(wave);free(cpu);free(full);
    video_gpu_destroy(&v,gpu);
}

static void receiver(SDL_GPUDevice *gpu) {
    for(int region=0;region<2;region++) {
        unsigned spp=region ? 10 : 8, width=341*spp, count=width*2;
        SignalChain sc;
        CHECK(chain_init(&sc,gpu,(int)count,"shaders/compute"));
        float *input=calloc(count,sizeof(float)), *raster=calloc(count,sizeof(float));
        for(unsigned i=0;i<256*spp*2;i++) input[i]=0.4f;
        struct { uint32_t count,width,active_width,spp; float phase,line_phase; uint32_t region,lines,separate_yc; }
            ep={count,width,256*spp,spp,3,(float)signal_region_line_phase(region),(uint32_t)region,2,0};
        int encode=chain_add_stage(&sc,"Raster test",CHAIN_KERNEL_RASTER,&ep,sizeof(ep),(count+255)/256,1);
        ChainStage *e=&sc.stages[encode]; e->io_typed=true;
        e->ro_count=2; e->ro[0]=CBR_BUF_SRC; e->ro[1]=CBR_AUX3;
        e->rw_count=2; e->rw[0]=CBR_BUF_DST; e->rw[1]=CBR_AUX2;
        CHECK(chain_upload_input(&sc,gpu,input,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
        CHECK(chain_download_output(&sc,gpu,raster,count*sizeof(float)));
        CHECK(fabsf(raster[10*spp]+264.0f/788.0f)<1e-6f);
        CHECK(fabsf(raster[70*spp]-0.4f)<1e-6f);
        CHECK(raster[46*spp]==0);
        uint32_t rp[]={2,width,spp,(uint32_t)region};
        int lock=chain_add_stage(&sc,"Receiver test",CHAIN_KERNEL_RECEIVER,rp,sizeof(rp),1,1);
        ChainStage *l=&sc.stages[lock]; l->io_typed=true;
        l->ro_count=1;l->ro[0]=CBR_BUF_SRC;l->rw_count=1;l->rw[0]=CBR_AUX0;
        chain_set_stage_enabled(&sc,encode,false);
        for(unsigned i=0;i<count;i++) raster[i]+=0.125f;
        CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
        float ref[8]; CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        for(int line=0;line<2;line++) {
            float expected=(3+line*signal_region_line_phase(region))*6.28318530718f/12;
            CHECK(fabsf(remainderf(ref[line*4]-expected,6.28318530718f))<1e-4f);
            CHECK(fabsf(ref[line*4+1]-0.125f)<1e-5f);
            CHECK(ref[line*4+2]>0.3f);
        }
        for(unsigned line=0;line<2;line++) for(unsigned x=29*spp;x<44*spp;x++) raster[line*width+x]=0.125f;
        CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float))); CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        CHECK(ref[2]<1e-5f && ref[6]<1e-5f);
        chain_set_stage_enabled(&sc,lock,false);
        // Two very different pictures, the same attenuated sync: gain must agree.
        for(unsigned line=0;line<2;line++) for(unsigned x=0;x<width;x++)
            raster[line*width+x]=(x<25*spp) ? -132.0f/788.0f : (x>=65*spp && x<321*spp ? (line ? 0.4f : 0.05f) : 0);
        GpuAGCParams ap={count,width,2,264.0f/788.0f,1,1,0.5f,4};
        int agc=chain_add_stage(&sc,"Sync AGC test",CHAIN_KERNEL_AGC,&ap,sizeof(ap),1,1);
        ChainStage *a=&sc.stages[agc];a->io_typed=true;a->ro_count=0;a->rw_count=2;
        a->rw[0]=CBR_BUF_SRC;a->rw[1]=CBR_AUX0;
        float zeros[8]={0};CHECK(gpu_buffer_upload(gpu,sc.aux[0],zeros,sizeof(zeros)));
        CHECK(chain_upload_input(&sc,gpu,raster,count*sizeof(float)));CHECK(chain_run(&sc,gpu));
        CHECK(gpu_buffer_download(gpu,sc.aux[0],ref,sizeof(ref)));
        CHECK(fabsf(ref[0]-2)<1e-5f && fabsf(ref[2]-2)<1e-5f);
        free(input);free(raster);chain_destroy(&sc,gpu);
    }
}

/* Test energy over pixel area, including fractional scanline scaling. */
static void beam_energy(SDL_GPUDevice *gpu, VideoGPUChain *v, VideoChain *c) {
    c->tv.noise_level=0; c->tv.black_floor=0; c->tv.hum_bar_amplitude=0;
    c->console_psu_hum=0; c->cable.shield_effectiveness=1;
    float *rgb=malloc(v->rgb_size);
    for(unsigned i=0;i<v->rgb_size/sizeof(float);i++) rgb[i]=0.5f;
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
            dy[i]=dy[i+1]=dy[i+2]=y+0.5f; dy[i+3]=1;
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
            int i=(y*sp.samples_per_line+x)*3;
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
    float *rgb=calloc(1,v->rgb_size), *out=malloc(v->rgb_size);
    int x=v->signal_fmt.samples_per_line/2;
    rgb[x*3]=rgb[x*3+1]=rgb[x*3+2]=1;
    CHECK(gpu_buffer_upload(gpu,v->buf_rgb,rgb,v->rgb_size));
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    CHECK(dispatch_video_amp_public(v,cmd)); CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    CHECK(gpu_buffer_download(gpu,v->buf_rgb,out,v->rgb_size));
    CHECK(out[x*3]<out[x*3+1] && out[x*3+1]<out[x*3+2]);
    for(int channel=0;channel<3;channel++) {
        double energy=0; for(int i=0;i<v->signal_fmt.samples_per_line;i++) energy+=out[i*3+channel];
        CHECK(fabs(energy-1)<0.0001);
    }
    free(rgb);free(out);
}

static void decoder_gain(SDL_GPUDevice *gpu) {
    SignalChain sc; CHECK(chain_init(&sc,gpu,240,"shaders/compute"));
    float input[240]; const float nominal=(376.0f/788.0f)/(6*sinf(3.14159265359f/12));
    GpuModulatorParams p={.count=240,.mode=3,.dp=6.28318530718f/12,.param_a=1,.samples_per_line=240};
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

int main(int argc, char **argv) {
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!gpu) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    failures += test_display_fidelity(gpu);
    receiver(gpu);
    decoder_gain(gpu);
    separated_yc(gpu);
    rf(gpu);
    dac_equivalence(gpu, SIGNAL_REGION_NTSC);
    dac_equivalence(gpu, SIGNAL_REGION_PAL);
    SignalPrecompute sp;
    VideoChain c;
    VideoGPUChain v;
    signal_precompute_init(&sp, SIGNAL_REGION_NTSC);
    video_chain_init_preset(&c, VIDEO_CONN_COMPOSITE, VIDEO_COMB_NONE, SIGNAL_REGION_NTSC);
    CHECK(video_gpu_init(&v, gpu, &c, "shaders/compute", sp.fir_y, sp.fir_y_n, sp.fir_c, sp.fir_c_n, sp.fir_q, sp.fir_q_n));
    CHECK(video_gpu_set_beam_params(&v, gpu, 16, 16, 1, 0.2f, 0.7f));
    c.tv.gamma = 2.2f;
    gun_bandwidth(gpu,&v,&c);
    beam_energy(gpu,&v,&c);
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
    if (argc > 1 && strcmp(argv[1], "--benchmark") == 0) {
        CHECK(video_gpu_set_beam_params(&v, gpu, 640, 480, 2, 0.2f, 0.7f));
        uint16_t indices[256*240];
        for (int i=0;i<256*240;i++) indices[i]=(i/32)%64;
        float *wave=calloc((size_t)sp.samples_per_line*240,sizeof(float));
        float *rgb=calloc((size_t)sp.samples_per_line*240*3,sizeof(float));
        waveform_generate(wave,indices,&sp,0);
        CHECK(video_gpu_upload_signal_table(&v,gpu,(float *)sp.table,NULL,SIG_TABLE_ENTRIES,SIG_TABLE_STRIDE));
        for (int mode=0;mode<3;mode++) {
            for (int i=0;i<3;i++) CHECK(mode==2 ? video_gpu_process_full(&v,gpu,indices,0,sp.phase_line_adv,0,NULL) : video_gpu_process(&v,gpu,wave,mode ? NULL : rgb));
            SDL_WaitForGPUIdle(gpu);
            Uint64 start=SDL_GetPerformanceCounter();
            for (int i=0;i<30;i++) {
                if (mode < 2) waveform_generate(wave,indices,&sp,0);
                CHECK(mode==2 ? video_gpu_process_full(&v,gpu,indices,0,sp.phase_line_adv,0,NULL) : video_gpu_process(&v,gpu,wave,mode ? NULL : rgb));
            }
            SDL_WaitForGPUIdle(gpu);
            printf("%s: %.3f ms/frame (30 frames, 640x480 beam)\n", mode==2 ? "GPU DAC + resident" : mode ? "GPU resident" : "RGB readback", (double)(SDL_GetPerformanceCounter()-start)*1000.0/SDL_GetPerformanceFrequency()/30.0);
        }
        free(wave);free(rgb);
    }
    video_gpu_destroy(&v,gpu);
    SDL_DestroyGPUDevice(gpu);
    SDL_Quit();
    printf("Fidelity regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
