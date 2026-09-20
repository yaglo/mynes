#include "crt_live.h"
#include <libusb.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

struct crt_live {
    libusb_context *ctx;
    libusb_device_handle *usb;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int claimed,started,stop,pending,failed;
    uint64_t sent,replaced;
    uint16_t pixels[CRT_PIXELS];
};
static void *worker(void *arg) {
    crt_live *s=arg;
    uint16_t pixels[CRT_PIXELS];
    uint8_t packet[CRT_PACKET],ack[CRT_ACK];
    uint32_t sequence=0;
    for(;;) {
        pthread_mutex_lock(&s->lock);
        while(!s->stop&&!s->pending) pthread_cond_wait(&s->wake,&s->lock);
        if(s->stop) {pthread_mutex_unlock(&s->lock);break;}
        memcpy(pixels,s->pixels,sizeof(pixels));s->pending=0;
        pthread_mutex_unlock(&s->lock);
        int error=crt_pack(packet+32,pixels),n=0;
        if(!error) {
            crt_header(packet,sequence,packet+32);
            error=libusb_bulk_transfer(s->usb,0x01,packet,sizeof(packet),&n,3000);
            if(!error&&n!=sizeof(packet)) error=-1;
        }
        if(!error) {
            error=libusb_bulk_transfer(s->usb,0x82,ack,sizeof(ack),&n,3000);
            if(!error&&n!=32&&n!=sizeof(ack)) error=-1;
        }
        if(!error) {
            int video=!memcmp(ack,"ACK2",4);
            if(!crt_ack_valid(ack,(size_t)n,sequence,crt_le32(packet+20))) error=-1;
            if(!error&&video&&sequence==0)
                fprintf(stderr,"CRT video image: SDRAM self-test passed; no missed-line flag\n");
        }
        pthread_mutex_lock(&s->lock);
        if(error) s->failed=1;
        else {s->sent++;sequence++;}
        pthread_mutex_unlock(&s->lock);
        if(error) {
            fprintf(stderr,"CRT USB transfer/FPGA validation failed; stream stopped (error %d)\n",error);
            break;
        }
    }
    return NULL;
}
crt_live *crt_live_open(char *error,size_t size) {
    crt_live *s=calloc(1,sizeof(*s));
    if(!s) {snprintf(error,size,"out of memory");return NULL;}
    if(pthread_mutex_init(&s->lock,NULL)) {free(s);snprintf(error,size,"mutex init failed");return NULL;}
    if(pthread_cond_init(&s->wake,NULL)) {pthread_mutex_destroy(&s->lock);free(s);snprintf(error,size,"condition init failed");return NULL;}
    int rc=libusb_init(&s->ctx);
    if(rc) {snprintf(error,size,"libusb init: %s",libusb_error_name(rc));goto fail;}
    libusb_device **devices=NULL;
    ssize_t count=libusb_get_device_list(s->ctx,&devices);
    if(count<0) {snprintf(error,size,"USB enumeration failed");goto fail;}
    for(ssize_t i=0;i<count;i++) {
        struct libusb_device_descriptor d;libusb_device_handle *h=NULL;unsigned char name[128];
        if(libusb_get_device_descriptor(devices[i],&d)||d.idVendor!=0xffff||d.idProduct!=0xfffe) continue;
        if(libusb_open(devices[i],&h)) continue;
        int len=libusb_get_string_descriptor_ascii(h,d.iProduct,name,sizeof(name)-1);
        if(len>=0) name[len]=0;
        if(len<0||strcmp((char *)name,"CRT Tang Bridge")) {libusb_close(h);continue;}
        if(s->usb) {
            libusb_close(h);libusb_free_device_list(devices,1);
            snprintf(error,size,"connect only one CRT Tang Bridge");goto fail;
        }
        s->usb=h;
    }
    libusb_free_device_list(devices,1);
    if(!s->usb) {snprintf(error,size,"CRT Tang Bridge not found");goto fail;}
    if(libusb_get_device_speed(libusb_get_device(s->usb))!=LIBUSB_SPEED_HIGH) {
        snprintf(error,size,"high-speed USB required");goto fail;
    }
    rc=libusb_reset_device(s->usb);
    if(!rc) rc=libusb_set_configuration(s->usb,1);
    if(!rc) {rc=libusb_claim_interface(s->usb,0);if(!rc)s->claimed=1;}
    if(!rc) rc=libusb_control_transfer(s->usb,0x41,0x40,40000,0,NULL,0,1000);
    if(!rc) rc=libusb_control_transfer(s->usb,0x41,0x41,1,0,NULL,0,1000);
    if(rc) {snprintf(error,size,"USB setup: %s",libusb_error_name(rc));goto fail;}
    if(pthread_create(&s->thread,NULL,worker,s)) {snprintf(error,size,"worker creation failed");goto fail;}
    s->started=1;return s;
fail:
    crt_live_close(s);return NULL;
}
int crt_live_submit(crt_live *s,const uint16_t pixels[CRT_PIXELS]) {
    pthread_mutex_lock(&s->lock);
    if(s->failed||s->stop) {pthread_mutex_unlock(&s->lock);return -1;}
    if(s->pending) s->replaced++;
    memcpy(s->pixels,pixels,sizeof(s->pixels));s->pending=1;
    pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->lock);return 0;
}
void crt_live_stats(crt_live *s,uint64_t *sent,uint64_t *replaced) {
    pthread_mutex_lock(&s->lock);*sent=s->sent;*replaced=s->replaced;pthread_mutex_unlock(&s->lock);
}
void crt_live_close(crt_live *s) {
    if(!s) return;
    pthread_mutex_lock(&s->lock);s->stop=1;pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->lock);
    if(s->started) pthread_join(s->thread,NULL);
    if(s->claimed) libusb_release_interface(s->usb,0);
    if(s->usb) libusb_close(s->usb);
    if(s->ctx) libusb_exit(s->ctx);
    pthread_cond_destroy(&s->wake);pthread_mutex_destroy(&s->lock);free(s);
}
