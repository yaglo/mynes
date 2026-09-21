#ifndef GPU_OSD_H
#define GPU_OSD_H
#include "nes/osd.h"
#include "gpu_render.h"
/* Render only: navigation and callback ownership stay with main.c. */
void gpu_osd_render(uint8_t *rgb, uint16_t *codes, const uint8_t (*palette)[3],
                    const OSDMenuLevel *level, const char *preset, bool modified,
                    bool pal, const GPURenderCtx *render);
void gpu_osd_preset_notice(uint8_t *rgb, uint16_t *codes, const uint8_t (*palette)[3],
                           const char *name);
#endif
