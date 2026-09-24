#ifndef CRT_PROTOCOL_H
#define CRT_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
#define CRT_PIXELS (256u * 240u)
#define CRT_PAYLOAD (CRT_PIXELS * 9u / 8u)
#define CRT_PACKET (32u + CRT_PAYLOAD)
#define CRT_ACK 40u
/* Audio packets: kind 2, signed 16-bit little-endian stereo frames. 400 frames
 * make a 1,632-byte packet, never a multiple of the 4,096-byte USB chunk, so
 * the bridge always sees a short final chunk. The FPGA accepts 1..1024 frames. */
#define CRT_AUDIO_FRAMES 400u
#define CRT_AUDIO_MAX_FRAMES 1024u
#define CRT_AUDIO_PACKET (32u + 4u * CRT_AUDIO_FRAMES)
/* The box clocks I2S from its video sample clock: 42,954,545/14/64 and 53,203,425/16/64. */
#define CRT_AUDIO_RATE_NTSC 47941u
#define CRT_AUDIO_RATE_PAL 51957u
uint32_t crt_crc32(const uint8_t *p, size_t n);
/* Returns -1 for tokens outside 0..511; never silently drops emphasis. */
int crt_pack(uint8_t out[CRT_PAYLOAD], const uint16_t pixels[CRT_PIXELS]);
void crt_header(uint8_t out[32], uint32_t sequence, const uint8_t payload[CRT_PAYLOAD]);
void crt_audio_header(uint8_t out[32], uint32_t sequence, uint32_t rate_hz, const uint8_t *payload, uint32_t frames);
uint32_t crt_le32(const uint8_t *p);
void crt_put32(uint8_t *p, uint32_t x);
/* ACK2 bytes 28-31, produced by the video image. */
typedef struct {
    unsigned sdram_passed, sdram_failed, missed_line;
    unsigned audio_playing, audio_level, audio_underruns, audio_rate_id, audio_muted;
} crt_status;
crt_status crt_status_decode(uint32_t word);
uint32_t crt_audio_rate_hz(unsigned rate_id);
/* Checks a 32-byte (ACK1) or 40-byte acknowledgement for the packet just sent.
 * accepted is the number of packets the box must have accepted so far,
 * including this one; payload_length is this packet's payload size. Returns 1
 * when the packet was accepted with no rejections, memory faults or bridge
 * errors; fills status (may be NULL) from ACK2 words. */
int crt_ack_check(const uint8_t *ack, size_t length, uint32_t sequence, uint32_t payload_crc,
                  uint32_t accepted, uint32_t payload_length, crt_status *status);
/* Video-only sessions: the packet with this sequence is the (sequence+1)th accepted. */
int crt_ack_valid(const uint8_t *ack, size_t length, uint32_t sequence, uint32_t payload_crc);
#endif
