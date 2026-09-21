#include "browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int failures;
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"browser:%d: %s\n",__LINE__,#x); failures++; } } while(0)
static void touch(const char *dir,const char *name) {
    char path[MYNES_PATH_MAX]; snprintf(path,sizeof(path),"%s/%s",dir,name);
    FILE *f=fopen(path,"wb"); CHECK(f); if(f) fclose(f);
}
int main(void) {
    char tmp[]="/tmp/mynes-browser-test-XXXXXX";
    CHECK(mkdtemp(tmp));
    char *dir=realpath(tmp,NULL); CHECK(dir); if(!dir) return 1;
    char folder[MYNES_PATH_MAX]; snprintf(folder,sizeof(folder),"%s/Folder",dir); CHECK(mkdir(folder,0700)==0);
    touch(dir,"Alpha.nes"); touch(dir,"Zelda.FDS"); touch(dir,"ignore.txt");
    char name[64]; for(int i=0;i<35;i++) { snprintf(name,sizeof(name),"Game %02d.nes",i); touch(dir,name); }
    Browser *b=calloc(1,sizeof(*b));
    CHECK(browser_init(b,dir,NULL));
    CHECK(b->entry_count==39 && b->visible_count==39 && !b->recent_tab);
    CHECK(!strcmp(b->entries[0],"..") && !strcmp(b->entries[1],"Folder"));
    browser_handle_key(b,BROWSER_KEY_PAGEDOWN);
    CHECK(b->selected==BROWSER_VISIBLE_ROWS && b->scroll_offset==1);
    browser_handle_key(b,BROWSER_KEY_END);
    CHECK(b->selected==38 && b->scroll_offset==25);
    browser_handle_key(b,BROWSER_KEY_HOME);
    CHECK(b->selected==0 && b->scroll_offset==0);
    browser_handle_text(b,"zELdA");
    CHECK(b->visible_count==1 && !strcmp(b->entries[b->visible[0]],"Zelda.FDS"));
    CHECK(browser_handle_key(b,BROWSER_KEY_ENTER)==BROWSER_SELECTED);
    CHECK(strstr(b->chosen_path,"/Zelda.FDS"));
    CHECK(browser_handle_key(b,BROWSER_KEY_ESCAPE)==BROWSER_BROWSING && !b->filter[0]);
    browser_handle_text(b,"not present");
    CHECK(b->visible_count==0 && browser_handle_key(b,BROWSER_KEY_ENTER)==BROWSER_BROWSING);
    CHECK(browser_handle_key(b,BROWSER_KEY_ESCAPE)==BROWSER_BROWSING);
    browser_handle_text(b,"Folder");
    CHECK(browser_handle_key(b,BROWSER_KEY_RIGHT)==BROWSER_BROWSING);
    CHECK(!strcmp(b->current_dir,folder) && !b->filter[0]);
    CHECK(browser_handle_key(b,BROWSER_KEY_LEFT)==BROWSER_BROWSING);
    CHECK(!strcmp(b->current_dir,dir) && !strcmp(b->entries[b->visible[b->selected]],"Folder"));
    touch(dir,"New arrival.nes");
    browser_refresh(b);
    CHECK(b->visible_count==40 && !strcmp(b->entries[b->visible[b->selected]],"Folder"));
    browser_handle_text(b,"abc"); browser_handle_key(b,BROWSER_KEY_BACK);
    CHECK(!strcmp(b->filter,"ab") && !strcmp(b->current_dir,dir));
    browser_handle_key(b,BROWSER_KEY_ESCAPE);
    CHECK(browser_handle_key(b,BROWSER_KEY_ESCAPE)==BROWSER_CANCELLED);
    browser_handle_key(b,BROWSER_KEY_TAB);
    CHECK(b->recent_tab && b->visible_count==0);
    CHECK(browser_handle_key(b,BROWSER_KEY_ESCAPE)==BROWSER_CANCELLED); // empty tab must be closable

    MynesConfig cfg={0}; cfg.recent_count=12;
    for(int i=0;i<12;i++) snprintf(cfg.recent_roms[i],MYNES_PATH_MAX,"%s/Game %02d.nes",dir,i);
    CHECK(browser_init(b,NULL,&cfg));
    CHECK(b->recent_tab && b->visible_count==12 && !strcmp(b->current_dir,dir));
    browser_handle_text(b,"03"); CHECK(b->visible_count==1);
    CHECK(browser_handle_key(b,BROWSER_KEY_ENTER)==BROWSER_SELECTED);
    CHECK(strstr(b->chosen_path,"Game 03.nes"));
    CHECK(unlink(b->chosen_path)==0);
    CHECK(browser_handle_key(b,BROWSER_KEY_ENTER)==BROWSER_BROWSING && b->error[0]);
    browser_handle_key(b,BROWSER_KEY_TAB);
    CHECK(b->filter[0] && !b->recent_tab); // query follows the tab
    browser_handle_key(b,BROWSER_KEY_ESCAPE);
    browser_handle_text(b,"Folder");
    CHECK(rmdir(folder)==0);
    CHECK(browser_handle_key(b,BROWSER_KEY_ENTER)==BROWSER_BROWSING && b->error[0]);
    CHECK(!strcmp(b->current_dir,dir)); // failed navigation doesn't lose location

    uint32_t *rgba=calloc(256*240,sizeof(*rgba)); browser_render_rgba(b,rgba);
    bool bright=false,accent=false;
    for(int i=0;i<256*240;i++) {
        CHECK((rgba[i]>>24)==255);
        bright|=rgba[i]==0xfff0eee6u; accent|=rgba[i]==0xffbbda58u;
    }
    CHECK(bright && accent); free(rgba);
    for(int i=0;i<35;i++) { char path[MYNES_PATH_MAX]; snprintf(path,sizeof(path),"%s/Game %02d.nes",dir,i); unlink(path); }
    const char *names[]={"Alpha.nes","Zelda.FDS","ignore.txt","New arrival.nes"};
    for(int i=0;i<4;i++) { char path[MYNES_PATH_MAX]; snprintf(path,sizeof(path),"%s/%s",dir,names[i]); unlink(path); }
    CHECK(rmdir(dir)==0); free(dir); free(b);
    return failures ? 1 : 0;
}
