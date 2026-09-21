/* Receiver colour decoding and emitted-light colourimetry. Decoder axes act
 * on voltage; phosphor primaries act on linear light. Nominal primaries are
 * ITU-T H.273 values 1, 6 and 5, not measured individual P22 tubes. */
#ifndef CRT_COLOR_H
#define CRT_COLOR_H
#include <math.h>
#include "video_chain.h"
#include "fw900_tone.h"

static inline void crt_phosphor_matrix(int gamut, float m[3][3]) {
    static const float matrices[4][3][3] = {
        {{1,0,0},{0,1,0},{0,0,1}},
        {{.939542f,.050181f,.010277f},
         {.017772f,.965793f,.016435f},
         {-.001622f,-.004370f,1.005992f}},
        {{1.044043f,-.044043f,0},
         {0,1,0}, {0,.011793f,.988207f}},
        // NIDL Table II.22.1 effective primaries, normalized to D65.
        {{1.08663571f,-.07951036f,-.00712535f},
         {.04991459f,.92427307f,.02581233f},
         {.01588561f,.02294961f,.96116478f}}
    };
    int g = gamut >= 0 && gamut <= 3 ? gamut : 0;
    for (int i=0;i<3;i++) for (int j=0;j<3;j++) m[i][j]=matrices[g][i][j];
}

static inline void crt_white_drive(const TVDisplayParams *tv, float drive[3]) {
    float t=fmaxf(3200,fminf(tv->color_temperature > 0 ? tv->color_temperature : 6500,12000));
    float x,y;
    // CIE daylight locus; nominal 6500 is anchored to D65. Below 4000 K
    // (the formula's limit), interpolate towards illuminant A.
    float kd=fmaxf(t,4000), k=1000/kd;
    x=kd<=7000 ? .244063f+.09911f*k+2.9678f*k*k-4.6070f*k*k*k
               : .237040f+.24748f*k+1.9018f*k*k-2.0064f*k*k*k;
    y=-3*x*x+2.87f*x-.275f;
    x-=.0000788762f; y-=.00018350f;
    if(t<4000) {
        float a=(4000-t)/(4000-2856.0f);
        x+=a*(.44757f-x); y+=a*(.40745f-y);
    }
    float xyz[3]={x/y,1,(1-x-y)/y};
    static const float to_rgb[3][3]={
        {3.24096994f,-1.53738318f,-.49861076f},
        {-.96924364f,1.87596750f,.04155506f},
        {.05563008f,-.20397696f,1.05697151f}};
    float m[3][3], a[3][4]; crt_phosphor_matrix(tv->phosphor_gamut,m);
    // Solve M * phosphor_light = target linear-sRGB white, Y held at one.
    for(int i=0;i<3;i++) {
        for(int j=0;j<3;j++) a[i][j]=m[i][j];
        a[i][3]=0; for(int j=0;j<3;j++) a[i][3]+=to_rgb[i][j]*xyz[j];
    }
    for(int i=0;i<3;i++) {
        float v=a[i][i]; for(int j=i;j<4;j++) a[i][j]/=v;
        for(int r=0;r<3;r++) if(r!=i) {
            v=a[r][i]; for(int j=i;j<4;j++) a[r][j]-=v*a[i][j];
        }
    }
    float gamma[3]={tv->gamma+tv->phosphor_gamma_offset_r,
        tv->gamma+tv->phosphor_gamma_offset_g,tv->gamma+tv->phosphor_gamma_offset_b};
    float peak_light=tv->monitor_model==1 ? fmaxf(1,fmaxf(a[0][3],fmaxf(a[1][3],a[2][3]))) : 1;
    for(int i=0;i<3;i++) {
        float light=fmaxf(a[i][3],0)/peak_light;
        drive[i]=powf(light,1/fmaxf(gamma[i],1));
        if(tv->monitor_model==1) {
            int k=0;
            while(k<254 && fw900_tone[k+1]<light) k++;
            drive[i]=fminf(1,(k+(light-fw900_tone[k])/(fw900_tone[k+1]-fw900_tone[k]))/255);
        }
    }
}

static inline void crt_decoder_matrix(const TVDisplayParams *tv, bool pal,
    float contrast, float brightness, float chroma_gain, float matrix[3][3], float bias[3]) {
    float base[3][2]={{.9563f,.6210f},{-.2721f,-.6474f},{-1.1070f,1.7046f}};
    if(pal) {
        base[0][0]=1.140f; base[0][1]=0;
        base[1][0]=-.581f; base[1][1]=-.395f;
        base[2][0]=0; base[2][1]=2.032f;
    }
    // Colour-difference gain leaves grey untouched. Zero offsets keep old
    // presets neutral. This is distinct from red gun gain / white balance.
    for(int j=0;j<2;j++) {
        float r=base[0][j]*tv->decoder_red_gain, b=base[2][j]*tv->decoder_blue_gain;
        base[0][j]+=r; base[2][j]+=b;
        base[1][j]-=(.299f*r+.114f*b)/.587f;
    }
    float h=tv->hue_offset*.01745329252f, ch=cosf(h), sh=sinf(h), d[3];
    crt_white_drive(tv,d);
    float gun[3]={tv->r_drive,tv->g_drive,tv->b_drive};
    float cutoff[3]={tv->r_cutoff,tv->g_cutoff,tv->b_cutoff};
    for(int i=0;i<3;i++) {
        d[i]*=gun[i]; matrix[i][0]=d[i]*contrast;
        matrix[i][1]=d[i]*tv->saturation*chroma_gain*(base[i][0]*ch-base[i][1]*sh);
        matrix[i][2]=d[i]*tv->saturation*chroma_gain*(base[i][0]*sh+base[i][1]*ch);
        bias[i]=cutoff[i]+brightness*d[i];
    }
}
#endif
