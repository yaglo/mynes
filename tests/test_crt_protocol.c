#include "crt_protocol.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "Failed line %d: %s\n", __LINE__, #x); return 1; } } while (0)
int main(void) {
    uint16_t pixels[CRT_PIXELS];
    uint8_t payload[CRT_PAYLOAD], header[32], ack[CRT_ACK] = {0};
    CHECK(crt_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    for (size_t i=0; i<CRT_PIXELS; i++) pixels[i]=(uint16_t)(i%512);
    CHECK(crt_pack(payload, pixels) == 0);
    /* Independently extract individual bits, including all emphasis values. */
    for (size_t i=0; i<CRT_PIXELS; i++) {
        unsigned token=0;
        for (unsigned b=0; b<9; b++) {
            size_t bit=i*9+b;
            token |= ((payload[bit/8] >> (bit%8)) & 1u) << b;
        }
        CHECK(token == pixels[i]);
    }
    pixels[CRT_PIXELS-1]=512;
    CHECK(crt_pack(payload, pixels) == -1);
    crt_header(header, 7, payload);
    CHECK(!memcmp(header, "CRT1\1\1\1\1", 8));
    CHECK(crt_le32(header+8) == 7 && crt_le32(header+12) == CRT_PAYLOAD);
    CHECK(header[16] == 0 && header[17] == 1 && header[18] == 240 && header[19] == 0);
    CHECK(crt_le32(header+20) == crt_crc32(payload, sizeof(payload)));
    CHECK(crt_le32(header+24) == crt_crc32(header, 24));
    memcpy(ack, "ACK2", 4);
    crt_put32(ack+4, 7); crt_put32(ack+8, crt_le32(header+20));
    crt_put32(ack+12, 8); crt_put32(ack+20, CRT_PAYLOAD);
    crt_put32(ack+24, 1); crt_put32(ack+28, 1);
    CHECK(crt_ack_valid(ack, 32, 7, crt_le32(header+20)));
    CHECK(crt_ack_valid(ack, 40, 7, crt_le32(header+20)));
    CHECK(!crt_ack_valid(ack, 31, 7, crt_le32(header+20)));
    CHECK(!crt_ack_valid(ack, 40, 8, crt_le32(header+20)));
    CHECK(!crt_ack_valid(ack, 40, 7, 0));
    ack[36]=1; CHECK(!crt_ack_valid(ack, 40, 7, crt_le32(header+20))); ack[36]=0;
    ack[28]=5; CHECK(!crt_ack_valid(ack, 40, 7, crt_le32(header+20)));
    memcpy(ack, "ACK1", 4); ack[28]=0;
    CHECK(crt_ack_valid(ack, 32, 7, crt_le32(header+20)));
    puts("PASS CRT packing, CRC, headers, legacy/extended ACKs and fault rejection");
    return 0;
}
