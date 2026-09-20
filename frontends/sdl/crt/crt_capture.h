#ifndef MYNES_CRT_CAPTURE_H
#define MYNES_CRT_CAPTURE_H
#include <stdio.h>
#include "crt_protocol.h"
/* Call immediately after nes_run_frame(), before overlays. File contains packed
   69,120-byte frames, without headers. Replay using crt-usb send capture.bin.
   Live streaming uses crt_live; CRT clock-feedback pacing is not implemented. */
static inline int crt_capture_frame(FILE *file, const uint16_t *index_framebuffer) {
    uint8_t packed[CRT_PAYLOAD];
    if(crt_pack(packed,index_framebuffer)) return -1;
    return fwrite(packed,1,sizeof(packed),file)==sizeof(packed)?0:-1;
}
#endif
