/* Read the actual render target: test light units, output transfer, and mask energy. */
#include "gpu_display.h"
#include "gpu_half.h"
#include "gpu_presentation.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define W 192
#define H 128
static int failures;
static float center_row[W][3];
static float row_mean[H][3];
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
        double sum[3]={0};
        memset(row_mean,0,sizeof(row_mean));
        for(int i=0;i<W*H;i++) for(int c=0;c<3;c++) {
            float x=gpu_half_to_float(v[4*i+c]);
            CHECK(isfinite(x)); sum[c]+=x; *peak=fmaxf(*peak,x);
            if(i/W==H/2) center_row[i%W][c]=x;
            row_mean[i/W][c]+=x/W;
        }
        for(int c=0;c<3;c++) avg[c]=(float)(sum[c]/(W*H));
        SDL_UnmapGPUTransferBuffer(gpu,download);
    }
    SDL_ReleaseGPUTransferBuffer(gpu,download);
}

static void clear_input(SDL_GPUDevice *gpu, SDL_GPUTexture *input, float level) {
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUColorTargetInfo ct={.texture=input,.clear_color={level,level,level,1},.load_op=SDL_GPU_LOADOP_CLEAR,.store_op=SDL_GPU_STOREOP_STORE};
    SDL_GPURenderPass *pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL); SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd)); CHECK(SDL_WaitForGPUIdle(gpu));
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

static void upload_pattern(SDL_GPUDevice *gpu, SDL_GPUTexture *input, int axis, int phase, uint16_t white) {
    SDL_GPUTransferBufferCreateInfo info={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=W*H*8};
    SDL_GPUTransferBuffer *buffer=SDL_CreateGPUTransferBuffer(gpu,&info);
    uint16_t *pixels=SDL_MapGPUTransferBuffer(gpu,buffer,false);
    CHECK(pixels!=NULL);
    if(!pixels) { SDL_ReleaseGPUTransferBuffer(gpu,buffer);return; }
    for(int y=0;y<H;y++) for(int x=0;x<W;x++) {
        bool bright=axis==2 ? x<W/2 : ((axis ? x : y)+phase)%4==0;
        for(int c=0;c<3;c++) pixels[(y*W+x)*4+c]=bright && (axis!=2 || c==0) ? white : 0;
        pixels[(y*W+x)*4+3]=0x3c00;
    }
    SDL_UnmapGPUTransferBuffer(gpu,buffer);
    SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
    SDL_GPUCopyPass *pass=SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo source={.transfer_buffer=buffer,.pixels_per_row=W,.rows_per_layer=H};
    SDL_GPUTextureRegion region={.texture=input,.w=W,.h=H,.d=1};
    SDL_UploadToGPUTexture(pass,&source,&region,false);SDL_EndGPUCopyPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));SDL_ReleaseGPUTransferBuffer(gpu,buffer);
}

/* Published target: Flynn & Badano (1999), Table 1, Hitachi Elite 751.
 * A 320 mm bright disk contains a 10 or 20 mm dark disk. The 400 mm
 * square below is a coordinate domain, NOT a claim about tube dimensions.
 * Isolate effective veiling glare: no beam, mask, room light or tone mapping.
 * The 5% tolerance bounds GPU quadrature/raster error, not source uncertainty. */
static void test_measured_glare(SDL_GPUDevice *gpu, int size) {
    GPUDisplay d;
    if(!gpu_display_init_target(&d,gpu,SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
                               size,size,"shaders/render")) { CHECK(false); return; }
    SDL_GPUTextureCreateInfo ci={.type=SDL_GPU_TEXTURETYPE_2D,.format=SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT,
        .usage=SDL_GPU_TEXTUREUSAGE_SAMPLER|SDL_GPU_TEXTUREUSAGE_COLOR_TARGET,
        .width=size,.height=size,.layer_count_or_depth=1,.num_levels=1};
    SDL_GPUTexture *input=SDL_CreateGPUTexture(gpu,&ci), *target=SDL_CreateGPUTexture(gpu,&ci);
    SDL_GPUTransferBufferCreateInfo bi={.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,.size=size*size*8};
    SDL_GPUTransferBuffer *upload=SDL_CreateGPUTransferBuffer(gpu,&bi);
    bi.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; bi.size=4*4*8;
    SDL_GPUTransferBuffer *download=SDL_CreateGPUTransferBuffer(gpu,&bi);
    CHECK(input && target && upload && download);
    if(!input || !target || !upload || !download) goto cleanup;
    GPUDisplayParams p={.glass_tint=1,.hdr_gain=1,.output_hdr=1,.hdr_headroom=8,.sdr_white_level=1,
        .halation_strength=.048294485894f,.halation_sigma=8.144620194f/400,
        .cathode_gain_r=1,.cathode_gain_g=1,.cathode_gain_b=1};
    float reference=0;
    const float radii[]={0,5,10};
    const float ratios[]={1,25,44};
    /* Exact half encodings for area coverage n/16. */
    const uint16_t coverage[]={0,0x2c00,0x3000,0x3200,0x3400,0x3500,0x3600,0x3700,
        0x3800,0x3880,0x3900,0x3980,0x3a00,0x3a80,0x3b00,0x3b80,0x3c00};
    for(int test=0;test<3;test++) {
        uint16_t *pixels=SDL_MapGPUTransferBuffer(gpu,upload,false);
        CHECK(pixels!=NULL); if(!pixels) break;
        for(int y=0;y<size;y++) for(int x=0;x<size;x++) {
            int covered=0;
            for(int sy=0;sy<4;sy++) for(int sx=0;sx<4;sx++) {
                float dx=(x+(sx+.5f)/4-size*.5f)*400/size;
                float dy=(y+(sy+.5f)/4-size*.5f)*400/size;
                float r2=dx*dx+dy*dy;
                covered+=r2>=radii[test]*radii[test] && r2<=160*160;
            }
            for(int c=0;c<3;c++) pixels[(y*size+x)*4+c]=coverage[covered];
            pixels[(y*size+x)*4+3]=0x3c00;
        }
        SDL_UnmapGPUTransferBuffer(gpu,upload);
        SDL_GPUCommandBuffer *cmd=SDL_AcquireGPUCommandBuffer(gpu);
        SDL_GPUCopyPass *copy=SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src={.transfer_buffer=upload,.pixels_per_row=size,.rows_per_layer=size};
        SDL_GPUTextureRegion region={.texture=input,.w=size,.h=size,.d=1};
        SDL_UploadToGPUTexture(copy,&src,&region,false); SDL_EndGPUCopyPass(copy);
        gpu_display_render(&d,gpu,cmd,input,size,size,target,size,size,&p,NULL);
        copy=SDL_BeginGPUCopyPass(cmd);
        region=(SDL_GPUTextureRegion){.texture=target,.x=size/2-2,.y=size/2-2,.w=4,.h=4,.d=1};
        SDL_GPUTextureTransferInfo dst={.transfer_buffer=download,.pixels_per_row=4,.rows_per_layer=4};
        SDL_DownloadFromGPUTexture(copy,&region,&dst); SDL_EndGPUCopyPass(copy);
        CHECK(SDL_SubmitGPUCommandBuffer(cmd)); CHECK(SDL_WaitForGPUIdle(gpu));
        const uint16_t *result=SDL_MapGPUTransferBuffer(gpu,download,false);
        CHECK(result!=NULL); if(!result) break;
        float center=0;
        for(int i=0;i<16;i++) center+=gpu_half_to_float(result[4*i])/16;
        SDL_UnmapGPUTransferBuffer(gpu,download);
        if(test==0) { reference=center; CHECK(fabsf(reference-1)<.001f); }
        else {
            float ratio=reference/center;
            printf("Measured glare %dpx, %.0fmm disk: GPU %.4f, published %.1f\n",size,2*radii[test],ratio,ratios[test]);
            CHECK(isfinite(ratio) && fabsf(ratio/ratios[test]-1)<.05f);
        }
    }
cleanup:
    if(upload) SDL_ReleaseGPUTransferBuffer(gpu,upload);
    if(download) SDL_ReleaseGPUTransferBuffer(gpu,download);
    if(input) SDL_ReleaseGPUTexture(gpu,input);
    if(target) SDL_ReleaseGPUTexture(gpu,target);
    gpu_display_destroy(&d,gpu);
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
    // FW900's physical grille is far below Nyquist at this window size.
    // It must average to neutral light, including with panel-pixel fitting on.
    p.monitor_model=1;
    GPUDisplayParams fwfit={.monitor_model=1,.mask_pitch_px=.08f};
    gpu_display_fit_mask(&fwfit,true,0,1,1,0,0);
    CHECK(fwfit.mask_pitch_px==.08f);
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int c=0;c<3;c++) CHECK(fabsf(avg[c]-.25f)<.001f);
    CHECK(fabsf(peak-.25f)<.001f);
    p.monitor_model=0;
    p.hdr_gain=8; p.hdr_headroom=1.5f;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(peak<=1.501f);
    p.mask_strength=0; p.hdr_gain=1; p.sdr_white_level=2;
    render(gpu,&d,input,target,&p,avg,&peak); CHECK(fabsf(avg[0]-0.5f)<0.001f);
    // Matte faceplate scatters the phosphor pattern itself, preserving field energy.
    p.sdr_white_level=1; p.hdr_headroom=8; p.mask_type=1;
    p.mask_pitch_px=4; p.mask_strength=1;
    render(gpu,&d,input,target,&p,avg,&peak);
    float glossy=row_amplitude(16,0);
    p.antiglare_blur=.8f;
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(row_amplitude(16,0)<glossy*.7f);
    for(int c=0;c<3;c++) CHECK(fabsf(avg[c]-.25f)<.001f);
    p.antiglare_blur=0; p.chromaticity_drive_shift=1;
    float reference[3];
    p.mask_strength=0; render(gpu,&d,input,target,&p,reference,&peak);
    p.mask_strength=1;
    for(int pitch=1;pitch<=4;pitch*=2) {
        p.mask_pitch_px=(float)pitch;
        render(gpu,&d,input,target,&p,avg,&peak);
        for(int c=0;c<3;c++) CHECK(fabsf(avg[c]-reference[c])<.001f);
    }
    p.chromaticity_drive_shift=0;
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
    // Drawn on panel subpixels (RGB stripe), a one-pixel triad needs no
    // computed stripes: every channel is flat and the panel's own red, green
    // and blue columns form the grille. Energy per channel is unchanged.
    p.mask_type=1; p.panel_subpixels=1; p.mask_pitch_px=1.0f/3;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int c=0;c<3;c++) {
        CHECK(fabsf(avg[c]-0.25f)<0.001f);
        float lo=1e9f,hi=-1e9f;
        for(int x=0;x<W;x++) { lo=fminf(lo,center_row[x][c]); hi=fmaxf(hi,center_row[x][c]); }
        CHECK(hi-lo<0.001f);
    }
    // A two-pixel triad lights one pixel's three subpixels and leaves the
    // next pixel dark: no colour spills into the gap, so clipping at the
    // display's peak cannot tint white.
    p.mask_pitch_px=2.0f/3;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int x=0;x<W;x+=2) for(int c=0;c<3;c++) {
        float lit=fmaxf(center_row[x][c],center_row[x+1][c]);
        float gap=fminf(center_row[x][c],center_row[x+1][c]);
        CHECK(fabsf(lit-0.5f)<0.002f && gap<0.002f);
    }
    for(int x=0;x<W;x++)
        CHECK(fabsf(center_row[x][0]-center_row[x][1])<0.002f && fabsf(center_row[x][2]-center_row[x][1])<0.002f);
    // Near an SDR peak (the shoulder starts at 0.75) the same grille on a 0.6
    // field lowers its contrast instead of clipping: lit pixels stop at 0.75,
    // gaps hold 0.45, and every channel still averages 0.6.
    clear_input(gpu,input,0.6f); p.hdr_headroom=1;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int c=0;c<3;c++) {
        CHECK(fabsf(avg[c]-0.6f)<0.002f);
        for(int x=0;x<W;x++) CHECK(center_row[x][c]<0.752f && center_row[x][c]>0.448f);
    }
    clear_input(gpu,input,0.25f); p.hdr_headroom=8;
    // A four-pixel triad: each colour's stripe moves onto the nearest
    // subpixel of its colour (green is centred by the phase shift), so the
    // light lands at 1.17, 2.50 and 3.83 pixels on an RGB panel and at 1.83,
    // 2.50 and 3.17 on a BGR panel, modulo 4.
    p.mask_pitch_px=4.0f/3;
    const float expected_centre[2][3]={{7.0f/6,2.50f,23.0f/6},{11.0f/6,2.50f,19.0f/6}};
    for(int order=1;order<=2;order++) {
        p.panel_subpixels=order;
        render(gpu,&d,input,target,&p,avg,&peak);
        for(int c=0;c<3;c++) {
            CHECK(fabsf(avg[c]-0.25f)<0.001f);
            for(int x=0;x<W;x++) CHECK(center_row[x][c]>=0);
            double re=0,im=0;
            for(int x=0;x<W;x++) {
                double phase=6.283185307179586*x/4.0;
                re+=center_row[x][c]*cos(phase); im+=center_row[x][c]*sin(phase);
            }
            // Peak pixel index of the channel, then its subpixel centre.
            double peak_index=fmod(atan2(im,re)/6.283185307179586*4.0+8.0,4.0);
            int site=order==1 ? c : 2-c;
            double centre=fmod(peak_index+(2*site+1)/6.0,4.0);
            CHECK(fabs(centre-expected_centre[order-1][c])<0.05);
        }
    }
    p.panel_subpixels=0;
    // Fitting: with subpixels a 2.07-pixel nominal triad becomes two pixels,
    // without them it stays at the three-pixel minimum, and a resampled
    // desktop never gets subpixel drawing.
    GPUDisplayParams sub={.mask_type=1,.mask_pitch_px=2.07f/3};
    gpu_display_fit_mask(&sub,true,1,1,1,0,0);
    CHECK(fabsf(3*sub.mask_pitch_px-2)<.00001f && sub.panel_subpixels==1);
    sub=(GPUDisplayParams){.mask_type=1,.mask_pitch_px=2.07f/3};
    gpu_display_fit_mask(&sub,true,0,1,1,0,0);
    CHECK(fabsf(3*sub.mask_pitch_px-3)<.00001f && sub.panel_subpixels==0);
    sub=(GPUDisplayParams){.mask_type=1,.mask_pitch_px=2.07f/3};
    gpu_display_fit_mask(&sub,true,1,2560.0f/2940,1664.0f/1912,0,0);
    CHECK(sub.panel_subpixels==0 && 3*sub.mask_pitch_px>=3);
    // A 25 um damper wire on a face 0.4 pixels of wire per 128 rows, a
    // quarter of the way down: the band [31.8, 32.2] darkens rows 31 and 32
    // by 0.2 each and leaves every other row alone.
    p.mask_pitch_px=1.0f/3; p.damper_wires=1; p.damper_y[0]=0.25f; p.damper_width=0.4f/H;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int c=0;c<3;c++) {
        float lit=row_mean[20][c];
        CHECK(fabsf(row_mean[31][c]/lit-0.8f)<0.01f);
        CHECK(fabsf(row_mean[32][c]/lit-0.8f)<0.01f);
        CHECK(fabsf(row_mean[30][c]/lit-1.0f)<0.001f);
        CHECK(fabsf(row_mean[33][c]/lit-1.0f)<0.001f);
        CHECK(fabsf(row_mean[96][c]/lit-1.0f)<0.001f);
    }
    // Two wires, and none on a shadow mask.
    p.damper_wires=2; p.damper_y[1]=0.75f;
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(fabsf(row_mean[95][0]/row_mean[20][0]-0.8f)<0.01f);
    p.mask_type=0;
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(fabsf(row_mean[31][0]/row_mean[20][0]-1.0f)<0.01f);
    p.mask_type=1; p.damper_wires=0;

    // Host fitting must use the drawable/panel ratio, not the 256-pixel source.
    // A 1470-point desktop backed at 2x on a 2560-pixel panel is not a 2940-pixel panel.
    GPUDisplayParams fit={.mask_type=1,.mask_pitch_px=2.24f};
    gpu_display_fit_mask(&fit,true,0,2560.0f/2940,1664.0f/1912,13.25f,7.5f);
    CHECK(fit.mask_pitch_px==2 && fit.mask_row_pitch==2);
    CHECK(fabsf(fit.mask_scale_x-2560.0f/2940)<.00001f && fit.mask_origin_x==13.25f);
    const int widths[]={640,1280,1333,1920,2560};
    for(unsigned i=0;i<sizeof(widths)/sizeof(widths[0]);i++) {
        TVDisplayParams tube={.mask_triads=1070};
        gpu_display_params_from_tv(&fit,&tube,W,H,widths[i],1000);
        gpu_display_fit_mask(&fit,true,0,1,1,0,0);
        CHECK(fit.mask_pitch_px>=1 && fabsf(3*fit.mask_pitch_px-roundf(3*fit.mask_pitch_px))<.00001f);
    }
    // Integer periods have no low-frequency envelope on a uniform field.
    // Moving/resizing the viewport must not move the screen-anchored mask.
    p.mask_type=1;p.mask_pitch_px=1;p.mask_strength=1;
    gpu_display_fit_mask(&p,true,0,1,1,0,0);
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
    // The fixed curved aperture clips emission and specular reflection to
    // the glass, with a one-pixel coverage edge rather than a UV-width fade.
    p.mask_strength=0;p.ambient_light=.1f;p.barrel=.8f;p.glass_glare=.2f;
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(fabsf(center_row[0][0]-.015f)<.001f);
    CHECK(center_row[W/2][0]>.25f);
    int partial=0;
    for(int x=0;x<W/2;x++)
        if(center_row[x][0]>.016f && center_row[x][0]<.20f) partial++;
    CHECK(partial<=1);
    p.barrel=0;p.glass_glare=0;
    p.mask_strength=0; p.ambient_light=0.1f;
    SDL_GPUViewport viewport={.x=W/4,.y=0,.w=W/2,.h=H,.min_depth=0,.max_depth=1};
    render_region(gpu,&d,input,target,&p,&viewport,avg,&peak);
    CHECK(fabsf(avg[0]-(0.015f+0.25f*0.5f))<0.001f);
    // Emission gain must not amplify reflected room light or the surround.
    p.hdr_gain=2; p.hdr_headroom=8;
    render_region(gpu,&d,input,target,&p,&viewport,avg,&peak);
    CHECK(fabsf(avg[0]-(0.015f+0.5f*0.5f))<0.001f);
    CHECK(fabsf(center_row[0][0]-.015f)<.001f);
    // Pulse energy and ambient invariance, through the real HDR fragment shader.
    // Keep enough headroom so the peak shoulder does not confound integration.
    p.pulse_enabled=true; p.halation_strength=.3f;
    for (int slots=2; slots<=4; slots++) for (int dim=0; dim<=2; dim++) {
        float integrated=0;
        for (int slot=0; slot<slots; slot++) {
            p.pulse_gain=gpu_presentation_gain(slots,slot,dim*.25f);
            p.reuse_halation=slot>0;
            render_region(gpu,&d,input,target,&p,&viewport,avg,&peak);
            integrated+=avg[0]/slots;
            CHECK(fabsf(center_row[0][0]-.015f)<.001f);
            if (slot>0 && dim==0) CHECK(fabsf(avg[0]-.015f)<.001f);
        }
        CHECK(fabsf(integrated-.265f)<.002f);
    }
    p.pulse_enabled=false; p.reuse_halation=false; p.halation_strength=0;
    p.ambient_light=0;p.mask_strength=1;p.mask_type=1;
    for(int pitch=1;pitch<=4;pitch*=2) {
        p.mask_pitch_px=(float)pitch;
        p.hdr_headroom=1;
        render(gpu,&d,input,target,&p,avg,&peak);
        float sdr_mean=avg[0],sdr_peak=peak;
        p.hdr_headroom=4;
        render(gpu,&d,input,target,&p,avg,&peak);
        printf("Grille period %d px: SDR mean/peak %.4f/%.4f, EDR %.4f/%.4f\n",
               3*pitch,sdr_mean,sdr_peak,avg[0],peak);
        CHECK(avg[0]>sdr_mean && peak>sdr_peak);
        CHECK(fabsf(avg[0]-.5f)<.002f);
        CHECK(fabsf(avg[0]-avg[2])<.002f);
    }
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

    // Extended-linear sRGB must carry signed components of real phosphor
    // colours to the host colour manager. This green is inside Display P3
    // but needs a negative red coordinate when expressed in sRGB.
    p=(GPUDisplayParams){.glass_tint=1,.hdr_gain=1,.output_hdr=1,.hdr_headroom=8,.sdr_white_level=1,
        .phosphor_gamut=2,.cathode_gain_r=1,.cathode_gain_g=1,.cathode_gain_b=1};
    cmd=SDL_AcquireGPUCommandBuffer(gpu);ct.clear_color=(SDL_FColor){0,1,0,1};
    pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL);SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    render(gpu,&d,input,target,&p,avg,&peak);
    printf("Extended phosphor green: %.7f %.7f %.7f\n",avg[0],avg[1],avg[2]);
    CHECK(fabsf(avg[0]+.044043f)<.0001f && fabsf(avg[1]-1)<.0001f && fabsf(avg[2]-.011793f)<.0001f);
    float reference_color[3];memcpy(reference_color,avg,sizeof(avg));
    p.hdr_gain=8;p.hdr_headroom=1.5f;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int ch=0;ch<3;ch++) CHECK(fabsf(avg[ch]/avg[1]-reference_color[ch])<.0003f);
    // SDR gamut fitting must preserve luminance and the chroma direction.
    // SDR output ignores emission gain, so halve the phosphor drive instead.
    cmd=SDL_AcquireGPUCommandBuffer(gpu);ct.clear_color=(SDL_FColor){0,.5f,0,1};
    pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL);SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    p.hdr_gain=8;p.output_hdr=0;p.hdr_headroom=1;
    render(gpu,&d,input,target,&p,avg,&peak);
    float linear[3];
    for(int ch=0;ch<3;ch++) {
        CHECK(avg[ch]>=0 && avg[ch]<=1);
        linear[ch]=avg[ch]<=.04045f ? avg[ch]/12.92f : powf((avg[ch]+.055f)/1.055f,2.4f);
    }
    float expected_y=.5f*(-.044043f*.2126f+.7152f+.011793f*.0722f);
    CHECK(fabsf(linear[0]*.2126f+linear[1]*.7152f+linear[2]*.0722f-expected_y)<.001f);
    CHECK(fabsf(linear[0])<.0001f);

    // Purity faults and cross-excitation cannot generate light without a
    // beam. Redistribution conserves nominal excitation before the mask.
    p.output_hdr=1;p.hdr_gain=1;p.hdr_headroom=8;p.phosphor_gamut=0;
    p.degauss_tint=1;p.secondary_scatter=.3f;
    cmd=SDL_AcquireGPUCommandBuffer(gpu);ct.clear_color=(SDL_FColor){0,0,0,1};
    pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL);SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    render(gpu,&d,input,target,&p,avg,&peak);CHECK(peak==0);
    cmd=SDL_AcquireGPUCommandBuffer(gpu);ct.clear_color=(SDL_FColor){1,0,0,1};
    pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL);SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(fabsf(avg[0]+avg[1]+avg[2]-1)<.001f);
    for(int x=0;x<W;x++) for(int ch=0;ch<3;ch++) CHECK(center_row[x][ch]>=0);
    // At full cross-excitation a red beam excites all three sites equally.
    // The receiving site's mask must still modulate each channel.
    p.degauss_tint=0;p.secondary_scatter=1;p.mask_strength=1;p.mask_type=1;p.mask_pitch_px=4;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int ch=0;ch<3;ch++) CHECK(fabsf(avg[ch]-1.0f/3)<.002f);
    CHECK(row_amplitude(16,0)>.3f);
    // A broad glass halo sees the integrated scanline energy, not whichever
    // fine row happens to coincide with its quarter-resolution sample grid.
    p=(GPUDisplayParams){.glass_tint=1,.hdr_gain=1,.output_hdr=1,.hdr_headroom=8,.sdr_white_level=1,
        .halation_strength=1,.cathode_gain_r=1,.cathode_gain_g=1,.cathode_gain_b=1};
    for(int fractional=0;fractional<2;fractional++) {
        if(fractional) gpu_display_resize(&d,gpu,W-3,H-3);
        for(int axis=0;axis<2;axis++) for(int phase=0;phase<4;phase++) {
            upload_pattern(gpu,input,axis,phase,0x3c00);
            render(gpu,&d,input,target,&p,avg,&peak);
            printf("Halo stripe fractional %d axis %d phase %d: mean %.6f\n",fractional,axis,phase,avg[0]);
            CHECK(fabsf(avg[0]-.25f)<.001f);
        }
    }
    // Legacy encoded input must become light before footprint averaging.
    gpu_display_resize(&d,gpu,W,H);
    p.input_gamma=2;
    upload_pattern(gpu,input,0,0,0x3800);
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(fabsf(avg[0]-.0625f)<.0001f);
    // Reflection alone must illuminate a neighbouring dark region, retain
    // the source colour, and redistribute rather than create field energy.
    p.input_gamma=0;p.halation_strength=0;p.glass_reflection=1;
    upload_pattern(gpu,input,2,0,0x3c00);
    render(gpu,&d,input,target,&p,avg,&peak);
    CHECK(fabsf(avg[0]-.5f)<.001f && avg[1]==0 && avg[2]==0);
    CHECK(center_row[W/2][0]>.005f && center_row[W-1][0]==0);
    // Wavelength-dependent scatter fractions must leave a uniform field
    // neutral, even for extreme user tint values and high HDR headroom.
    p.glass_reflection=0;p.halation_strength=.3f;
    p.halation_tint_r=.1f;p.halation_tint_g=2;p.halation_tint_b=4;
    cmd=SDL_AcquireGPUCommandBuffer(gpu);ct.clear_color=(SDL_FColor){.25f,.25f,.25f,1};
    pass=SDL_BeginGPURenderPass(cmd,&ct,1,NULL);SDL_EndGPURenderPass(pass);
    CHECK(SDL_SubmitGPUCommandBuffer(cmd));
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int ch=0;ch<3;ch++) CHECK(fabsf(avg[ch]-.25f)<.001f);
    p.halation_sigma=.03f;
    render(gpu,&d,input,target,&p,avg,&peak);
    for(int ch=0;ch<3;ch++) CHECK(fabsf(avg[ch]-.25f)<.001f);
    TVDisplayParams tv={.mask_triads=500,.mask_pitch_px=3};
    GPUDisplayParams scaled;
    gpu_display_params_from_tv(&scaled,&tv,W,H,1500,1125);
    CHECK(fabsf(scaled.mask_pitch_px-1)<0.0001f);
    gpu_display_params_from_tv(&scaled,&tv,W,H,750,563);
    CHECK(fabsf(scaled.mask_pitch_px-0.5f)<0.0001f);
    SDL_ReleaseGPUTexture(gpu,input); SDL_ReleaseGPUTexture(gpu,target); gpu_display_destroy(&d,gpu);
    test_measured_glare(gpu,512);
    test_measured_glare(gpu,1024);
    return failures;
}
