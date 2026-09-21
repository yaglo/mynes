#include "gpu_benchmark.h"
#include "gpu_display.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

enum { WARMUP = 12, SAMPLES = 60 };
static int compare(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

bool gpu_benchmark(VideoGPUChain *v, SDL_GPUDevice *gpu,
                   const SignalPrecompute *sp, const char *render_shader_dir, bool pixel_aligned) {
    bool recompute_geometry = getenv("MYNES_BENCH_RECOMPUTE_GEOMETRY") != NULL;
    const int sizes[][2] = {{640,480}, {1280,960}, {1920,1440}, {2560,1920}};
    uint16_t codes[256*240];
    for (int y=0; y<240; y++) for (int x=0; x<256; x++) {
        // All DAC colours plus monochrome detail.
        codes[y*256+x] = y<192 ? (uint16_t)((x/16)+(y/48)*16)
                              : (x%8<4 ? 0x20 : 0x0f);
    }
    printf("BENCH backend=%s validation=%d frames=%d warmup=%d region=%s\n",
           SDL_GetGPUDeviceDriver(gpu), getenv("MYNES_GPU_VALIDATION")!=NULL,
           SAMPLES,WARMUP,sp->region ? "PAL" : "NTSC");
    printf("BENCH metric=CPU-submit-to-final-GPU-fence; includes uploads, complete CRT; excludes emulation/audio/vsync/readback\n");
    printf("BENCH geometry_cache=%d\n", !recompute_geometry);
    for (unsigned size=0; size<sizeof(sizes)/sizeof(sizes[0]); size++) {
        int w=sizes[size][0], h=v->chain->tv.monitor_model==1 ? w*10/16 : sizes[size][1];
        if (!video_gpu_set_beam_params(v,gpu,w,h,h/240,v->beam_sigma_narrow,v->beam_sigma_wide)) return false;
        GPUDisplay display={0};
        if (!gpu_display_init_target(&display,gpu,SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,w,h,render_shader_dir)) return false;
        SDL_GPUTextureCreateInfo ti={.type=SDL_GPU_TEXTURETYPE_2D,
            .format=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
            .usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,.width=w,.height=h,
            .layer_count_or_depth=1,.num_levels=1};
        SDL_GPUTexture *target=SDL_CreateGPUTexture(gpu,&ti);
        bool ok=target!=NULL;
        double ms[SAMPLES], total=0;
        GPUDisplayParams p;
        gpu_display_params_from_tv(&p,&v->chain->tv,w,h,w,h);
        gpu_display_fit_mask(&p,pixel_aligned,1,1,0,0);
        p.output_hdr=1; p.hdr_headroom=4; p.sdr_white_level=1;
        SDL_WaitForGPUIdle(gpu);
        for (int frame=-WARMUP; ok && frame<SAMPLES; frame++) {
            SDL_PumpEvents();
            int phase=signal_frame_phase(sp,(unsigned)(frame+WARMUP));
            v->signal_phase_base=phase;
            v->signal_frame_counter=v->beam_frame_counter=(unsigned)(frame+WARMUP);
            v->elapsed_frames=1;
            video_gpu_set_demod(v,(phase+sp->demod_rotate)*6.28318530718f/12,6.28318530718f/12);
            Uint64 start=SDL_GetTicksNS();
            if (recompute_geometry) v->deflection_cache_valid = false;
            ok=video_gpu_process_full(v,gpu,codes,phase,sp->phase_line_adv,0,NULL);
            if (!ok) break;
            SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
            if (!cmd) { ok=false; break; }
            gpu_display_render(&display,gpu,cmd,video_gpu_get_beam_texture(v),w,h,target,w,h,&p,NULL);
            SDL_GPUFence *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
            if (!fence) { ok=false; break; }
            ok=SDL_WaitForGPUFences(gpu,true,&fence,1);
            double elapsed=(SDL_GetTicksNS()-start)/1e6;
            SDL_ReleaseGPUFence(gpu,fence);
            if (frame>=0) { ms[frame]=elapsed; total+=elapsed; }
        }
        if (ok) {
            qsort(ms,SAMPLES,sizeof(*ms),compare);
            printf("BENCH %dx%d mean_ms=%.3f median_ms=%.3f p95_ms=%.3f max_ms=%.3f equivalent_fps=%.1f\n",
                w,h,total/SAMPLES,(ms[SAMPLES/2-1]+ms[SAMPLES/2])/2,
                ms[(int)ceil(SAMPLES*.95)-1],ms[SAMPLES-1],1000*SAMPLES/total);
            fflush(stdout);
        }
        if (target) SDL_ReleaseGPUTexture(gpu,target);
        gpu_display_destroy(&display,gpu);
        if (!ok) return false;
    }
    return true;
}
