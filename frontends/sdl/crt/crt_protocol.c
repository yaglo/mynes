#include "crt_protocol.h"
#include <string.h>
uint32_t crt_le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
void crt_put32(uint8_t *p, uint32_t x) {
    for (unsigned i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i));
}
uint32_t crt_crc32(const uint8_t *p, size_t n) {
    uint32_t c=~0u;
    while(n--) {
        c^=*p++;
        for(unsigned b=0;b<8;b++) c=(c>>1)^(0xedb88320u & (0u-(c&1u)));
    }
    return ~c;
}
int crt_pack(uint8_t *out, const uint16_t *pixels) {
    uint32_t bits=0; unsigned count=0; size_t j=0;
    for(size_t i=0;i<CRT_PIXELS;i++) {
        if(pixels[i]>511) return -1;
        bits|=(uint32_t)pixels[i]<<count; count+=9;
        while(count>=8) { out[j++]=(uint8_t)bits; bits>>=8; count-=8; }
    }
    return 0;
}
void crt_header(uint8_t *out, uint32_t sequence, const uint8_t *payload) {
    memset(out,0,32); memcpy(out,"CRT1",4);
    out[4]=1; out[5]=1; out[6]=1; out[7]=1;
    crt_put32(out+8,sequence); crt_put32(out+12,CRT_PAYLOAD);
    out[17]=1; out[18]=240;
    crt_put32(out+20,crt_crc32(payload,CRT_PAYLOAD));
    crt_put32(out+24,crt_crc32(out,24));
}
uint32_t crt_audio_bytes(uint8_t format, uint32_t frames) {
    return frames*(format==CRT_FORMAT_MONO?2u:4u);
}
static void packet_header(uint8_t *out, uint8_t kind, uint8_t format, uint32_t sequence, uint32_t word16,
                          const uint8_t *payload, uint32_t length) {
    memset(out,0,32); memcpy(out,"CRT1",4);
    out[4]=1; out[5]=kind; out[6]=format; out[7]=1;
    crt_put32(out+8,sequence); crt_put32(out+12,length); crt_put32(out+16,word16);
    crt_put32(out+20,crt_crc32(payload,length));
    crt_put32(out+24,crt_crc32(out,24));
}
void crt_audio_header(uint8_t *out, uint32_t sequence, uint32_t rate_hz, uint8_t format, const uint8_t *payload, uint32_t frames) {
    packet_header(out,CRT_KIND_AUDIO,format,sequence,rate_hz,payload,crt_audio_bytes(format,frames));
}
void crt_av_header(uint8_t *out, uint32_t sequence, uint8_t format, const uint8_t *payload, uint32_t frames) {
    packet_header(out,CRT_KIND_AV,format,sequence,0x00f00100u,payload,crt_audio_bytes(format,frames)+CRT_PAYLOAD);
}
crt_status crt_status_decode(uint32_t w) {
    crt_status s;
    s.sdram_passed=w&1u; s.sdram_failed=(w>>1)&1u; s.missed_line=(w>>2)&1u;
    s.audio_playing=(w>>3)&1u; s.audio_level=(w>>4)&0x1fffu; s.audio_underruns=(w>>17)&0xffu;
    s.audio_rate_id=(w>>25)&3u; s.audio_muted=(w>>27)&1u;
    return s;
}
uint32_t crt_audio_rate_hz(unsigned rate_id) {
    return rate_id==1?CRT_AUDIO_RATE_NTSC:rate_id==2?CRT_AUDIO_RATE_PAL:0u;
}
int crt_ack_check(const uint8_t *ack, size_t length, uint32_t sequence, uint32_t payload_crc,
                  uint32_t accepted, uint32_t payload_length, crt_status *status) {
    if(length!=32 && length!=CRT_ACK) return 0;
    int video=!memcmp(ack,"ACK2",4);
    if(memcmp(ack,"ACK1",4) && !video) return 0;
    if(crt_le32(ack+4)!=sequence || crt_le32(ack+8)!=payload_crc || crt_le32(ack+12)!=accepted ||
       crt_le32(ack+16)!=0 || crt_le32(ack+20)!=payload_length || crt_le32(ack+24)!=1) return 0;
    if(length==CRT_ACK && crt_le32(ack+36)) return 0;
    crt_status s=crt_status_decode(video?crt_le32(ack+28):0u);
    if(status) *status=s;
    if(video && (!s.sdram_passed || s.sdram_failed || s.missed_line)) return 0;
    if(!video && crt_le32(ack+28)) return 0;
    return 1;
}
int crt_ack_valid(const uint8_t *ack, size_t length, uint32_t sequence, uint32_t payload_crc) {
    return crt_ack_check(ack,length,sequence,payload_crc,sequence+1,CRT_PAYLOAD,NULL);
}
uint32_t crt_usb_safe_frames(uint8_t kind, uint8_t format, uint32_t frames) {
    while(frames && (32u+crt_audio_bytes(format,frames)+(kind==CRT_KIND_AV?CRT_PAYLOAD:0u))%512u==0) frames--;
    return frames;
}
