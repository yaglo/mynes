/* Navigation semantics and real GPU alpha mixing for the TV-generated OSD. */
#include "gpu_osd.h"
#include "browser.h"
#include "video_gpu.h"
#include "signal_precompute.h"
#include <stdlib.h>
#include <math.h>

static int failures,changes;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"OSD FAIL %d: %s\n",__LINE__,#x); failures++; } } while(0)
static void changed(void) { changes++; }

int test_osd(SDL_GPUDevice *gpu) {
    float value=.5f; int toggle=0; bool editing=false;
    OSDMenuItem items[]={
        {.label="Brightness",.type=OSD_MI_FLOAT,.target=&value,.step=.1f,.min_val=0,.max_val=1,.on_change=changed},
        {.label="Enabled",.type=OSD_MI_TOGGLE,.target=&toggle,.on_change=changed}
    };
    OSDMenuItem root={.label="Picture",.type=OSD_MI_SUBMENU,.submenu=items,.submenu_count=2};
    osd_menu_open_root(&root,1,"Setup");
    CHECK(gpu_osd_handle_key(SDL_SCANCODE_RETURN,&editing));
    CHECK(!editing && osd_menu_depth==2);
    gpu_osd_handle_key(SDL_SCANCODE_RETURN,&editing);
    CHECK(editing && value==.5f && changes==0); // Enter opens adjustment without changing it.
    gpu_osd_handle_key(SDL_SCANCODE_RIGHT,&editing);
    CHECK(fabsf(value-.6f)<1e-6f && changes==1);
    gpu_osd_handle_key(SDL_SCANCODE_DOWN,&editing);
    CHECK(editing && osd_menu_current()->selected==1 && toggle==0 && changes==1);
    gpu_osd_handle_key(SDL_SCANCODE_DOWN,&editing);
    CHECK(editing && osd_menu_current()->selected==0); // wrap while adjusting
    gpu_osd_handle_key(SDL_SCANCODE_UP,&editing);
    CHECK(editing && osd_menu_current()->selected==1);
    gpu_osd_handle_key(SDL_SCANCODE_UP,&editing);
    CHECK(editing && osd_menu_current()->selected==0 && changes==1);
    gpu_osd_handle_key(SDL_SCANCODE_ESCAPE,&editing);
    CHECK(!editing && osd_menu_is_open && osd_menu_depth==2);
    gpu_osd_handle_key(SDL_SCANCODE_DOWN,&editing);
    gpu_osd_handle_key(SDL_SCANCODE_RETURN,&editing);
    CHECK(editing && toggle==0);
    gpu_osd_handle_key(SDL_SCANCODE_LEFT,&editing);
    CHECK(toggle==1 && changes==2);
    gpu_osd_handle_key(SDL_SCANCODE_RETURN,&editing);
    CHECK(!editing && osd_menu_current()->selected==1);
    gpu_osd_handle_key(SDL_SCANCODE_M,&editing);
    CHECK(!osd_menu_is_open && !editing);

    OSDMenuItem mixed[]={items[0],root};
    osd_menu_open_root(mixed,2,"Mixed");
    gpu_osd_handle_key(SDL_SCANCODE_RETURN,&editing);
    gpu_osd_handle_key(SDL_SCANCODE_DOWN,&editing);
    CHECK(!editing && osd_menu_depth==1 && osd_menu_current()->selected==1);
    gpu_osd_handle_key(SDL_SCANCODE_M,&editing);

    SignalPrecompute sp; VideoChain c; VideoGPUChain v;
    signal_precompute_init(&sp,0);
    video_chain_init_preset(&c,VIDEO_CONN_COMPOSITE,VIDEO_COMB_NONE,0);
    CHECK(video_gpu_init(&v,gpu,&c,"shaders/compute",sp.fir_y,sp.fir_y_n,sp.fir_c,sp.fir_c_n,sp.fir_q,sp.fir_q_n));
    CHECK(v.stage_osd>v.stage_matrix && v.stage_osd<v.stage_post_pipeline);
    for(int i=0;i<v.sig_chain.num_stages;i++) chain_set_stage_enabled(&v.sig_chain,i,false);
    uint32_t *ui=calloc(GPU_OSD_PIXELS,sizeof(uint32_t));
    float *input=malloc(v.rgb_size),*output=malloc(v.rgb_size);
    for(size_t i=0;i<v.rgb_size/sizeof(float);i++) input[i]=.25f;
    for(int connection=VIDEO_CONN_RF;connection<VIDEO_CONN_COUNT;connection++) {
      c.connection=(VideoConnectionType)connection;
      video_gpu_reinit_stages(&v,&c);
      CHECK(v.sig_chain.first_stage==(video_connection_uses_signal_decode(c.connection) ? 0 : v.stage_osd));
      for(int i=0;i<v.sig_chain.num_stages;i++) chain_set_stage_enabled(&v.sig_chain,i,false);
      for(int panel=0;panel<3;panel++) {
        if(panel==0) {
            memset(ui,0,GPU_OSD_PIXELS*sizeof(*ui));
            ui[37*256+20]=0xff0000ffu; // opaque red
            ui[37*256+21]=0x8000ff00u; // translucent green
            ui[37*256+22]=0x00ffffffu; // transparent white must not alter the picture
        } else if(panel==1) {
            memset(ui,0,GPU_OSD_PIXELS*sizeof(*ui));
            gpu_osd_performance(ui,"EMU 2.1 ENC 1.2 PRESENT 0.3");
        } else if(panel==2) {
            Browser *b=calloc(1,sizeof(*b));
            snprintf(b->current_dir,sizeof(b->current_dir),"/Games");
            strcpy(b->entries[0],"Contra.nes");
            b->entry_count=b->visible_count=1;
            browser_render_rgba(b,ui);
            free(b);
        }
        CHECK(video_gpu_set_osd(&v,gpu,ui));
        CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));
        CHECK(chain_run(&v.sig_chain,gpu));
        CHECK(gpu_buffer_download(gpu,v.buf_rgb,output,v.rgb_size));
        /* The overlay lands on the console picture's place in the decode
         * window and leaves the border around it alone. */
        const DecodeWindow *w=&v.window;
        for(size_t i=0;i<decode_window_samples(w);i++) {
            int px=(int)(i%w->width)-w->picture_x, py=(int)(i/w->width)-w->picture_row;
            bool inside=px>=0 && px<w->picture_w && py>=0 && py<240;
            uint32_t rgba=inside ? ui[py*256+px*256/w->picture_w] : 0; float a=(rgba>>24)/255.0f;
            for(int ch=0;ch<3;ch++) {
                float color=((rgba>>(ch*8))&255)/255.0f;
                CHECK(fabsf(output[i*3+ch]-(.25f*(1-a)+color*a))<1e-6f);
            }
        }
      }
    }
    CHECK(video_gpu_set_osd(&v,gpu,NULL));
    CHECK(gpu_buffer_upload(gpu,v.buf_rgb,input,v.rgb_size));
    CHECK(chain_run(&v.sig_chain,gpu));
    CHECK(gpu_buffer_download(gpu,v.buf_rgb,output,v.rgb_size));
    CHECK(memcmp(input,output,v.rgb_size)==0); // closing leaves no stale overlay
    free(ui); free(input); free(output); video_gpu_destroy(&v,gpu);
    return failures;
}
