/*
 * Chain Visualiser Overlay -- Public API
 * ========================================
 *
 * Draws an interactive overlay showing the video and audio signal chain
 * stages, their bypass state, and per-stage timing. Renders into a
 * 256x240 uint8 buffer (NES-resolution, palette-indexed) that the main
 * loop composites onto the PPU framebuffer.
 *
 * Phase A: compact two-column stage list (video left, audio right)
 *          with bypass toggles and keyboard navigation.
 *
 * Toggle with F8. When open, arrow keys navigate stages, B toggles
 * bypass on the selected stage.
 */

#ifndef CHAIN_VIS_H
#define CHAIN_VIS_H

#include "video_chain.h"
#include "audio_chain.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct ChainVis ChainVis;

/* Create a chain visualiser. vc and ac are borrowed (not owned). */
ChainVis *chain_vis_create(const VideoChain *vc, const AudioChain *ac);

/* Free all resources. Safe to call with NULL. */
void chain_vis_destroy(ChainVis *vis);

/* Toggle visibility (bound to F8). */
void chain_vis_toggle(ChainVis *vis);

/* Query visibility. */
bool chain_vis_is_open(const ChainVis *vis);

/* Rebuild the overlay for the current frame.
 * current_preset: index into physical_presets[] for the header line. */
void chain_vis_update(ChainVis *vis, int current_preset);

/* Return the 256x240 overlay buffer. Each byte is a NES palette index
 * (0x0F = black, 0x30 = white, etc.). *w and *h are set to the
 * buffer dimensions. Returns NULL when the visualiser is hidden. */
const uint8_t *chain_vis_get_overlay(const ChainVis *vis, int *w, int *h);

/* Feed a key event. Returns true if the visualiser consumed it
 * (caller should NOT forward to the emulator). */
bool chain_vis_handle_key(ChainVis *vis, int scancode, bool down);

#endif /* CHAIN_VIS_H */
