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

int crt_ack_valid(const uint8_t *ack, size_t length, uint32_t sequence, uint32_t crc) {
    if (length != 32 && length != CRT_ACK) return 0;
    int video = !memcmp(ack, "ACK2", 4);
    return (!memcmp(ack, "ACK1", 4) || video) &&
        crt_le32(ack+4) == sequence && crt_le32(ack+8) == crc &&
        crt_le32(ack+12) == sequence+1 && !crt_le32(ack+16) &&
        crt_le32(ack+20) == CRT_PAYLOAD && crt_le32(ack+24) == 1 &&
        crt_le32(ack+28) == (video ? 1u : 0u) &&
        (length == 32 || !crt_le32(ack+36));
}
