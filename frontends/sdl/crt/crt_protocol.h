#ifndef CRT_PROTOCOL_H
#define CRT_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define CRT_PIXELS (256u * 240u)
#define CRT_PAYLOAD (CRT_PIXELS * 9u / 8u)
#define CRT_PACKET (32u + CRT_PAYLOAD)
#define CRT_ACK 40u
uint32_t crt_crc32(const uint8_t *p, size_t n);
/* Returns -1 for tokens outside 0..511; never silently drops emphasis. */
int crt_pack(uint8_t out[CRT_PAYLOAD], const uint16_t pixels[CRT_PIXELS]);
void crt_header(uint8_t out[32], uint32_t sequence, const uint8_t payload[CRT_PAYLOAD]);
/* Validate legacy 32-byte or extended 40-byte FPGA acknowledgements. */
int crt_ack_valid(const uint8_t *ack, size_t length, uint32_t sequence, uint32_t crc);
uint32_t crt_le32(const uint8_t *p);
void crt_put32(uint8_t *p, uint32_t x);
#endif
