#include "crt_live.h"
#include <libusb.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define AUDIO_RING 32768u   /* mono frames, about 0.7 s at 47.9 kHz */
/* The box queues 4,096 frames. Audio goes out only while the projected queue
 * level plus the frames being sent stays at or below this limit, so a host
 * that runs ahead (fast-forward, bursts) can never overflow the box: it would
 * reject the packet and the stream would stop. The 496-frame margin covers the
 * box's one-in-64 drift correction and clock tolerance between reports.
 * Samples the host cannot hold are dropped and counted instead. */
#define BOX_AUDIO_LIMIT 3600.0
/* While frames arrive, audio rides in the next video packet (kind 3). Only
 * when no frame has been submitted for this long does audio go out alone. */
#define VIDEO_IDLE_S 0.05

struct crt_live {
    libusb_context *ctx;
    libusb_device_handle *usb;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    int claimed,started,stop,pending,failed;
    uint64_t sent,replaced,audio_packets;
    uint32_t packets,audio_rate,audio_sequence;
    unsigned audio_level,audio_underruns,audio_playing;
    double audio_level_time;   /* when audio_level was reported */
    double video_time;         /* when a frame was last submitted; 0 = never */
    size_t audio_dropped,audio_head,audio_tail;
    int16_t audio[AUDIO_RING];
    uint16_t pixels[CRT_PIXELS];
};

static size_t audio_count(const crt_live *s) {
    return (s->audio_head+AUDIO_RING-s->audio_tail)%AUDIO_RING;
}
static double monotonic(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9;
}
/* Box queue level now, projected from the last report: it drains at the
 * audio rate while playing and holds while priming. Caller holds the lock. */
static double box_level(const crt_live *s) {
    double level=s->audio_level;
    if(s->audio_playing) level-=(monotonic()-s->audio_level_time)*s->audio_rate;
    return level>0?level:0;
}
/* Frames that may go out now: queued, within one packet, and within the box's room. */
static unsigned audio_sendable(const crt_live *s) {
    if(!s->audio_rate) return 0;
    double room=BOX_AUDIO_LIMIT-box_level(s);
    size_t n=audio_count(s);
    if(n>CRT_AUDIO_MAX_MONO) n=CRT_AUDIO_MAX_MONO;
    if(room<(double)n) n=room>0?(size_t)room:0;
    return (unsigned)n;
}
static int video_idle(const crt_live *s) {
    return s->video_time==0||monotonic()-s->video_time>VIDEO_IDLE_S;
}
static void record_status(crt_live *s, const crt_status *status) {
    s->audio_level=status->audio_level;s->audio_underruns=status->audio_underruns;
    s->audio_playing=status->audio_playing;s->audio_level_time=monotonic();
}
/* Moves n queued samples into the packet as mono 16-bit little-endian. */
static void take_audio(crt_live *s, uint8_t *out, unsigned n) {
    for(unsigned i=0;i<n;i++) {
        int16_t v=s->audio[s->audio_tail];s->audio_tail=(s->audio_tail+1)%AUDIO_RING;
        out[2*i]=(uint8_t)v;out[2*i+1]=(uint8_t)((uint16_t)v>>8);
    }
}
/* One packet out, one acknowledgement back, verified. Caller holds no lock. */
static int exchange(crt_live *s, uint8_t *packet, size_t size, uint32_t sequence, uint32_t payload_length, crt_status *status) {
    uint8_t ack[CRT_ACK]; int n=0;
    int error=libusb_bulk_transfer(s->usb,0x01,packet,(int)size,&n,3000);
    if(!error&&n!=(int)size) error=-1;
    if(!error) {
        error=libusb_bulk_transfer(s->usb,0x82,ack,sizeof(ack),&n,3000);
        if(!error&&n!=32&&n!=(int)sizeof(ack)) error=-1;
    }
    if(!error&&!crt_ack_check(ack,(size_t)n,sequence,crt_le32(packet+20),s->packets+1,payload_length,status)) error=-2;
    if(!error) s->packets++;
    return error;
}
static void *worker(void *arg) {
    crt_live *s=arg;
    static uint16_t pixels[CRT_PIXELS];
    static uint8_t packet[CRT_MAX_PACKET];
    uint32_t sequence=0;
    for(;;) {
        pthread_mutex_lock(&s->lock);
        for(;;) {   /* wake for a frame, or for audio alone once video has gone idle */
            if(s->stop||s->pending) break;
            if(audio_count(s)>=CRT_AUDIO_FRAMES&&video_idle(s)&&audio_sendable(s)>=CRT_AUDIO_FRAMES) break;
            if(audio_count(s)>=CRT_AUDIO_FRAMES) {
                struct timespec until;clock_gettime(CLOCK_REALTIME,&until);
                long ns=until.tv_nsec+5000000L;until.tv_sec+=ns/1000000000L;until.tv_nsec=ns%1000000000L;
                pthread_cond_timedwait(&s->wake,&s->lock,&until);
            } else pthread_cond_wait(&s->wake,&s->lock);
        }
        if(s->stop) {pthread_mutex_unlock(&s->lock);break;}
        int video=s->pending;
        unsigned frames=crt_usb_safe_frames(video?CRT_KIND_AV:CRT_KIND_AUDIO,CRT_FORMAT_MONO,audio_sendable(s));
        take_audio(s,packet+32,frames);
        uint32_t aseq=s->audio_sequence;
        if(video) {memcpy(pixels,s->pixels,sizeof(pixels));s->pending=0;}
        else s->audio_sequence++;
        pthread_mutex_unlock(&s->lock);
        int error=0;crt_status status={0};
        uint32_t abytes=crt_audio_bytes(CRT_FORMAT_MONO,frames);
        if(video) {
            error=crt_pack(packet+32+abytes,pixels);
            if(!error) {
                if(frames) crt_av_header(packet,sequence,CRT_FORMAT_MONO,packet+32,frames);
                else crt_header(packet,sequence,packet+32);
                error=exchange(s,packet,32+abytes+CRT_PAYLOAD,sequence,abytes+CRT_PAYLOAD,&status);
                if(!error&&sequence==0) fprintf(stderr,"CRT box: SDRAM self-test passed; no missed-line flag\n");
            }
        } else {
            crt_audio_header(packet,aseq,s->audio_rate,CRT_FORMAT_MONO,packet+32,frames);
            error=exchange(s,packet,32+abytes,aseq,abytes,&status);
        }
        pthread_mutex_lock(&s->lock);
        if(error) s->failed=1;
        else {
            record_status(s,&status);   /* every ACK2 carries the box's audio queue */
            if(frames) s->audio_packets++;
            if(video) {s->sent++;sequence++;}
        }
        pthread_mutex_unlock(&s->lock);
        if(error) {
            fprintf(stderr,"CRT USB transfer/FPGA validation failed; stream stopped (error %d)\n",error);
            break;
        }
    }
    return NULL;
}
static int session_start(crt_live *s, char *error, size_t size) {
    int rc=libusb_reset_device(s->usb);
    if(!rc) rc=libusb_set_configuration(s->usb,1);
    if(!rc&&!s->claimed) {rc=libusb_claim_interface(s->usb,0);if(!rc)s->claimed=1;}
    if(!rc) rc=libusb_control_transfer(s->usb,0x41,0x40,40000,0,NULL,0,1000);
    if(!rc) rc=libusb_control_transfer(s->usb,0x41,0x41,1,0,NULL,0,1000);
    if(rc) snprintf(error,size,"USB setup: %s",libusb_error_name(rc));
    s->packets=0;
    return rc;
}
/* A four-frame silent packet tells us whether the image has an audio path and
 * at which rate it runs. Older images reject it; the session is then restarted
 * so the rejection does not poison the counters. */
static int probe_audio(crt_live *s, char *error, size_t size) {
    uint8_t packet[32+8]={0};crt_status status={0};
    crt_audio_header(packet,0,0,CRT_FORMAT_MONO,packet+32,4);
    int rc=exchange(s,packet,sizeof(packet),0,8,&status);
    if(rc==0) {s->audio_rate=crt_audio_rate_hz(status.audio_rate_id);s->audio_sequence=1;record_status(s,&status);return 0;}
    if(rc!=-2) {snprintf(error,size,"audio probe transfer failed");return rc;}
    s->audio_rate=0;
    return session_start(s,error,size);
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
    if(session_start(s,error,size)) goto fail;
    if(probe_audio(s,error,size)) goto fail;
    if(!s->audio_rate) fprintf(stderr,"CRT box: image has no audio path; audio stays on the host\n");
    if(pthread_create(&s->thread,NULL,worker,s)) {snprintf(error,size,"worker creation failed");goto fail;}
    s->started=1;return s;
fail:
    crt_live_close(s);return NULL;
}
int crt_live_submit(crt_live *s,const uint16_t pixels[CRT_PIXELS]) {
    pthread_mutex_lock(&s->lock);
    if(s->failed||s->stop) {pthread_mutex_unlock(&s->lock);return -1;}
    if(s->pending) s->replaced++;
    memcpy(s->pixels,pixels,sizeof(s->pixels));s->pending=1;s->video_time=monotonic();
    pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->lock);return 0;
}
size_t crt_live_submit_audio(crt_live *s,const int16_t *samples,size_t frames) {
    size_t dropped=0;
    pthread_mutex_lock(&s->lock);
    if(!s->audio_rate||s->failed||s->stop) {pthread_mutex_unlock(&s->lock);return frames;}
    for(size_t i=0;i<frames;i++) {
        size_t next=(s->audio_head+1)%AUDIO_RING;
        if(next==s->audio_tail) {dropped++;continue;}
        s->audio[s->audio_head]=samples[i];s->audio_head=next;
    }
    s->audio_dropped+=dropped;
    if(audio_count(s)>=CRT_AUDIO_FRAMES&&video_idle(s)) pthread_cond_signal(&s->wake);
    pthread_mutex_unlock(&s->lock);
    return dropped;
}
uint32_t crt_live_audio_rate(crt_live *s) {return s->audio_rate;}
void crt_live_stats(crt_live *s,uint64_t *sent,uint64_t *replaced) {
    pthread_mutex_lock(&s->lock);*sent=s->sent;*replaced=s->replaced;pthread_mutex_unlock(&s->lock);
}
void crt_live_audio_stats(crt_live *s,uint64_t *packets,unsigned *level,unsigned *underruns,size_t *dropped) {
    pthread_mutex_lock(&s->lock);
    *packets=s->audio_packets;*level=s->audio_level;*underruns=s->audio_underruns;*dropped=s->audio_dropped;
    pthread_mutex_unlock(&s->lock);
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
