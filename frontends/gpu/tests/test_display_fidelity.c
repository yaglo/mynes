/* Read the actual render target: test light units, output transfer, and mask energy. */
#include "gpu_display.h"
#include "gpu_half.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 192
#define H 128
static int failures;
static float center_row[W][3];
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"Display FAIL %d: %s\n",__LINE__,#x); failures++; } } while(0)

static void render_region(SDL_GPUDevice *gpu, GPUDisplay *d, SDL_GPUTexture *input,
                   SDL_GPUTexture *target, GPUDisplayParams *p, const SDL_GPUViewport *viewport, float avg[3], float *peak) {
    SDL_GPUTransferBufferCreateInfo bi = {.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,.size=W*H*8};
    SDL_GPUTransferBuffer *download = SDL_CreateGPUTransferBuffer(gpu,&bi);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    gpu_display_render(d,gpu,cmd,input,W,H,target,W,H,p,viewport);
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
            if(i/W==H/2) center_row[i%W][c]=x;
        }
        SDL_UnmapGPUTransferBuffer(gpu,download);
    }
    SDL_ReleaseGPUTransferBuffer(gpu,download);
}

static void render(SDL_GPUDevice *gpu, GPUDisplay *d, SDL_GPUTexture *input,
                   SDL_GPUTexture *target, GPUDisplayParams *p, float avg[3], float *peak) {
    render_region(gpu,d,input,target,p,NULL,avg,peak);
}

static float row_amplitude(int cycles, int channel) {
    double re=0,im=0;
    for(int x=0;x<W;x++) {
        double phase=6.283185307179586*cycles*x/W;
        re+=center_row[x][channel]*cos(phase);
        im+=center_row[x][channel]*sin(phase);
    }
    return (float)(2*hypot(re,im)/W);
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
    // A mask smaller than Nyquist must converge to neutral unit energy.
    p.sdr_white_level=1; p.mask_strength=1; p.mask_pitch_px=0.5f;
    for(int type=0;type<3;type++) {
        p.mask_type=type; render(gpu,&d,input,target,&p,avg,&peak);
        for(int c=0;c<3;c++) CHECK(fabsf(avg[c]-0.25f)<0.001f);
    }
    // A fine PVM grille still resolves its RGB fundamental at 2.4 pixels
    // per triad. It must not fade to flat gray or alias its second harmonic.
    p.mask_type=1; p.mask_pitch_px=0.8f;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int c=0;c<3;c++) {
        CHECK(fabsf(avg[c]-0.25f)<0.001f);
        CHECK(row_amplitude(80,c)>0.12f);
        CHECK(row_amplitude(32,c)<0.001f);
    }
    float rgb_row[W][3]; memcpy(rgb_row,center_row,sizeof(rgb_row));
    p.subpixel_layout=2;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int x=0;x<W;x++) for(int c=0;c<3;c++)
        CHECK(fabsf(center_row[x][c]-rgb_row[x][2-c])<0.001f);
    p.subpixel_layout=0;
    // Across resize, coverage stays nonnegative and white-field energy
    // stays constant. Use full periods to avoid partial-cell edge bias.
    const float periods[]={2.0f,2.4f,3.0f,4.0f,6.0f,8.0f,12.0f};
    for(unsigned i=0;i<sizeof(periods)/sizeof(periods[0]);i++) {
        p.mask_pitch_px=periods[i]/3;
        render(gpu,&d,input,target,&p,avg,&peak);
        for(int c=0;c<3;c++) CHECK(fabsf(avg[c]-0.25f)<0.001f);
        for(int x=0;x<W;x++) for(int c=0;c<3;c++) CHECK(center_row[x][c]>=0);
    }
    // Host fitting must use the drawable/panel ratio, not the 256-pixel source.
    // A 1470-point desktop backed at 2x on a 2560-pixel panel is not a 2940-pixel panel.
    GPUDisplayParams fit={.mask_type=1,.mask_pitch_px=2.24f};
    gpu_display_fit_mask(&fit,true,2560.0f/2940,1664.0f/1912,13.25f,7.5f);
    CHECK(fit.mask_pitch_px==2 && fit.mask_row_pitch==2);
    CHECK(fabsf(fit.mask_scale_x-2560.0f/2940)<.00001f && fit.mask_origin_x==13.25f);
    const int widths[]={640,1280,1333,1920,2560};
    for(unsigned i=0;i<sizeof(widths)/sizeof(widths[0]);i++) {
        TVDisplayParams tube={.mask_triads=1070};
        gpu_display_params_from_tv(&fit,&tube,W,H,widths[i],1000);
        gpu_display_fit_mask(&fit,true,1,1,0,0);
        CHECK(fit.mask_pitch_px>=1 && fabsf(3*fit.mask_pitch_px-roundf(3*fit.mask_pitch_px))<.00001f);
    }
    // Integer periods have no low-frequency envelope on a uniform field.
    // Moving/resizing the viewport must not move the screen-anchored mask.
    p.mask_type=1;p.mask_pitch_px=1;p.mask_strength=1;
    gpu_display_fit_mask(&p,true,1,1,0,0);
    render(gpu,&d,input,target,&p,avg,&peak);
    memcpy(rgb_row,center_row,sizeof(rgb_row));
    for(int x=3;x<W;x++) for(int c=0;c<3;c++)
        CHECK(fabsf(center_row[x][c]-center_row[x-3][c])<.001f);
    for(int c=0;c<3;c++) { CHECK(fabsf(avg[c]-.25f)<.001f);CHECK(row_amplitude(1,c)<.001f); }
    // Unequal triad-wire gaps leave a common vertical divider in white.
    // Equal RGB stripe spacing incorrectly cancels this achromatic component.
    float white_min=1e9f,white_max=0;
    for(int x=0;x<W;x++) {
        float white=rgb_row[x][0]+rgb_row[x][1]+rgb_row[x][2];
        white_min=fminf(white_min,white); white_max=fmaxf(white_max,white);
    }
    CHECK(white_max-white_min>.025f);
    SDL_GPUViewport shifted={.x=17,.y=0,.w=151,.h=H,.min_depth=0,.max_depth=1};
    render_region(gpu,&d,input,target,&p,&shifted,avg,&peak);
    for(int x=18;x<167;x++) for(int c=0;c<3;c++)
        CHECK(fabsf(center_row[x][c]-rgb_row[x][c])<.001f);
    p.mask_strength=0; p.ambient_light=0.1f;
    SDL_GPUViewport viewport={.x=W/4,.y=0,.w=W/2,.h=H,.min_depth=0,.max_depth=1};
    render_region(gpu,&d,input,target,&p,&viewport,avg,&peak);
    CHECK(fabsf(avg[0]-(0.015f+0.25f*0.5f))<0.001f);
    // Limited host headroom must preserve RGB ratios and a nonzero
    // highlight slope, rather than clip each channel into white.
    p.ambient_light=0;p.mask_strength=0;p.hdr_headroom=1.25f;p.hdr_gain=4;
    cmd=SDL_AcquireGPUCommandBuffer(gpu);
    ct.clear_color=(SDL_FColor){.9f,.6f,.3f,1};
    pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL);SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(peak<1.25f && peak>1.0f);
    CHECK(fabsf(avg[0]/avg[1]-1.5f)<.003f && fabsf(avg[1]/avg[2]-2.0f)<.003f);
    float first_peak=peak;p.hdr_gain=8;
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(peak>first_peak+.005f && peak<1.25f);
    TVDisplayParams tv={.mask_triads=500,.mask_pitch_px=3};
    GPUDisplayParams scaled;
    gpu_display_params_from_tv(&scaled,&tv,W,H,1500,1125);
    CHECK(fabsf(scaled.mask_pitch_px-1)<0.0001f);
    gpu_display_params_from_tv(&scaled,&tv,W,H,750,563);
    CHECK(fabsf(scaled.mask_pitch_px-0.5f)<0.0001f);
    SDL_ReleaseGPUTexture(gpu,input); SDL_ReleaseGPUTexture(gpu,target); gpu_display_destroy(&d,gpu);
    return failures;
}
