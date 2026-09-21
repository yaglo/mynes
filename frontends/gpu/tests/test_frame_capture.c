/* File orientation, HDR preservation, channel order and async ownership. */
#include "frame_capture.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"capture FAIL %d: %s\n",__LINE__,#x); failures++; } } while (0)

static bool same_file(const char *a, const char *b) {
    FILE *x=fopen(a,"rb"), *y=fopen(b,"rb");
    bool same=x && y;
    if (same) {
        int c,d;
        do { c=fgetc(x); d=fgetc(y); if(c!=d) same=false; } while(c!=EOF && d!=EOF);
    }
    if(x) fclose(x); if(y) fclose(y);
    return same;
}

int main(void) {
    CHECK(SDL_Init(0));
    char directory[]="/tmp/mynes-capture-test-XXXXXX";
    CHECK(mkdtemp(directory)!=NULL);
    char a[256],b[256],af[280],bf[280];
    snprintf(a,sizeof(a),"%s/sync.ppm",directory);
    snprintf(b,sizeof(b),"%s/async.ppm",directory);
    snprintf(af,sizeof(af),"%s.linear.pfm",a);
    snprintf(bf,sizeof(bf),"%s.linear.pfm",b);
    /* Top: red, green, blue; bottom: HDR white, black, half white. */
    uint16_t pixels[]={0x3c00,0,0,0x3c00, 0,0x3c00,0,0x3c00, 0,0,0x3c00,0x3c00,
        0x4000,0x4000,0x4000,0x3c00, 0,0,0,0x3c00, 0x3800,0x3800,0x3800,0x3c00};
    FrameCaptureImage image={.pixels=pixels,.width=3,.height=2,.hdr=true,.white_level=1};
    CHECK(frame_capture_write(&image,a));
    FrameCaptureJob *job=frame_capture_start(&image,b);
    CHECK(job!=NULL);
    memset(pixels,0,sizeof(pixels)); // Worker must own both its pixels and path.
    char saved_path[256]; strcpy(saved_path,b); b[0]='X';
    CHECK(frame_capture_finish(job));
    strcpy(b,saved_path);
    CHECK(same_file(a,b)); CHECK(same_file(af,bf));
    FILE *f=fopen(af,"rb");
    if(f) {
        char magic[3]; int w,h; float scale,values[18];
        CHECK(fscanf(f,"%2s\n%d %d\n%f",magic,&w,&h,&scale)==4);
        CHECK(fgetc(f)=='\n');
        CHECK(w==3 && h==2 && strcmp(magic,"PF")==0);
        CHECK(fread(values,sizeof(float),18,f)==18);
        CHECK(values[0]==2 && values[3]==0 && values[6]==.5f); // Bottom row first, HDR unclipped.
        CHECK(values[9]==1 && values[10]==0 && values[14]==0 && values[17]==1);
        fclose(f);
    } else CHECK(false);
    uint8_t bgra[]={0,0,255,255, 0,255,0,255, 255,0,0,255};
    image=(FrameCaptureImage){.pixels=bgra,.width=3,.height=1,.bgra=true,.white_level=1};
    CHECK(frame_capture_write(&image,a));
    f=fopen(a,"rb");
    if(f) {
        char magic[3]; int w,h,max; uint8_t row[9];
        CHECK(fscanf(f,"%2s\n%d %d\n%d",magic,&w,&h,&max)==4);
        CHECK(fgetc(f)=='\n'); CHECK(fread(row,1,9,f)==9);
        CHECK(row[0]==255 && row[1]==0 && row[2]==0 && row[4]==255 && row[8]==255);
        fclose(f);
    } else CHECK(false);
    char bad_path[256]; snprintf(bad_path,sizeof(bad_path),"%s/missing/image.ppm",directory);
    job=frame_capture_start(&image,bad_path); CHECK(job!=NULL);
    CHECK(!frame_capture_finish(job)); // Deferred I/O errors must reach the caller.
    CHECK(frame_capture_finish(NULL));
    unlink(a); unlink(b); unlink(af); unlink(bf); rmdir(directory);
    SDL_Quit();
    return failures ? 1 : 0;
}
