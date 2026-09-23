/* Causality and landing-map checks through the actual CRT compute stages. */
#include "video_gpu.h"
#include "post_pipeline.h"
#include "signal_precompute.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CRT load FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)

int test_crt_load(SDL_GPUDevice *gpu) {
    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp,0);
    /* The map is built at one output pixel per dot of the 256-wide face; the
     * picture sits inside the receiver's active line, so pixel x lands on a
     * dot of the picture as the raster window says. */
    float active_dots, picture_left, active_lines, picture_top;
    post_pipeline_raster_window(0, sp.samples_per_pixel, &active_dots, &picture_left, &active_lines, &picture_top);
    #define LANDED(px) ((((px)+.5f)/256.f*active_dots-picture_left)*sp.samples_per_pixel)
    video_chain_init_preset(&c,VIDEO_CONN_COMPOSITE,VIDEO_COMB_NONE,0);
    memset(&c.tv,0,sizeof(c.tv)); c.tv.gamma=1; c.tv.h_size=c.tv.v_size=1;
    c.console_psu_hum=0;
    CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
    CHECK(video_gpu_set_beam_params(&v,gpu,256,240,1,.2f,.7f));
    for(int i=0;i<v.sig_chain.num_stages;i++) chain_set_stage_enabled(&v.sig_chain,i,false);
    chain_set_stage_enabled(&v.sig_chain,v.stage_crt_load,true);
    chain_set_stage_enabled(&v.sig_chain,v.stage_crt_supply,true);
    chain_set_stage_enabled(&v.sig_chain,v.stage_deflection,true);
    size_t floats=v.rgb_size/sizeof(float), map_count=256*240+241;
    float *input=malloc(v.rgb_size), *output=malloc(v.rgb_size), *load=malloc(map_count*sizeof(float));
    float *landing=malloc(256*240*4*sizeof(float)), *focus=malloc(256*240*4*sizeof(float));
    float before[2]={0}, after[2]={0};
    for(int pattern=0;pattern<2;pattern++) {
        for(size_t i=0;i<floats;i++) {
            int dot=(int)(i/3)%sp.samples_per_line/sp.samples_per_pixel;
            input[i]=pattern && dot>=64 && dot<128 ? 1 : .25f;
        }
        c.tv.beam_current_load=.2f;
        video_gpu_reset_temporal_state(&v,gpu);
        CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));
        CHECK(chain_run(&v.sig_chain,gpu));
        CHECK(gpu_buffer_download(gpu,v.buf_rgb,output,v.rgb_size));
        CHECK(gpu_buffer_download(gpu,v.buf_crt_load,load,map_count*sizeof(float)));
        before[pattern]=output[(120*sp.samples_per_line+32*sp.samples_per_pixel)*3];
        after[pattern]=output[(120*sp.samples_per_line+140*sp.samples_per_pixel)*3];
        CHECK(load[120*256+127]>load[120*256+32]);
        CHECK(load[256*240+120] > .15f);
        CHECK(gpu_buffer_download(gpu,v.buf_deflection_x,landing,256*240*4*sizeof(float)));
        CHECK(fabsf(landing[(120*256+192)*4]-LANDED(192))<.01f);
    }
    CHECK(fabsf(before[1]-before[0])<1e-6f); // A future patch cannot darken earlier pixels.
    CHECK(after[1]<after[0]-.01f);          // Its rail depletion persists to the right.
    c.tv.beam_current_load=0;
    CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(gpu_buffer_download(gpu,v.buf_rgb,output,v.rgb_size));
    CHECK(memcmp(input,output,v.rgb_size)==0);
    c.tv.video_black_droop=.2f; c.tv.video_recovery_us=18;
    float trails[3]={0};
    for(int patch=0;patch<3;patch++) {
        for(size_t i=0;i<floats;i++) {
            int dot=(int)(i/3)%sp.samples_per_line/sp.samples_per_pixel;
            input[i]=dot>=64 && dot<128 ? (patch==0 ? .5f : patch==1 ? 1 : 0) : .5f;
        }
        CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));
        CHECK(chain_run(&v.sig_chain,gpu));
        CHECK(gpu_buffer_download(gpu,v.buf_rgb,output,v.rgb_size));
        trails[patch]=output[(120*sp.samples_per_line+140*sp.samples_per_pixel)*3];
    }
    CHECK(trails[1]<trails[0]-.02f); // Dark wake after white.
    CHECK(trails[2]>trails[0]+.02f); // Bright wake after black.
    c.tv.video_black_droop=0;
    // Superwhite must produce more load, even though display tone mapping
    // later fits the emitted peaks to the host's available headroom.
    float nominal_load=0;
    for(int white=1;white<=2;white++) {
        for(size_t i=0;i<floats;i++) input[i]=(float)white;
        CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));
        CHECK(chain_run(&v.sig_chain,gpu));
        CHECK(gpu_buffer_download(gpu,v.buf_crt_load,load,map_count*sizeof(float)));
        float measured=load[120*256+255];
        if(white==1) nominal_load=measured;
        else CHECK(fabsf(measured/nominal_load-2)<.001f);
    }
    // An unloaded first-order rail agrees with an independent RC step response.
    // Optional export lets tools/circuits/measure_crt_recovery.py compare ngspice.
    c.tv.beam_current_load=0; c.tv.video_black_droop=0;
    for(size_t i=0;i<floats;i++) {
        int dot=(int)(i/3)%sp.samples_per_line/sp.samples_per_pixel;
        input[i]=dot>=64 && dot<128 ? 1 : 0;
    }
    CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(gpu_buffer_download(gpu,v.buf_crt_load,load,map_count*sizeof(float)));
    double dt=1.0/(signal_region_sample_rate_hz(SIGNAL_REGION_NTSC)/sp.samples_per_pixel);
    const char *export_path=getenv("MYNES_CRT_MEASUREMENTS");
    FILE *csv=export_path ? fopen(export_path,"w") : NULL;
    if(csv) fprintf(csv,"seconds,gpu_rail\n");
    double max_error=0;
    for(int x=0;x<256;x++) {
        double t=(x+1)*dt,charge=x>=64 ? 1-exp(-(t-64*dt)/12e-6) : 0;
        if(x>=128) charge=(1-exp(-64*dt/12e-6))*exp(-(t-128*dt)/12e-6);
        max_error=fmax(max_error,fabs(load[120*256+x]-charge));
        if(csv) fprintf(csv,"%.12g,%.12g\n",t,load[120*256+x]);
    }
    if(csv) fclose(csv);
    printf("CRT RC step maximum error: %.8g\n",max_error);CHECK(max_error<.00001);
    // Restore a nonzero load for the following geometry checks.
    for(size_t i=0;i<floats;i++) input[i]=1;
    CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));CHECK(chain_run(&v.sig_chain,gpu));
    for(int sign=-1;sign<=1;sign+=2) {
        c.tv.hv_sag=sign*.3f; c.tv.focus_breathing=.2f;
        // Freeze the measured load while checking the inverse landing map.
        chain_set_stage_enabled(&v.sig_chain,v.stage_crt_load,false);
        chain_set_stage_enabled(&v.sig_chain,v.stage_crt_supply,false);
        CHECK(chain_run(&v.sig_chain,gpu));
        CHECK(gpu_buffer_download(gpu,v.buf_deflection_x,landing,256*240*4*sizeof(float)));
        CHECK(gpu_buffer_download(gpu,v.buf_deflection_y,focus,256*240*4*sizeof(float)));
        float x=landing[(120*256+192)*4], f=focus[(120*256+192)*4+3];
        CHECK((x-LANDED(192))*sign>1);
        CHECK(f>1.01f);
        v.beam_frame_counter+=100;
        CHECK(chain_run(&v.sig_chain,gpu));
        CHECK(gpu_buffer_download(gpu,v.buf_deflection_y,focus,256*240*4*sizeof(float)));
        CHECK(fabsf(f-focus[(120*256+192)*4+3])<1e-6f);
    }
    c.tv.hv_sag=0; c.tv.focus_breathing=0;
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(gpu_buffer_download(gpu,v.buf_deflection_x,landing,256*240*4*sizeof(float)));
    float original_x=landing[(120*256+192)*4];
    v.beam_frame_counter+=10; v.frame_brightness=.9f;
    CHECK(chain_set_stage_capture(&v.sig_chain,gpu,v.stage_deflection,true));
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(v.sig_chain.stages[v.stage_deflection].reuse_output);
    CHECK(gpu_buffer_download(gpu,v.buf_deflection_x,landing,256*240*4*sizeof(float)));
    CHECK(landing[(120*256+192)*4]==original_x);
    // The visualiser must still receive geometry while its compute is reused.
    CHECK(gpu_buffer_download(gpu,chain_get_stage_capture_buffer(&v.sig_chain,v.stage_deflection),focus,256*240*4*sizeof(float)));
    CHECK(memcmp(landing,focus,256*240*4*sizeof(float))==0);
    // A service control must invalidate reused geometry immediately.
    c.tv.h_pos=.1f;
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(!v.sig_chain.stages[v.stage_deflection].reuse_output);
    CHECK(gpu_buffer_download(gpu,v.buf_deflection_x,landing,256*240*4*sizeof(float)));
    CHECK(fabsf(landing[(120*256+192)*4]-(original_x-.1f*active_dots*sp.samples_per_pixel))<.01f);
    c.tv.h_pos=0;
    c.tv.hv_sag=0; c.tv.focus_breathing=0; c.tv.overscan=.04f;
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(gpu_buffer_download(gpu,v.buf_deflection_x,landing,256*240*4*sizeof(float)));
    // Overscan expands the picture: a right-side pixel samples closer to
    // the source center, and the illuminated raster still fills the tube.
    CHECK(landing[(120*256+192)*4]<LANDED(192)-8);
    CHECK(landing[(120*256+254)*4+3]>.99f);
    // Underscan changes the landing coordinates, not beam intensity through
    // an arbitrary UV-width raster fade. The beam sampler owns source bounds.
    c.tv.overscan=0;c.tv.h_size=.8f;c.tv.v_size=.8f;
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(gpu_buffer_download(gpu,v.buf_deflection_x,landing,256*240*4*sizeof(float)));
    CHECK(landing[(120*256+26)*4+3]>.99f);
    CHECK(landing[(25*256+128)*4+3]>.99f);
    video_gpu_reset_temporal_state(&v,gpu);
    CHECK(!v.deflection_cache_valid);
    CHECK(gpu_buffer_download(gpu,v.buf_crt_load,load,map_count*sizeof(float)));
    for(size_t i=0;i<map_count;i++) CHECK(load[i]==0);
    free(input);free(output);free(load);free(landing);free(focus);
    video_gpu_destroy(&v,gpu);
    return failures;
}
