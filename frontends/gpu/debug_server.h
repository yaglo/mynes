/*
 * Debug Server — Emulator-side IPC for the SwiftUI Visualiser
 * ============================================================
 *
 * A Unix domain socket server that allows the SwiftUI chain visualiser to:
 * 1. Monitor signal chain execution (per-stage timing, enabled/bypassed state)
 * 2. Request waveform taps at specific stages
 * 3. Update stage parameters (RC filter R/C, FIR cutoffs, etc)
 * 4. Change presets
 *
 * The server listens on a background thread but all message processing happens
 * on the emulator's main thread (via debug_server_frame) to avoid threading issues.
 * The visualiser can connect/disconnect at any time.
 *
 * Protocol: Binary messages with frame: [uint32 type][uint32 size][payload]
 * Types: 0=snapshot, 1=param_update, 2=preset_change, 3=tap_request, 4=tap_data
 */

#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

#include "signal_chain.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct DebugServer DebugServer;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/** Create a debug server listening on the specified socket path.
 *
 * Starts a background thread that listens and accepts one client connection
 * (the visualiser). If no visualiser connects, the server waits silently.
 * If the visualiser disconnects, the server waits for reconnection.
 *
 * @param socket_path  Path to Unix domain socket (e.g., "/tmp/nes_gpu_debug.sock")
 * @return             Allocated DebugServer, or NULL on failure
 */
DebugServer *debug_server_create(const char *socket_path);

/** Shut down the debug server gracefully.
 *
 * Closes all sockets, stops the background thread, and frees resources.
 * Safe to call repeatedly (no-op if already destroyed).
 *
 * @param srv  Debug server from debug_server_create (NULL is OK)
 */
void debug_server_destroy(DebugServer *srv);

/* ============================================================================
 * Main loop integration
 * ============================================================================ */

/** Process one frame: read incoming messages and send snapshot.
 *
 * Called once per emulator frame from the main loop. This function:
 * 1. Reads any pending messages from the visualiser (param updates, tap requests)
 * 2. Applies parameter updates to the signal chains
 * 3. Builds and sends a snapshot (per-stage timing + optional waveform tap)
 *
 * @param srv             Debug server from debug_server_create
 * @param video_chain     Video SignalChain (borrowed, may be NULL)
 * @param audio_chain     Audio SignalChain (borrowed, may be NULL)
 * @param frame_number    Current frame count (for snapshot metadata)
 */
void debug_server_frame(DebugServer *srv,
                        const SignalChain *video_chain,
                        const SignalChain *audio_chain,
                        uint32_t frame_number);

/* ============================================================================
 * Waveform taps
 * ============================================================================ */

/** Send captured waveform data to the visualiser.
 *
 * Call this after capturing a waveform tap (typically after chain_download_output
 * or similar). The data is sent as a separate message so the visualiser can
 * display it alongside the snapshot.
 *
 * @param srv             Debug server
 * @param stage_index     Which stage was tapped (0-based)
 * @param data            Waveform samples (float32, may be large)
 * @param sample_count    Number of samples
 * @param samples_per_line  Video: 2048 (NTSC) or 2560 (PAL), Audio: ignored
 * @param lines           Video: 240, Audio: 1
 */
void debug_server_send_tap(DebugServer *srv,
                           int stage_index,
                           const float *data,
                           int sample_count,
                           int samples_per_line,
                           int lines);

/* ============================================================================
 * Query
 * ============================================================================ */

/** Check if a visualiser client is currently connected.
 *
 * @param srv  Debug server
 * @return     true if connected and ready to receive messages
 */
bool debug_server_has_client(const DebugServer *srv);

#endif /* DEBUG_SERVER_H */
