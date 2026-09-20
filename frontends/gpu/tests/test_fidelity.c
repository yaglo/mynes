/* GPU regressions for temporal history and RF; optional end-to-end benchmark. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "video_gpu.h"
#include "waveform_gen.h"
#include "gpu_half.h"

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
        struct { uint32_t count,width,active_width,spp; float phase,line_phase; uint32_t region,lines; }
            ep={count,width,256*spp,spp,3,(float)signal_region_line_phase(region),(uint32_t)region,2};
        int encode=chain_add_stage(&sc,"Raster test",CHAIN_KERNEL_RASTER,&ep,sizeof(ep),(count+255)/256,1);
        ChainStage *e=&sc.stages[encode]; e->io_typed=true;
        e->ro_count=1; e->ro[0]=CBR_BUF_SRC; e->rw_count=1; e->rw[0]=CBR_BUF_DST;
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

int main(int argc, char **argv) {
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!gpu) { fprintf(stderr, "%s\n", SDL_GetError()); return 1; }
    failures += test_display_fidelity(gpu);
    receiver(gpu);
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
