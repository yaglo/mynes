/*
 * Debug Tap API — GPU Buffer Readback for SwiftUI Visualiser
 * ===========================================================
 *
 * Provides a debug interface for the SwiftUI chain visualiser to read
 * intermediate GPU buffer contents after signal processing stages.
 *
 * Design goals:
 *   1. Zero cost when disabled (visualiser not connected)
 *   2. Selective tap points (only download what visualiser requests)
 *   3. Asynchronous capture (don't block the emulator's 60 Hz)
 *   4. IPC-agnostic (caller chooses shared memory, socket, Metal buffer)
 *   5. Per-stage metadata (stage name, size, timing, parameters)
 *
 * Typical workflow:
 *   1. Create manager at emulator startup
 *   2. SwiftUI visualiser connects (via custom protocol, not in this header)
 *   3. Visualiser enables tap points it wants to monitor (request: "tap stage 3")
 *   4. Emulator calls debug_tap_capture() after each chain_run()
 *   5. Visualiser queries captured buffers (async or on-demand)
 *   6. SwiftUI renders oscilloscope traces, waveform viewers, etc.
 *   7. Visualiser sends parameter changes back (out of scope for this header)
 *   8. Emulator adjusts stage params and re-runs chain
 *
 * Thread safety:
 *   This API is NOT thread-safe. The caller (emulator main loop) is
 *   responsible for serialization. Typical pattern:
 *     - Main thread: calls debug_tap_capture(), emulator_step()
 *     - IPC thread: queries debug_tap_get() (read-only, safe)
 *     - No concurrent modifications to enable/disable tap points
 *   For production, add mutexes if needed.
 */

#ifndef DEBUG_TAP_H
#define DEBUG_TAP_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include "signal_chain.h"
#include "signal_format.h"
#include "audio_format.h"

/* ============================================================================
 * Tap Point Data Structure
 * ============================================================================ */

/*
 * A single captured snapshot of a GPU buffer after a specific stage.
 * The visualiser uses this to render oscilloscope traces, waterfall plots,
 * or histogram analysis.
 *
 * Lifetime: Valid until the next debug_tap_capture() call for the same
 * stage, at which point the buffer is freed and reallocated. The visualiser
 * must either copy the data or consume it before the next capture.
 *
 * Memory ownership:
 *   - data pointer is owned by the DebugTapManager
 *   - Caller must NOT free it
 *   - Caller must NOT write to it
 *   - Valid only while the manager exists
 */
typedef struct {
    /* --- Identity --- */
    int stage_index;                    /* which stage in the chain (0-31) */
    const char *stage_name;             /* human-readable: "Luma FIR", etc. */
    const char *chain_name;             /* which chain: "video" or "audio" */

    /* --- Data layout --- */
    float *data;                        /* float32 buffer, CPU-side copy */
    int sample_count;                   /* total floats in the buffer */

    /* --- 2D interpretation (for video taps) --- */
    int samples_per_line;               /* stride for 2D indexing (NTSC: 2048, PAL: 2560) */
    int lines;                          /* 240 for video, 1 for audio */

    /* --- Captured frame metadata --- */
    uint64_t frame_number;              /* emulator frame counter at capture time */
    uint64_t capture_timestamp_ns;      /* monotonic SDL tick time in nanoseconds */

    /* --- Performance --- */
    double gpu_dispatch_us;             /* how long the stage's GPU dispatch took */
    double readback_us;                 /* how long the buffer download took */

    /* --- Stage parameters snapshot (for visualiser UI) --- */
    bool stage_enabled;                 /* whether this stage is active */
    bool stage_bypassed;                /* user-toggled bypass flag */
    uint8_t stage_params[128];          /* copy of the stage's uniform params */
    uint32_t stage_params_size;         /* actual size in bytes */

    /* --- Quality flags --- */
    bool is_valid;                      /* false if readback failed */
    const char *error_msg;              /* non-NULL if readback failed */
} DebugTap;

/* ============================================================================
 * Tap Manager Opaque Handle
 * ============================================================================ */

typedef struct DebugTapManager DebugTapManager;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/*
 * Create a debug tap manager for both video and audio chains.
 *
 * Args:
 *   video_chain: pointer to the video GPU chain (SignalChain), or NULL
 *   audio_chain: pointer to the audio GPU chain (SignalChain), or NULL
 *
 * Returns:
 *   Non-NULL on success (even if chains are NULL — useful for offline
 *   emulation without GPU). NULL on malloc failure.
 *
 * Postcondition:
 *   All tap points are disabled by default (zero overhead when visualiser
 *   is not connected).
 *
 * Lifetime:
 *   Caller must call debug_tap_destroy() when emulator exits.
 */
DebugTapManager *debug_tap_create(SDL_GPUDevice *gpu,
                                   SignalChain *video_chain,
                                   SignalChain *audio_chain);

/*
 * Destroy the manager and free all allocated buffers.
 *
 * Safe to call on NULL.
 */
void debug_tap_destroy(DebugTapManager *mgr);

/* ============================================================================
 * Enable/Disable Tap Points
 * ============================================================================ */

/*
 * Enable or disable a single tap point by stage index.
 *
 * When enabled, debug_tap_capture() will download this stage's output
 * buffer after the next GPU dispatch. When disabled, no download happens
 * (zero cost).
 *
 * Args:
 *   mgr: manager from debug_tap_create()
 *   stage_index: 0-based index into the chain's stages array
 *   enabled: true to capture, false to skip
 *
 * Returns: true if the stage exists and was updated, false if stage_index
 *          is out of range or chains are NULL.
 *
 * Note:
 *   This is NOT idempotent with respect to data lifecycle. If you disable
 *   a tap and re-enable it, the old buffer is freed. If you enable a tap,
 *   the buffer is allocated on first capture (lazy).
 */
bool debug_tap_set_enabled(DebugTapManager *mgr, int stage_index, bool enabled);

/*
 * Batch enable/disable: set the enabled flag for all tap points in one
 * or both chains.
 *
 * Args:
 *   mgr: manager
 *   video_enabled: set all video tap points to this flag
 *   audio_enabled: set all audio tap points to this flag
 *
 * Useful for:
 *   - "Enable all" button in visualiser UI
 *   - Disabling taps when visualiser disconnects
 */
void debug_tap_set_all_enabled(DebugTapManager *mgr, bool video_enabled,
                                bool audio_enabled);

/*
 * Query whether a tap point is enabled.
 *
 * Returns: true if enabled, false if disabled or stage_index is invalid.
 */
bool debug_tap_is_enabled(const DebugTapManager *mgr, int stage_index);

/*
 * Query how many enabled tap points exist across both chains.
 *
 * Useful for the visualiser to know: "are we capturing anything?"
 */
int debug_tap_enabled_count(const DebugTapManager *mgr);

/* ============================================================================
 * Capture and Readback
 * ============================================================================ */

/*
 * Capture enabled tap points by downloading GPU buffers to CPU.
 *
 * This is called ONCE PER FRAME by the emulator, after all signal chain
 * dispatches are complete. It's the point where the visualiser's waveform
 * data is gathered.
 *
 * Args:
 *   mgr: manager
 *   gpu: SDL3 GPU device (needed for buffer download commands)
 *   frame_number: emulator frame counter (for metadata)
 *
 * Behavior:
 *   - For each enabled tap point:
 *     - Allocate a CPU-side buffer (first time) or reuse existing
 *     - Issue GPU→CPU readback command
 *     - Wait for readback fence (blocks main thread briefly)
 *     - Copy GPU data to the DebugTap.data pointer
 *     - Update DebugTap.frame_number, timestamps, etc.
 *   - For each disabled tap point:
 *     - Skip (zero overhead)
 *
 * Performance:
 *   Each readback is a GPU→CPU transfer + synchronization. Typical latency:
 *     - Video tap (~1.9 MB for NTSC): ~3-5 ms on modern GPU
 *     - Audio tap (~120 KB for NTSC): <1 ms
 *   This is NOT on the critical path (visualiser updates at 10-15 Hz,
 *   emulator runs at 60 Hz), but calling every frame adds up. Consider:
 *     - Only enable taps when visualiser is open
 *     - Implement readback batching (capture every other frame)
 *     - Use asynchronous readback if GPU supports (future)
 *
 * Thread safety:
 *   Must be called from the same thread that created the manager.
 *   The GPU device must be safe for concurrent use (SDL3 is).
 */
void debug_tap_capture(DebugTapManager *mgr, SDL_GPUDevice *gpu,
                       uint64_t frame_number);

/*
 * Retrieve the most recent captured data for a tap point.
 *
 * Args:
 *   mgr: manager
 *   stage_index: which stage to query
 *
 * Returns:
 *   Pointer to the DebugTap struct, or NULL if:
 *     - stage_index is out of range
 *     - the stage has never been captured
 *     - the stage is disabled
 *
 * Lifetime:
 *   The pointer is valid until:
 *     - debug_tap_capture() is called again for this stage
 *     - debug_tap_set_enabled(mgr, stage_index, false) is called
 *     - debug_tap_destroy(mgr) is called
 *
 *   The data buffer (DebugTap.data) is valid for the same duration.
 *
 * Thread safety:
 *   Read-only safe from other threads (no modifications happen during
 *   queries). The visualiser can call this from a separate IPC thread
 *   while the emulator is running.
 */
const DebugTap *debug_tap_get(const DebugTapManager *mgr, int stage_index);

/* ============================================================================
 * Metadata Queries
 * ============================================================================ */

/*
 * Query the total number of tap points (all stages in both chains).
 *
 * Returns: number of stages across all chains, or 0 if no chains are set.
 */
int debug_tap_total_count(const DebugTapManager *mgr);

/*
 * Query the number of video chain stages.
 */
int debug_tap_video_stage_count(const DebugTapManager *mgr);

/*
 * Query the number of audio chain stages.
 */
int debug_tap_audio_stage_count(const DebugTapManager *mgr);

/*
 * Get the stage name by index (useful for UI dropdown menus).
 *
 * Returns: human-readable name, or "" if index is invalid.
 */
const char *debug_tap_get_stage_name(const DebugTapManager *mgr,
                                      int stage_index);

/*
 * Get which chain a stage belongs to.
 *
 * Returns: "video", "audio", or "unknown".
 */
const char *debug_tap_get_chain_name(const DebugTapManager *mgr,
                                      int stage_index);

/* ============================================================================
 * Statistics (for visualiser performance monitoring)
 * ============================================================================ */

/*
 * Query cumulative statistics about readback performance.
 *
 * Useful for the visualiser to detect if it's starving the emulator.
 */
typedef struct {
    int total_captures;                 /* number of debug_tap_capture() calls */
    double total_readback_us;           /* cumulative readback time (microseconds) */
    double peak_readback_us;            /* longest single capture */
    double avg_readback_us;             /* moving average */
    int current_enabled_count;          /* taps enabled right now */
} DebugTapStats;

/*
 * Get performance statistics.
 *
 * The visualiser can display these in a "debug stats" overlay to verify
 * it's not impacting emulator performance.
 */
DebugTapStats debug_tap_get_stats(const DebugTapManager *mgr);

/*
 * Reset statistics (useful at visualiser disconnect).
 */
void debug_tap_reset_stats(DebugTapManager *mgr);

/* ============================================================================
 * Integration Points in Emulator Main Loop
 * ============================================================================
 *
 * This section shows HOW and WHERE to call the debug tap API in the
 * emulator's signal processing. It's pseudo-code, not a real function.
 *
 * === In main.c startup (after chains are created): ===
 *
 *   DebugTapManager *tap_mgr = debug_tap_create(
 *       &video_gpu_chain.sig_chain,
 *       &audio_gpu.chain  // or NULL if audio GPU not available
 *   );
 *   if (!tap_mgr) {
 *       fprintf(stderr, "debug_tap_create failed\n");
 *       return false;
 *   }
 *
 * === In main loop, after each frame's signal processing: ===
 *
 *   // VIDEO PROCESSING
 *   generate_waveform(...);
 *   video_gpu_process(...);
 *
 *   // CAPTURE VIDEO TAPS (must be called AFTER chain_run,
 *   // while GPU buffers still contain the current frame's data)
 *   debug_tap_capture(tap_mgr, gpu, frame_count);
 *
 *   // AUDIO PROCESSING
 *   if (audio_gpu_enabled) {
 *       audio_gpu_process(...);
 *       // Audio taps are captured in the same debug_tap_capture() call
 *       // because SignalChain is chain-agnostic
 *   }
 *
 * === When visualiser connects (via IPC, not in this API): ===
 *
 *   // SwiftUI sends: "enable tap stage 4"
 *   debug_tap_set_enabled(tap_mgr, 4, true);
 *
 * === When visualiser queries waveform data (from IPC thread): ===
 *
 *   const DebugTap *tap = debug_tap_get(tap_mgr, stage_idx);
 *   if (tap) {
 *       // Copy tap->data[0..sample_count-1] to IPC buffer
 *       // OR render directly (oscilloscope, waterfall, histogram, etc.)
 *   }
 *
 * === When visualiser sends parameter change (from IPC thread): ===
 *
 *   // SwiftUI sends: "set stage 4 param[0] = 0.8"
 *   // Emulator main thread receives this and calls:
 *   chain_update_params(&video_gpu_chain.sig_chain, 4, &new_params, sizeof(...));
 *
 * === On emulator exit: ===
 *
 *   debug_tap_destroy(tap_mgr);
 *
 * ============================================================================ */

/* ============================================================================
 * IPC Design Recommendations (NOT IN THIS HEADER)
 * ============================================================================
 *
 * The debug_tap API handles GPU↔CPU data transfer. The IPC (visualiser
 * ↔ emulator) is out of scope, but here are notes for implementation:
 *
 * === Option A: Shared Memory (mmap) ===
 * Pros:
 *   - Zero-copy: visualiser reads directly from a fixed mmap region
 *   - Lowest latency (<1 ms roundtrip)
 *   - Simplest (no serialization, no socket overhead)
 * Cons:
 *   - Platform-specific (POSIX shm_open / mmap)
 *   - Fixed buffer size (pre-allocate for worst case: PAL video + audio)
 *   - Less suitable for remote connections (not applicable here)
 *
 * Layout:
 *   struct SharedMemHeader {
 *       uint32_t magic;              // 0xDEADBEEF
 *       uint32_t version;            // 1
 *       uint64_t frame_number;       // current emulator frame
 *       uint32_t enabled_mask;       // bitmask: which taps are live
 *       DebugTap tap_points[32];     // array of tap metadata
 *       float    tap_data[...];      // concatenated tap buffers
 *   };
 *
 * === Option B: Unix Domain Socket ===
 * Pros:
 *   - Simple connection/disconnection (bidirectional IPC)
 *   - Symmetric: visualiser can send parameter changes back
 *   - Supports multiple visualisers (if desired)
 * Cons:
 *   - Serialization overhead (copy data into socket buffer)
 *   - Small latency tax (~2-5 ms)
 *   - Need a simple protocol layer
 *
 * Protocol skeleton:
 *   Client: "ENABLE_TAP 4"
 *   Server: "ACK"
 *   Server: "FRAME 12345 TAP 4 2048 240 ..." + binary waveform
 *   Client: "SET_PARAM 4 0 0.8"
 *   Server: "ACK"
 *
 * === Option C: Metal Shared Buffer (Apple-only) ===
 * Pros:
 *   - Direct GPU↔SwiftUI zero-copy (Metal texture binding)
 *   - Highest performance (no CPU readback needed)
 * Cons:
 *   - Metal-only (not portable to Linux/Windows)
 *   - Complex (MTLHeap, MTLResource synchronization)
 *
 * Design:
 *   - Allocate MTLBuffer in shared storage mode (storageModeShared)
 *   - GPU writes to the Metal buffer during dispatch
 *   - SwiftUI binds the buffer directly to a Metal texture
 *   - No explicit readback needed
 *
 * ============================================================================ */

#endif /* DEBUG_TAP_H */
