/* Read the actual render target: test light units, output transfer, and mask energy. */
#include "gpu_display.h"
#include "gpu_half.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 192
#define H 128
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"Display FAIL %d: %s\n",__LINE__,#x); failures++; } } while(0)

static void render(SDL_GPUDevice *gpu, GPUDisplay *d, SDL_GPUTexture *input,
                   SDL_GPUTexture *target, GPUDisplayParams *p, float avg[3], float *peak) {
    SDL_GPUTransferBufferCreateInfo bi = {.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=W*H*8};
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(gpu,&bi);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    gpu_display_render(d,gpu,cmd,input,W,H,target,W,H,p,NULL);
    SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion region={.texture=target,.w=W,.h=H,.d=1};
    SDL_GPUTextureTransferInfo dst={.transfer_buffer=download,.pixels_per_row=W,.rows_per_layer=H};
    SDL_DownloadFromGPUTexture(copy,&region,&dst);
    SDL_EndGPUCopyPass(copy);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd)); CHECK(SDL_WaitForGPUIdle(gpu));
    const uint16_t *v=SDL_MapGPUTransferBuffer(gpu,download,false);
    CHECK(v!=NULL); avg[0]=avg[1]=avg[2]=0; *peak=0;
    if(v) {
        for(int i=0;i<W*H;i++) for(int c=0;c<3;c++) {
            float x=gpu_half_to_float(v[4*i+c]);
            CHECK(isfinite(x)); avg[c]+=x/(W*H); *peak=fmaxf(*peak,x);
        }
        SDL_UnmapGPUTransferBuffer(gpu,download);
    }
    SDL_ReleaseGPUTransferBuffer(gpu,download);
}

int test_display_fidelity(SDL_GPUDevice *gpu) {
    failures=0;
    GPUDisplay d;
    if(!gpu_display_init_target(&d,gpu,SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,W,H,"shaders/render")) {
        fprintf(stderr,"Display init: %s\n",SDL_GetError()); return 1;
    }
    SDL_GPUTextureCreateInfo ci={.type=SDL_GPU_TEXTURETYPE_2D,.format=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        .usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,.width=W,.height=H,.layer_count_or_depth=1,.num_levels=1};
    SDL_GPUTexture *input=SDL_CreateGPUTexture(gpu,&ci), *target=SDL_CreateGPUTexture(gpu,&ci);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUColorTargetInfo ct={.texture=input,.clear_color={0.25f,0.25f,0.25f,1},.load_op=SDL_GPU_LOADOP_CLEAR,.store_op=SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL); SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    GPUDisplayParams p={.glass_tint=1,.hdr_gain=1,.output_hdr=1,.hdr_headroom=8,.sdr_white_level=1,
        .mask_pitch_px=4,.cathode_gain_r=1,.cathode_gain_g=1,.cathode_gain_b=1};
    float avg[3],peak;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(fabsf(avg[0]-0.25f)<0.001f);
    p.hdr_gain=2;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(fabsf(avg[0]-0.5f)<0.001f);
    p.hdr_gain=1; p.output_hdr=0; p.hdr_headroom=1;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(fabsf(avg[0]-0.537099f)<0.001f);
    p.output_hdr=1; p.hdr_headroom=8; p.halation_strength=0.3f;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(fabsf(avg[0]-0.25f)<0.001f);
    p.halation_strength=0; p.mask_strength=1;
    for(int type=0;type<3;type++) {
        p.mask_type=type;
        render(gpu,&d,input,target,&p,avg,&peak);
        printf("Mask %d: mean %.4f %.4f %.4f, peak %.4f\n",type,avg[0],avg[1],avg[2],peak);
        for(int c=0;c<3;c++) CHECK(fabsf(avg[c]-0.25f)<0.015f);
        CHECK(peak>0.4f); CHECK(fabsf(avg[0]-avg[2])<0.005f);
    }
    p.hdr_gain=8; p.hdr_headroom=1.5f;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(peak<=1.501f);
    p.mask_strength=0; p.hdr_gain=1; p.sdr_white_level=2;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(fabsf(avg[0]-0.5f)<0.001f);
    SDL_ReleaseGPUTexture(gpu,input); SDL_ReleaseGPUTexture(gpu,target); gpu_display_destroy(&d,gpu);
    return failures;
}
