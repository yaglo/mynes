#include "browser.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "nes/osd.h"

static const char *basename_of(const char *path) {
    const char *p=strrchr(path,'/');
    return p ? p+1 : path;
}
static bool is_rom(const char *name) {
    const char *ext=strrchr(name,'.');
    return ext && (!strcasecmp(ext,".nes") || !strcasecmp(ext,".fds"));
}
static bool contains(const char *name,const char *query) {
    size_t n=strlen(query);
    for(;*name;name++) if(!strncasecmp(name,query,n)) return true;
    return !n;
}
static void ensure_visible(Browser *b) {
    if(b->selected>=b->visible_count) b->selected=b->visible_count-1;
    if(b->selected<0) b->selected=0;
    if(b->scroll_offset>b->selected) b->scroll_offset=b->selected;
    if(b->selected>=b->scroll_offset+BROWSER_VISIBLE_ROWS)
        b->scroll_offset=b->selected-BROWSER_VISIBLE_ROWS+1;
    int max=b->visible_count-BROWSER_VISIBLE_ROWS;
    if(max<0) max=0;
    if(b->scroll_offset>max) b->scroll_offset=max;
}
static void filter_entries(Browser *b) {
    b->visible_count=0;
    int count=b->recent_tab ? (b->config ? b->config->recent_count : 0) : b->entry_count;
    if(b->recent_tab && count>MYNES_RECENT_MAX) count=MYNES_RECENT_MAX;
    for(int i=0;i<count;i++) {
        const char *name=b->recent_tab ? basename_of(b->config->recent_roms[i]) : b->entries[i];
        if(contains(name,b->filter)) b->visible[b->visible_count++]=i;
    }
    b->selected=b->scroll_offset=0;
}
void browser_set_error(Browser *b,const char *message) {
    snprintf(b->error,sizeof(b->error),"%s",message ? message : "Cannot open ROM");
}
typedef struct { char name[256]; bool dir; } Entry;
static int compare_entries(const void *a,const void *c) {
    const Entry *x=a,*y=c;
    if(x->dir!=y->dir) return x->dir ? -1 : 1;
    return strcasecmp(x->name,y->name);
}
static bool load_directory(Browser *b,const char *path) {
    DIR *d=opendir(path);
    if(!d) { browser_set_error(b,strerror(errno)); return false; }
    Entry *entries=calloc(BROWSER_MAX_ENTRIES,sizeof(*entries));
    if(!entries) { closedir(d); browser_set_error(b,"Out of memory"); return false; }
    int count=0;
    bool truncated=false;
    struct dirent *ent;
    while((ent=readdir(d))) {
        if(ent->d_name[0]=='.') continue;
        char full[MYNES_PATH_MAX];
        if(snprintf(full,sizeof(full),"%s/%s",path,ent->d_name)>=(int)sizeof(full)) {
            truncated=true; continue;
        }
        struct stat st;
        if(stat(full,&st)!=0) continue;
        bool dir=S_ISDIR(st.st_mode);
        if(!dir && (!S_ISREG(st.st_mode) || !is_rom(ent->d_name))) continue;
        if(count==BROWSER_MAX_ENTRIES-1) { truncated=true; continue; }
        snprintf(entries[count].name,sizeof(entries[count].name),"%s",ent->d_name);
        entries[count++].dir=dir;
    }
    closedir(d);
    qsort(entries,count,sizeof(*entries),compare_entries);
    snprintf(b->current_dir,sizeof(b->current_dir),"%s",path);
    b->entry_count=0;
    if(strcmp(path,"/")) {
        strcpy(b->entries[0],".."); b->entry_is_dir[0]=true; b->entry_count=1;
    }
    for(int i=0;i<count;i++) {
        int n=b->entry_count++;
        strcpy(b->entries[n],entries[i].name); b->entry_is_dir[n]=entries[i].dir;
    }
    free(entries);
    b->filter[0]=b->error[0]=0; b->truncated=truncated;
    filter_entries(b);
    return true;
}
bool browser_init(Browser *b,const char *start_dir,const MynesConfig *config) {
    if(!b) return false;
    memset(b,0,sizeof(*b)); b->config=config;
    char path[MYNES_PATH_MAX];
    if(start_dir && *start_dir) snprintf(path,sizeof(path),"%s",start_dir);
    else if(config && config->recent_count>0) {
        snprintf(path,sizeof(path),"%s",config->recent_roms[0]);
        char *slash=strrchr(path,'/');
        if(slash) slash[slash==path ? 1 : 0]=0;
        else strcpy(path,".");
    } else strcpy(path,".");
    char *absolute=realpath(path,NULL);
    bool loaded=absolute && strlen(absolute)<sizeof(b->current_dir) && load_directory(b,absolute);
    free(absolute);
    if(!loaded && getcwd(path,sizeof(path))) loaded=load_directory(b,path);
    b->recent_tab=config && config->recent_count>0;
    filter_entries(b);
    if(!loaded) browser_set_error(b,"Cannot read folder");
    return loaded;
}
void browser_refresh(Browser *b) {
    char path[MYNES_PATH_MAX],query[sizeof(b->filter)],selected[256]="";
    snprintf(path,sizeof(path),"%s",b->current_dir);
    snprintf(query,sizeof(query),"%s",b->filter);
    if(!b->recent_tab && b->visible_count)
        snprintf(selected,sizeof(selected),"%s",b->entries[b->visible[b->selected]]);
    int scroll=b->scroll_offset;
    load_directory(b,path);
    snprintf(b->filter,sizeof(b->filter),"%s",query);
    filter_entries(b);
    if(!b->recent_tab) {
        for(int i=0;i<b->visible_count;i++)
            if(!strcmp(selected,b->entries[b->visible[i]])) b->selected=i;
        b->scroll_offset=scroll;
    }
    ensure_visible(b);
}
static void cd_to(Browser *b,const char *name) {
    char path[MYNES_PATH_MAX],previous[MYNES_PATH_MAX];
    snprintf(previous,sizeof(previous),"%s",b->current_dir);
    bool parent=!strcmp(name,"..");
    if(snprintf(path,sizeof(path),"%s/%s",b->current_dir,name)>=(int)sizeof(path)) {
        browser_set_error(b,"Path is too long"); return;
    }
    char *absolute=realpath(path,NULL);
    if(!absolute) { browser_set_error(b,strerror(errno)); return; }
    if(strlen(absolute)>=sizeof(b->current_dir)) browser_set_error(b,"Path is too long");
    else if(load_directory(b,absolute)) {
        b->recent_tab=false; filter_entries(b);
        if(parent) {
            for(int i=0;i<b->visible_count;i++)
                if(!strcmp(b->entries[b->visible[i]],basename_of(previous))) b->selected=i;
        } else if(b->visible_count>1 && !strcmp(b->entries[0],"..")) b->selected=1;
        ensure_visible(b);
    }
    free(absolute);
}
void browser_handle_text(Browser *b,const char *text) {
    if(!b || !text) return;
    size_t n=strlen(b->filter),old=n;
    /* UI font is ASCII. SDL text events respect keyboard layout and repeats. */
    for(;*text && n<sizeof(b->filter)-1;text++)
        if((unsigned char)*text>=32 && (unsigned char)*text<127) b->filter[n++]=*text;
    b->filter[n]=0;
    if(n!=old) { b->error[0]=0; filter_entries(b); }
}
BrowserResult browser_handle_key(Browser *b,BrowserKey key) {
    if(!b) return BROWSER_BROWSING;
    switch(key) {
    case BROWSER_KEY_ESCAPE:
        if(b->filter[0]) { b->filter[0]=0; filter_entries(b); break; }
        return BROWSER_CANCELLED;
    case BROWSER_KEY_TAB:
        b->recent_tab=!b->recent_tab; b->error[0]=0; filter_entries(b); break;
    case BROWSER_KEY_BACK:
        if(b->filter[0]) { b->filter[strlen(b->filter)-1]=0; filter_entries(b); break; }
        /* fall through */
    case BROWSER_KEY_LEFT: cd_to(b,".."); break;
    case BROWSER_KEY_UP: b->selected--; break;
    case BROWSER_KEY_DOWN: b->selected++; break;
    case BROWSER_KEY_PAGEUP: b->selected-=BROWSER_VISIBLE_ROWS; break;
    case BROWSER_KEY_PAGEDOWN: b->selected+=BROWSER_VISIBLE_ROWS; break;
    case BROWSER_KEY_HOME: b->selected=0; break;
    case BROWSER_KEY_END: b->selected=b->visible_count-1; break;
    case BROWSER_KEY_RIGHT:
    case BROWSER_KEY_ENTER: {
        if(!b->visible_count) break;
        int i=b->visible[b->selected];
        if(!b->recent_tab && b->entry_is_dir[i]) { cd_to(b,b->entries[i]); break; }
        if(key==BROWSER_KEY_RIGHT) break;
        if(b->recent_tab) snprintf(b->chosen_path,sizeof(b->chosen_path),"%s",b->config->recent_roms[i]);
        else snprintf(b->chosen_path,sizeof(b->chosen_path),"%s/%s",b->current_dir,b->entries[i]);
        struct stat st;
        if(stat(b->chosen_path,&st)!=0 || !S_ISREG(st.st_mode) || access(b->chosen_path,R_OK)!=0) {
            browser_set_error(b,"ROM missing or unreadable"); break;
        }
        b->error[0]=0;
        return BROWSER_SELECTED;
    }
    }
    ensure_visible(b);
    return BROWSER_BROWSING;
}

/* Preserve beginnings of names, but show the useful tail of folder paths. */
static void elide(char *out,size_t size,const char *in,int width,bool tail) {
    size_t n=strlen(in);
    if(n<=(size_t)width) snprintf(out,size,"%s",in);
    else if(tail) snprintf(out,size,"...%s",in+n-width+3);
    else snprintf(out,size,"%.*s...",width-3,in);
}
void browser_render(const Browser *b,uint8_t *rgb,uint16_t *indices,const uint8_t (*palette)[3]) {
    if(!b || !rgb || !indices || !palette) return;
    OSDNesFB t={.rgb=rgb,.idx=indices,.pal=palette};
    char line[128];
    osd_nesfb_fill(&t,0,0,256,240,0x02);
    osd_nesfb_text(&t,12,10,"MYNES / OPEN ROM",0x30,1);
    snprintf(line,sizeof(line),"%d/%d",b->visible_count ? b->selected+1 : 0,b->visible_count);
    osd_nesfb_text(&t,244-osd_nesfb_text_width(line,1),10,line,0x10,1);
    osd_nesfb_fill(&t,b->recent_tab ? 84 : 12,23,66,12,0x01);
    osd_nesfb_text(&t,18,26,"FILES",b->recent_tab ? 0x10 : 0x30,1);
    osd_nesfb_text(&t,90,26,"RECENT",b->recent_tab ? 0x30 : 0x10,1);
    osd_nesfb_text(&t,178,26,"TAB SWITCH",0x10,1);
    char location[MYNES_PATH_MAX];
    snprintf(location,sizeof(location),"%s",b->current_dir);
    if(b->recent_tab) {
        if(b->visible_count) {
            snprintf(location,sizeof(location),"%s",b->config->recent_roms[b->visible[b->selected]]);
            char *slash=strrchr(location,'/');
            if(slash) slash[slash==location ? 1 : 0]=0;
            else strcpy(location,".");
        } else strcpy(location,"RECENTLY PLAYED");
    }
    elide(line,sizeof(line),location,38,true);
    osd_nesfb_text(&t,12,40,line,0x10,1);
    if(b->filter[0]) snprintf(line,sizeof(line),"FIND: %s_",strlen(b->filter)>30 ? b->filter+strlen(b->filter)-30 : b->filter);
    else strcpy(line,"TYPE TO FILTER...");
    osd_nesfb_text(&t,12,52,line,0x2c,1);
    osd_nesfb_fill(&t,12,63,232,1,0x00);
    int first=b->scroll_offset,last=first+BROWSER_VISIBLE_ROWS;
    if(last>b->visible_count) last=b->visible_count;
    for(int row=first;row<last;row++) {
        int i=b->visible[row],y=69+(row-first)*9;
        bool dir=!b->recent_tab && b->entry_is_dir[i],sel=row==b->selected;
        const char *name=b->recent_tab ? basename_of(b->config->recent_roms[i]) : b->entries[i];
        if(sel) osd_nesfb_fill(&t,10,y-1,232,9,0x01);
        osd_nesfb_text(&t,12,y,sel ? ">" : " ",0x2c,1);
        elide(line,sizeof(line),!strcmp(name,"..") ? "Parent folder" : name,34,false);
        osd_nesfb_text(&t,24,y,line,sel ? 0x30 : dir ? 0x2c : 0x10,1);
        if(dir) osd_nesfb_text(&t,230,y,"/",0x2c,1);
    }
    if(!b->visible_count) {
        osd_nesfb_text(&t,18,83,b->filter[0] ? "NO MATCHES" : b->recent_tab ? "NO RECENT ROMS" : "NO ROMS IN THIS FOLDER",0x30,1);
        osd_nesfb_text(&t,18,97,b->filter[0] ? "ESC CLEARS FILTER" : "TAB SWITCHES FILES / RECENT",0x10,1);
    }
    if(b->visible_count>BROWSER_VISIBLE_ROWS) {
        int h=126*BROWSER_VISIBLE_ROWS/b->visible_count;
        if(h<3) h=3;
        int y=69+(126-h)*first/(b->visible_count-BROWSER_VISIBLE_ROWS);
        osd_nesfb_fill(&t,245,69,1,126,0x00);
        osd_nesfb_fill(&t,245,y,1,h,0x2c);
    }
    osd_nesfb_fill(&t,12,197,232,1,0x00);
    const char *detail=b->error;
    char selected[MYNES_PATH_MAX]="";
    if(!*detail && b->visible_count) {
        int i=b->visible[b->selected];
        snprintf(selected,sizeof(selected),"%s",b->recent_tab ? basename_of(b->config->recent_roms[i]) : b->entries[i]);
        detail=selected;
    }
    elide(line,sizeof(line),detail,76,false);
    char top[39]; snprintf(top,sizeof(top),"%.38s",line);
    osd_nesfb_text(&t,12,201,top,b->error[0] ? 0x2c : 0x30,1);
    if(strlen(line)>38) osd_nesfb_text(&t,12,209,line+38,b->error[0] ? 0x2c : 0x10,1);
    else if(b->truncated && !b->recent_tab) osd_nesfb_text(&t,12,209,"FOLDER LIMIT: SOME ITEMS OMITTED",0x10,1);
    osd_nesfb_text(&t,12,221,"ENTER OPEN  LEFT UP  PGUP/DN PAGE",0x10,1);
    osd_nesfb_text(&t,12,231,b->filter[0] ? "BKSP ERASE / ESC CLEAR FILTER" : b->can_resume ? "HOME/END JUMP / ESC RESUME" : "HOME/END JUMP / ESC QUIT",0x10,1);
}
void browser_render_rgba(const Browser *b,uint32_t *rgba) {
    /* These colors are UI voltage values, independent of the NES palette. */
    static const uint8_t palette[64][3]={
        [0x02]={16,23,30},[0x01]={35,65,76},[0x00]={65,83,92},
        [0x10]={165,184,193},[0x2c]={88,218,187},[0x30]={230,238,240}
    };
    uint8_t rgb[256*240*3]; uint16_t indices[256*240];
    browser_render(b,rgb,indices,palette);
    for(int i=0;i<256*240;i++)
        rgba[i]=rgb[i*3] | ((uint32_t)rgb[i*3+1]<<8) | ((uint32_t)rgb[i*3+2]<<16) | 0xff000000u;
}
