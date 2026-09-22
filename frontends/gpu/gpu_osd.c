#include "gpu_osd.h"
#include <math.h>

static uint32_t color(uint8_t index, uint8_t alpha) {
    unsigned r=220,g=220,b=220;
    if (index==0x0f) r=g=b=0;
    else if (index==0x00) r=g=b=110;
    else if (index==0x10) r=g=b=180;
    else if (index==0x2c) { r=80;g=210;b=170; }
    return r | (g<<8) | (b<<16) | ((uint32_t)alpha<<24);
}
static void rect(uint32_t *rgba,int x,int y,int w,int h,uint32_t value) {
    for(int yy=y;yy<y+h;yy++) for(int xx=x;xx<x+w;xx++)
        if((unsigned)xx<256 && (unsigned)yy<240) rgba[yy*256+xx]=value;
}
static void fill(uint32_t *rgba,int x,int y,int w,int h,uint8_t index) {
    rect(rgba,x,y,w,h,color(index,index==0x0f ? 112 : 255));
}
static void text(uint32_t *rgba,int x,int y,const char *value,uint8_t index,int scale) {
    for(;*value;value++,x+=6*scale) {
        unsigned c=(unsigned char)*value;
        if(c>127) continue;
        if(c>='a' && c<='z') c-=32;
        /* Keyed black outline keeps translucent OSD text readable on whites. */
        for(int pass=0;pass<2;pass++)
            for(int row=0;row<7;row++) for(int col=0;col<5;col++)
                if(osd_font5x7[c][row] & (0x10>>col)) {
                    int px=x+col*scale,py=y+row*scale;
                    if(pass==0) {
                        rect(rgba,px-scale,py,3*scale,scale,color(0x0f,255));
                        rect(rgba,px,py-scale,scale,3*scale,color(0x0f,255));
                    } else rect(rgba,px,py,scale,scale,color(index,255));
                }
    }
}
static void adjustment(uint32_t *rgba,const OSDMenuItem *item) {
    const int x=12,y=175,w=232;
    char label[37],value[40];
    snprintf(label,sizeof(label),"%.36s",item->label);
    osd_menu_format_value(item,value,sizeof(value));
    fill(rgba,x,y,w,49,0x0f);
    fill(rgba,x,y,w,1,0x2c);
    text(rgba,x+8,y+5,label,0x30,1);
    bool numeric=item->type==OSD_MI_FLOAT || item->type==OSD_MI_INT;
    if(numeric && item->max_val>item->min_val) {
        float v=item->type==OSD_MI_FLOAT ? *(float *)item->target : (float)*(int *)item->target;
        float t=fminf(1,fmaxf(0,(v-item->min_val)/(item->max_val-item->min_val)));
        fill(rgba,x+8,y+18,120,3,0x00);
        fill(rgba,x+8,y+18,(int)lroundf(t*120),3,0x2c);
    }
    int value_x=numeric ? x+w-8-osd_nesfb_text_width(value,1) : x+8;
    text(rgba,value_x,y+16,value,0x2c,1);
    text(rgba,x+8,y+29,"L/R ADJUST  U/D SETTING",0x10,1);
    text(rgba,x+8,y+38,"ENTER/ESC RETURN  M CLOSE",0x10,1);
}
void gpu_osd_blend_rgb(uint8_t *rgb,const uint32_t *rgba) {
    for(int p=0;p<GPU_OSD_PIXELS;p++) {
        unsigned a=rgba[p]>>24;
        for(int c=0;c<3;c++) {
            unsigned v=(rgba[p]>>(8*c))&255;
            rgb[p*3+c]=(uint8_t)((rgb[p*3+c]*(255-a)+v*a+127)/255);
        }
    }
}

void gpu_osd_preset_notice(uint32_t *rgba, const char *name) {
    gpu_osd_notice(rgba,"PRESET",name);
}

void gpu_osd_notice(uint32_t *rgba, const char *title, const char *name) {
    if (!name || !*name) return;

    char lines[4][37]={{0}};
    int count=0;
    while (*name && count<4) {
        size_t n=strlen(name); if(n>36) n=36;
        if(name[n]) {
            size_t split=n;
            while(split && name[split]!=' ') split--;
            if(split) n=split;
        }
        memcpy(lines[count++],name,n); name+=n;
        while(*name==' ') name++;
    }
    int height=19+count*9, y=224-height;
    fill(rgba,12,y,232,height,0x0f);
    fill(rgba,12,y,232,1,0x2c);
    text(rgba,20,y+5,title,0x2c,1);
    for(int i=0;i<count;i++) text(rgba,20,y+16+i*9,lines[i],0x30,1);
}

void gpu_osd_performance(uint32_t *rgba,const char *stats) {
    if(!stats || !*stats) return;
    /* Wrap exceptional long timings rather than running beyond the tube. */
    char lines[4][37]={{0}};
    int count=0;
    while(*stats && count<4) {
        size_t n=strlen(stats); if(n>36) n=36;
        if(stats[n]) {
            size_t split=n;
            while(split && stats[split]!=' ') split--;
            if(split) n=split;
        }
        memcpy(lines[count++],stats,n); stats+=n;
        while(*stats==' ') stats++;
    }
    int height=10+count*9,y=224-height;
    fill(rgba,12,y,232,height,0x0f);
    fill(rgba,12,y,232,1,0x2c);
    for(int i=0;i<count;i++) text(rgba,20,y+5+i*9,lines[i],0x30,1);
}

void gpu_osd_render(uint32_t *rgba, const OSDMenuLevel *level, bool editing,
                    const char *preset, bool modified,
                    bool pal, const GPURenderCtx *render, float headroom) {
    if (!level) return;
    if (editing && level->selected >= 0 && level->selected < level->count &&
        gpu_osd_editable(&level->items[level->selected])) {
        adjustment(rgba, &level->items[level->selected]);
        return;
    }
    int rows = level->count < 10 ? level->count : 10;
    int height = 98 + rows * 12, x=12, y=(240-height)/2, width=232;
    int first = level->selected - rows/2;
    if (first < 0) first=0;
    if (first+rows > level->count) first=level->count-rows;
    fill(rgba,x,y,width,height,0x0f);
    fill(rgba,x,y,width,1,0x2c);
    fill(rgba,x,y+height-1,width,1,0x2c);
    char line[64];
    snprintf(line,sizeof(line),"%.34s",level->title ? level->title : "SETUP");
    text(rgba,x+8,y+7,line,0x30,1);
    snprintf(line,sizeof(line),"%s%.34s",modified ? "* " : "",preset ? preset : "Custom");
    text(rgba,x+8,y+18,line,0x10,1);
    snprintf(line,sizeof(line),"GPU / %s / %s %.2fX",pal ? "PAL" : "NTSC",
        render->hdr_enabled ? "HDR" : "SDR",headroom);
    text(rgba,x+8,y+29,line,0x00,1);
    snprintf(line,sizeof(line),"%dX%d / %s / %.0f TRIADS",render->drawable_w,render->drawable_h,
        render->mask_alignment ? "TUBE" : "PIXELS",render->effective_mask_triads);
    text(rgba,x+8,y+40,line,0x10,1);
    snprintf(line,sizeof(line),"%s",render->offscreen_w ? "OFFSCREEN DRAWABLE PIXELS" : render->output_geometry.native_known
        ? (render->output_geometry.resampled ? "SCALED DESKTOP / F NATIVE FULLSCREEN" : "NATIVE PANEL PIXELS")
        : "DRAWABLE PIXELS / PANEL UNKNOWN");
    if (render->presentation_mode == GPU_PRESENT_BFI) {
        if (render->presentation_slots>1)
            snprintf(line,sizeof(line),"BFI %DX / %.1f HZ / DIM %.2f",render->presentation_slots,
                render->presentation_hz,render->dark_frame_level);
        else
            snprintf(line,sizeof(line),"BFI INACTIVE / %s",render->presentation_blocked ? "CADENCE TOO SLOW" : "NEEDS MATCHED HIGH HZ");
    } else if (render->presentation_mode == GPU_PRESENT_60HZ) {
        snprintf(line,sizeof(line),"60 HZ HOLD / NO DARK REFRESH");
    }
    text(rgba,x+8,y+51,line,0x00,1);
    for(int row=0;row<rows;row++) {
        int i=first+row, ry=y+65+row*12;
        bool selected=i==level->selected;
        const OSDMenuItem *item=&level->items[i];
        if(selected) fill(rgba,x+4,ry-2,width-8,11,0x00);
        char value[32];
        osd_menu_format_value(item,value,sizeof(value));
        if(item->type==OSD_MI_SUBMENU) snprintf(value,sizeof(value),">");
        int value_width=osd_nesfb_text_width(value,1);
        int label_chars=(width-24-value_width-(value_width ? 8 : 0))/6;
        snprintf(line,sizeof(line),"%.*s",label_chars,item->label);
        text(rgba,x+10,ry,line,selected ? 0x30 : 0x10,1);
        text(rgba,x+width-10-value_width,ry,value,selected ? 0x2c : 0x10,1);
    }
    if(level->count>rows) {
        snprintf(line,sizeof(line),"%d/%d",level->selected+1,level->count);
        text(rgba,x+width-8-osd_nesfb_text_width(line,1),y+7,line,0x10,1);
    }
    text(rgba,x+8,y+height-22,"ARROWS SELECT / ENTER EDIT",0x10,1);
    text(rgba,x+8,y+height-11,"M CLOSE / ESC BACK",0x00,1);
}
