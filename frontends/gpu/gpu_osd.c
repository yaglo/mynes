#include "gpu_osd.h"

void gpu_osd_render(uint8_t *rgb, uint16_t *codes, const uint8_t (*palette)[3],
                    const OSDMenuLevel *level, const char *preset, bool modified,
                    bool pal, const GPURenderCtx *render) {
    if (!level) return;
    OSDNesFB fb = {.rgb=rgb,.idx=codes,.pal=palette};
    int rows = level->count < 10 ? level->count : 10;
    int height = 98 + rows * 12, x=12, y=(240-height)/2, width=232;
    int first = level->selected - rows/2;
    if (first < 0) first=0;
    if (first+rows > level->count) first=level->count-rows;
    osd_nesfb_fill(&fb,x,y,width,height,0x0f);
    osd_nesfb_fill(&fb,x,y,width,1,0x2c);
    osd_nesfb_fill(&fb,x,y+height-1,width,1,0x2c);
    char text[64];
    snprintf(text,sizeof(text),"%.34s",level->title ? level->title : "SETUP");
    osd_nesfb_text(&fb,x+8,y+7,text,0x30,1);
    snprintf(text,sizeof(text),"%s%.34s",modified ? "* " : "",preset ? preset : "Custom");
    osd_nesfb_text(&fb,x+8,y+18,text,0x10,1);
    snprintf(text,sizeof(text),"GPU / %s / %s %.2fX",pal ? "PAL" : "NTSC",
        render->hdr_enabled ? "HDR" : "SDR",gpu_render_headroom(render));
    osd_nesfb_text(&fb,x+8,y+29,text,0x00,1);
    snprintf(text,sizeof(text),"%dX%d / %s / %.0f TRIADS",render->drawable_w,render->drawable_h,
        render->mask_alignment ? "TUBE" : "PIXELS",render->effective_mask_triads);
    osd_nesfb_text(&fb,x+8,y+40,text,0x10,1);
    snprintf(text,sizeof(text),"%s",render->offscreen_w ? "OFFSCREEN DRAWABLE PIXELS" : render->output_geometry.native_known
        ? (render->output_geometry.resampled ? "SCALED DESKTOP / F NATIVE FULLSCREEN" : "NATIVE PANEL PIXELS")
        : "DRAWABLE PIXELS / PANEL UNKNOWN");
    osd_nesfb_text(&fb,x+8,y+51,text,0x00,1);
    for(int row=0;row<rows;row++) {
        int i=first+row, ry=y+65+row*12;
        bool selected=i==level->selected;
        const OSDMenuItem *item=&level->items[i];
        if(selected) osd_nesfb_fill(&fb,x+4,ry-2,width-8,11,0x00);
        char value[32];
        osd_menu_format_value(item,value,sizeof(value));
        if(item->type==OSD_MI_SUBMENU) snprintf(value,sizeof(value),">");
        int value_width=osd_nesfb_text_width(value,1);
        int label_chars=(width-24-value_width-(value_width ? 8 : 0))/6;
        snprintf(text,sizeof(text),"%.*s",label_chars,item->label);
        osd_nesfb_text(&fb,x+10,ry,text,selected ? 0x30 : 0x10,1);
        osd_nesfb_text(&fb,x+width-10-value_width,ry,value,selected ? 0x2c : 0x10,1);
    }
    if(level->count>rows) {
        snprintf(text,sizeof(text),"%d/%d",level->selected+1,level->count);
        osd_nesfb_text(&fb,x+width-8-osd_nesfb_text_width(text,1),y+7,text,0x10,1);
    }
    osd_nesfb_text(&fb,x+8,y+height-22,"ARROWS ADJUST / ENTER OPEN",0x10,1);
    osd_nesfb_text(&fb,x+8,y+height-11,"M CLOSE / ESC BACK",0x00,1);
}
