#ifndef CRT_LIVE_H
#define CRT_LIVE_H
#include <stddef.h>
#include <stdint.h>
#include "crt_protocol.h"
typedef struct crt_live crt_live;
/* Owns its USB connection. Exactly one in-flight and one replaceable pending frame. */
crt_live *crt_live_open(char *error, size_t error_size);
int crt_live_submit(crt_live *stream, const uint16_t pixels[CRT_PIXELS]);
void crt_live_stats(crt_live *stream, uint64_t *sent, uint64_t *replaced);
void crt_live_close(crt_live *stream);
#endif
