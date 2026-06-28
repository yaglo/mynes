# SwiftUI Chain Visualiser Design

## Overview

A standalone macOS SwiftUI application that provides real-time visualization and parameter editing for the NES GPU signal chain. The visualiser communicates with the running emulator via Unix domain sockets to:

1. **Monitor** the signal chain execution: stage timing, buffer contents, and activation state
2. **Inspect** waveforms at any tap point between stages (oscilloscope view)
3. **Edit** parameters and presets with immediate feedback
4. **Compare** before/after waveforms (A/B split-screen)
5. **Debug** shader compilation and buffer routing

**Key Design Decision**: The visualiser runs as a separate SwiftUI process, not embedded in the GPU frontend, to avoid adding UI latency to the real-time emulation path. The emulator exposes a lightweight debug API over Unix sockets.

---

## Architecture

### 1. Communication Layer: Unix Domain Sockets

The emulator and visualiser communicate via a Unix domain socket at `$TMPDIR/nes_gpu_debug.sock`.

**Why Unix sockets?**
- Zero-copy buffer sharing via `SO_PEERCRED` verification
- Symmetric IPC (works on any POSIX system)
- No separate server process needed
- Built-in access control via file permissions

**Protocol Structure** (binary, little-endian):

```c
typedef struct {
    uint32_t msg_type;      /* 0=snapshot, 1=param_update, 2=preset_change, etc */
    uint32_t payload_size;
    uint8_t  payload[...];
} DebugMessage;

/* Snapshot message (emulator → visualiser) */
typedef struct {
    uint32_t frame_number;
    uint32_t timestamp_ms;
    
    /* Stage information */
    uint16_t num_video_stages;
    uint16_t num_audio_stages;
    
    /* Per-stage metadata (one entry per stage) */
    struct {
        uint8_t  enabled;           /* bool */
        uint8_t  bypassed;          /* bool */
        uint32_t timing_us;         /* wall-clock dispatch time */
        uint32_t timing_avg_us;     /* exponential moving average */
    } stages[CHAIN_MAX_STAGES];
    
    /* Optional: waveform tap at a specific stage */
    uint32_t tap_stage_index;       /* ~0 = none requested */
    uint32_t tap_sample_count;      /* samples in tap buffer */
    float    tap_samples[...];      /* variable-length array */
} SnapshotMessage;

/* Parameter update message (visualiser → emulator) */
typedef struct {
    uint16_t stage_index;
    uint32_t params_size;
    uint8_t  params[CHAIN_MAX_UNIFORM_SIZE];
} ParamUpdateMessage;

/* Preset change message (visualiser → emulator) */
typedef struct {
    uint8_t preset_index;
} PresetChangeMessage;
```

### 2. Emulator-Side Debug API

Add to `frontends/gpu/debug_api.h`:

```c
typedef struct DebugServer DebugServer;

/* Initialize the debug server (called once at startup).
 * Returns NULL on failure. Runs on a background thread. */
DebugServer *debug_server_create(SignalChain *video_chain,
                                 SignalChain *audio_chain,
                                 const char *socket_path);

/* Queue a waveform tap snapshot for the next poll.
 * stage_index: which stage to tap (0=DAC, 1=console_output, etc)
 * connection_type: 0=video, 1=audio */
void debug_server_request_tap(DebugServer *srv, int stage_index, int connection_type);

/* Shut down the server gracefully. Safe to call repeatedly. */
void debug_server_destroy(DebugServer *srv);

/* Accept and process one incoming message (non-blocking).
 * Called periodically from the main emulator loop. */
void debug_server_poll(DebugServer *srv);
```

**Implementation Notes:**
- Background thread listens on the socket
- Main emulator thread calls `debug_server_poll()` once per frame
- Tap requests are queued and serviced after that frame's chain runs
- Snapshot buffer is a simple ring (holds the last complete snapshot)

### 3. Visualiser-Side Connection

**Swift Protocol** (in `EmulatorConnection.swift`):

```swift
protocol EmulatorConnectionDelegate: AnyObject {
    func onSnapshotReceived(_ snapshot: ChainSnapshot)
    func onConnectionEstablished()
    func onConnectionLost(_ reason: String)
}

class EmulatorConnection {
    weak var delegate: EmulatorConnectionDelegate?
    
    init(socketPath: String = "/tmp/nes_gpu_debug.sock")
    
    /// Connect to the emulator (may block briefly).
    func connect() async -> Result<Void, ConnectionError>
    
    /// Disconnect gracefully.
    func disconnect()
    
    /// Request a waveform tap at the given stage.
    func requestTap(stageIndex: Int, chainType: ChainType) async
    
    /// Send a parameter update to a stage.
    func updateStageParams(_ params: StageParams, stageIndex: Int) async -> Result<Void, ConnectionError>
    
    /// Send a preset change.
    func setPreset(_ presetIndex: Int) async -> Result<Void, ConnectionError>
    
    /// Start listening for snapshots (internally runs on background thread).
    func startListening()
}
```

---

## UI Components

### 1. Signal Chain Node Graph (`NodeGraphView.swift`)

Visual representation of the 13 video + 10 audio stages as a flow graph.

**Features:**
- Vertical layout: video stages on left column, audio on right
- Each stage is a clickable node showing:
  - Stage name (e.g., "RC Coupling", "FIR Luma Lowpass")
  - Kernel type icon (circle=pointwise, square=RC, diamond=FIR, etc)
  - Enabled/bypassed state (color: green=active, gray=disabled, yellow=bypassed)
  - Current timing in µs (e.g., "14.2 µs")
- Connecting lines show buffer flow (dotted=aux buffer, solid=main)
- Selection: click a stage to show details in the Parameter Editor
- Double-click to toggle bypass

**Data Model:**

```swift
struct StageNode: Identifiable {
    let id: Int                 // stage index
    let name: String
    let kernelType: KernelType
    let enabled: Bool
    let bypassed: Bool
    let timingUs: UInt32
    let timingAvgUs: UInt32
    
    var isActive: Bool { enabled && !bypassed }
    var statusColor: Color {
        switch (enabled, bypassed) {
        case (false, _): return .gray
        case (true, true): return .yellow
        case (true, false): return .green
        }
    }
}

enum KernelType: String {
    case pointwise = "pw"
    case rcFilter = "rc"
    case fir = "fir"
    case delay = "dly"
    case modulator = "mod"
    case dac = "dac"
    case matrix = "mtx"
    case beam = "bm"
}

struct ChainSnapshot {
    let frameNumber: UInt32
    let videoStages: [StageNode]
    let audioStages: [StageNode]
    let totalTimingUs: Double
}
```

### 2. Oscilloscope View (`OscilloscopeView.swift`)

Real-time waveform display at a selected tap point.

**Features:**
- X-axis: sample index (0 to ~2048 for video, ~30K for audio)
- Y-axis: amplitude (-1.5 to 1.5 normalized)
- Multiple trace modes:
  - **Single**: shows the current waveform at the selected tap
  - **Before/After**: comparison of the stage input and output (dual overlay)
  - **A/B Split**: left=before, right=after at the same X zoom level
- Time domain by default; toggle for FFT magnitude spectrum (Y-axis becomes dB)
- Trigger modes: frame start, scanline start, zero-crossing, or free-running
- Zoom/pan controls (scroll wheel = zoom, drag = pan)
- Mark colors: red for significant frequencies (> -20dB in FFT mode)

**Data Model:**

```swift
struct WaveformData: Identifiable {
    let id: UUID
    let stageName: String
    let stageIndex: Int
    let samples: [Float]           // raw float32 samples
    let sampleRate: Float          // Hz
    let timestamp: Date
    
    /// Convert to frequency domain via vDSP FFT
    func toFrequencyDomain() -> [Float] { ... }
}

enum OscilloscopeMode {
    case single(tapStageIndex: Int)
    case beforeAfter(stageIndex: Int)
    case abSplit(referenceSnapshot: ChainSnapshot, stageIndex: Int)
}

enum TriggerMode {
    case free
    case frameStart
    case scanlineStart
    case zeroCrossing
}

struct OscilloscopeState {
    var mode: OscilloscopeMode
    var triggerMode: TriggerMode
    var displayDomain: DisplayDomain     // .time or .frequency
    var xZoom: Float = 1.0              // samples per pixel
    var xOffset: Int = 0                // first visible sample
    var yScale: Float = 1.0             // units per pixel
}
```

**Metal Rendering** (for performance):

The oscilloscope uses a custom Metal shader to render waveforms efficiently:

```swift
class WaveformRenderer {
    func render(waveform: WaveformData,
                displayDomain: DisplayDomain,
                xZoom: Float,
                xOffset: Int,
                yScale: Float,
                to texture: MTLTexture)
}
```

The shader handles:
- Line rasterization (bresenham-style for crisp 1px traces)
- Frequency-domain conversion (computes FFT on GPU if needed)
- Color mapping (white trace, red peaks)

### 3. Parameter Editor (`ParameterEditorView.swift`)

Per-stage parameter editing with live physical units.

**Features:**
- Dynamically loads parameter schema based on kernel type
- RC filters: R (Ω), C (F), and computed cutoff frequency (Hz)
- FIR filters: tap count, cutoff (Hz), window type (Hamming/Kaiser/Blackman)
- Pointwise stages: gain (linear/dB), clip level, gamma, hue offset
- Modulator: carrier frequency, deviation, phase lock mode
- All sliders show live-computed dependent values:
  - RC: cutoff = 1 / (2π·R·C)
  - FIR: bandwidth = cutoff × sample_rate
- **Undo/Redo** support via Combine
- **Apply/Reset** buttons (apply commits to emulator, reset discards)

**Data Model:**

```swift
protocol StageParameter: Identifiable {
    var name: String { get }
    var unit: String { get }
    var value: Float { get set }
    var min: Float { get }
    var max: Float { get }
    var dependents: [String: Float] { get }
}

struct RCFilterParams: StageParameter {
    var resistanceOhms: Float           // Ω, 10-100kΩ range
    var capacitanceFarads: Float        // F, 1nF-100µF range
    
    var id: String { "rc_\(stageIndex)" }
    var name: String { "RC Filter" }
    
    var dependents: [String: Float] {
        let fc = 1.0 / (2.0 * .pi * resistanceOhms * capacitanceFarads)
        return ["cutoff_hz": fc]
    }
    
    /// Export to C struct for emulator
    func asCStruct() -> AudioRCStage { ... }
}

struct FIRParams: StageParameter {
    var tapCount: Int                   // 37-127
    var cutoffHz: Float                 // normalized: 0.01-0.5
    var windowType: WindowType
    
    var dependents: [String: Float] {
        ["bandwidth_hz": cutoffHz * sampleRate]
    }
}
```

### 4. Preset Selector & A/B Split (`PresetView.swift` + `SplitScreenView.swift`)

**Preset Selector:**
- Dropdown showing all presets from `physical_presets[]`
- Shows preset name + brief description
- Selecting a preset immediately applies it (sends `PresetChangeMessage`)
- Star icon to favorite (stored in `~/Library/Preferences/nes_visualiser_favorites.plist`)

**A/B Split Screen:**
- Horizontal or vertical split
- Left side: current emulator output
- Right side: reference waveform (previous frame, different preset, or bypassed stage)
- Synchronized scrolling/zooming between panels
- Toggle button to swap sides
- Timeline slider to compare across multiple frames

```swift
struct PresetView: View {
    @StateObject var connection: EmulatorConnection
    @State var selectedPreset: Int = 0
    @State var favorites: Set<Int> = []
    
    var body: some View {
        VStack {
            Picker("Preset", selection: $selectedPreset) {
                ForEach(0..<PhysicalPresets.count, id: \.self) { idx in
                    Text(PhysicalPresets[idx].name)
                        .tag(idx)
                }
            }
            .onChange(of: selectedPreset) { _, new in
                Task {
                    await connection.setPreset(new)
                }
            }
            
            HStack {
                Text(PhysicalPresets[selectedPreset].description)
                    .font(.caption)
                    .foregroundColor(.secondary)
                
                Spacer()
                
                Button(action: { toggleFavorite(selectedPreset) }) {
                    Image(systemName: favorites.contains(selectedPreset) ? "star.fill" : "star")
                }
            }
        }
    }
}

struct SplitScreenView: View {
    @State var leftWaveform: WaveformData?
    @State var rightWaveform: WaveformData?
    @State var leftSnapshot: ChainSnapshot?
    @State var rightSnapshot: ChainSnapshot?
    @State var syncZoom: Bool = true
    @State var xZoom: Float = 1.0
    @State var xOffset: Int = 0
    
    var body: some View {
        HStack(spacing: 0) {
            if let wf = leftWaveform {
                OscilloscopeView(waveform: wf, xZoom: $xZoom, xOffset: $xOffset)
            }
            
            Divider()
            
            if let wf = rightWaveform {
                OscilloscopeView(waveform: wf, xZoom: $xZoom, xOffset: $xOffset)
            }
        }
    }
}
```

### 5. Performance Dashboard (`PerformanceDashboardView.swift`)

Real-time metrics visualization.

**Metrics:**
- Per-stage timing as horizontal bar chart (sorted by duration)
- Total frame time as gauge (green < 16.67ms, yellow 16.67-33ms, red > 33ms)
- Buffer memory usage (input + output + aux + carry, in MB)
- Dispatch count (total and per-kernel type)
- Frame rate graph (60fps = horizontal line, dips visible)
- GPU utilization % (estimated from per-stage timing)

**Data Model:**

```swift
struct PerformanceMetrics {
    let frameNumber: UInt32
    let totalTimingUs: Double
    let perStageTiming: [(stageName: String, timingUs: Double)]
    let bufferSizeMB: Float
    let dispatchCount: Int
    let dispatchCountByType: [String: Int]
    let fps: Float
    
    var frameTimeMs: Double { totalTimingUs / 1000.0 }
    var gpuUtilizationPercent: Float {
        Float(totalTimingUs) / (1000000.0 / 60.0) * 100.0  // assuming 60fps
    }
}

class PerformanceHistory {
    private var history: [PerformanceMetrics] = []
    private let maxEntries = 600  // 10 seconds at 60fps
    
    func append(_ metrics: PerformanceMetrics) {
        history.append(metrics)
        if history.count > maxEntries {
            history.removeFirst()
        }
    }
    
    var frameTimeGraph: [Double] { history.map { $0.frameTimeMs } }
    var fpsGraph: [Float] { history.map { $0.fps } }
}
```

### 6. Shader Compilation Panel (`ShaderDebugView.swift`)

Displays shader compilation status and resource usage.

**Features:**
- List of all kernel types (pointwise, rc_filter, fir, etc.)
- Each row shows:
  - Shader name
  - Compilation status (✓ OK, ⚠ warning, ✗ error)
  - Resource counts: read-only buffers, read-write buffers, uniform buffers
  - File size + modification time
- "Recompile All" button calls `compile_shaders.sh` via subprocess
- Compiler error output displayed in a scrollable text view
- Buffer index mapping verification (ensures stage reads match emulator writes)

**Data Model:**

```swift
struct ShaderInfo: Identifiable {
    let id: String
    let kernelType: KernelType
    let filePath: URL
    let compilationStatus: CompilationStatus
    let resourceCount: (readonly: Int, readwrite: Int, uniform: Int)
    let fileSize: Int
    let modificationDate: Date
    
    var isCompiled: Bool {
        compilationStatus == .ok
    }
}

enum CompilationStatus {
    case ok
    case warning(String)
    case error(String)
}

class ShaderRegistry: ObservableObject {
    @Published var shaders: [ShaderInfo] = []
    
    func recompileAll() async -> Result<Void, ShaderError> {
        // Execute compile_shaders.sh
    }
}
```

---

## Data Flow

### Snapshot Cycle (10–15 fps)

```
Emulator (main loop, 60fps):
  ┌─ Frame N+1 begins
  │
  ├─ If visualiser is listening:
  │  ├─ Run video chain (produces waveform buffer)
  │  ├─ Run audio chain (produces audio buffer)
  │  └─ Build snapshot:
  │     ├─ For each stage: read enabled, bypassed, timing
  │     ├─ If tap requested: read stage output buffer
  │     └─ Queue to debug server
  │
  └─ debug_server_poll() transmits snapshot to visualiser


Visualiser (background connection thread):
  ┌─ Socket receives snapshot message
  ├─ Decode into ChainSnapshot
  ├─ Extract WaveformData if present
  └─ Post to main thread via @MainActor

Visualiser (main thread):
  ┌─ @Published snapshot updated
  ├─ NodeGraphView redraws stages
  ├─ OscilloscopeView updates trace
  ├─ PerformanceDashboard redraws charts
  └─ UI refreshes (SwiftUI automatic)
```

### Parameter Update Flow

```
SwiftUI UI:
  ┌─ User moves slider for "R" in RC filter stage
  ├─ ParameterEditorView recomputes cutoff dependent
  ├─ User clicks "Apply"
  └─ Calls EmulatorConnection.updateStageParams()

EmulatorConnection:
  ┌─ Encode RCFilterParams → C struct
  ├─ Send ParamUpdateMessage on socket
  └─ Wait for ACK (optional)

Emulator (debug_server_poll()):
  ┌─ Receive ParamUpdateMessage
  ├─ Call chain_update_params(video_chain, stage_idx, new_params)
  └─ On next dispatch, new parameters are used

Result:
  ┌─ Next frame's waveform reflects the change
  └─ Visualiser receives new snapshot automatically
```

---

## Technology Stack

### SwiftUI + Combine
- Reactive data binding for parameters
- `@Published` for snapshot updates
- `async/await` for socket communication

### Metal
- Custom Metal shader for oscilloscope rendering (line rasterization)
- Parallel FFT via Accelerate framework (vDSP)
- On-GPU frequency domain conversion (future optimization)

### Accelerate Framework
- `vDSP` for FFT (for frequency-domain oscilloscope)
- `vDSP.multiply()` for scaling waveforms

### Core Foundation
- `CFSocket` for Unix domain socket handling
- `CFRunLoop` for event-driven I/O

---

## File Structure

```
frontends/visualiser/
├── Package.swift
├── README.md
│
├── Sources/
│   └── ChainVisualiser/
│       ├── App.swift                    # Entry point, window setup
│       │
│       ├── Views/
│       │   ├── ContentView.swift        # Main tab view (tabs for each section)
│       │   ├── NodeGraphView.swift      # Stage flow diagram
│       │   ├── OscilloscopeView.swift   # Waveform display + Metal rendering
│       │   ├── ParameterEditorView.swift
│       │   ├── PresetView.swift
│       │   ├── SplitScreenView.swift
│       │   ├── PerformanceDashboardView.swift
│       │   ├── ShaderDebugView.swift
│       │   └── MetalRenderingView.swift # Wrapper for Metal texture rendering
│       │
│       ├── Models/
│       │   ├── ChainSnapshot.swift      # Decodable struct matching C binary format
│       │   ├── Stage.swift              # StageNode, KernelType enums
│       │   ├── WaveformData.swift       # Sample buffers + frequency domain
│       │   ├── Preset.swift             # Mirror of PhysicalPreset
│       │   ├── PerformanceMetrics.swift
│       │   └── ShaderInfo.swift
│       │
│       ├── Services/
│       │   ├── EmulatorConnection.swift # Unix socket client
│       │   ├── BufferReader.swift       # Parse C binary messages
│       │   └── ShaderCompiler.swift     # Execute compile_shaders.sh
│       │
│       └── Shaders/
│           └── Waveform.metal           # Line rasterization shader
│
└── Resources/
    ├── Assets.xcassets
    └── Localizable.strings
```

---

## API Between Emulator and Visualiser

### C Header: `frontends/gpu/debug_api.h`

```c
/*
 * Debug API — Emulator-side interface for the SwiftUI visualiser.
 * Runs on a background thread, communicates via Unix domain socket.
 * The visualiser connects to $TMPDIR/nes_gpu_debug.sock or /tmp/nes_gpu_debug.sock.
 */

#ifndef DEBUG_API_H
#define DEBUG_API_H

#include "signal_chain.h"
#include <stdbool.h>

typedef struct DebugServer DebugServer;

/** Initialize the debug server.
 * Listens on a Unix domain socket for incoming visualiser connections.
 * Runs on a background thread.
 *
 * @param video_chain  Pointer to the video SignalChain (borrowed, not owned)
 * @param audio_chain  Pointer to the audio SignalChain (borrowed, not owned)
 * @param socket_path  Path to Unix socket (e.g., "/tmp/nes_gpu_debug.sock")
 * @return Allocated DebugServer, or NULL on failure
 */
DebugServer *debug_server_create(SignalChain *video_chain,
                                 SignalChain *audio_chain,
                                 const char *socket_path);

/** Request a waveform tap at the given stage.
 * The emulator will capture the stage's output buffer on the next frame
 * and include it in the next snapshot. Only one tap can be pending at a time;
 * calling this again overwrites the previous request.
 *
 * @param srv            Debug server (from debug_server_create)
 * @param stage_index    Stage index in the chain (0-based)
 * @param is_audio       true for audio chain, false for video
 */
void debug_server_request_tap(DebugServer *srv, int stage_index, bool is_audio);

/** Process one incoming message from the visualiser.
 * Non-blocking. Called once per emulator frame from the main loop.
 * Handles parameter updates, preset changes, and tap requests.
 *
 * @param srv  Debug server (from debug_server_create)
 */
void debug_server_poll(DebugServer *srv);

/** Shut down the debug server gracefully.
 * Closes the socket and stops the background thread.
 * Safe to call repeatedly (no-op if not running).
 *
 * @param srv  Debug server (from debug_server_create)
 */
void debug_server_destroy(DebugServer *srv);

#endif /* DEBUG_API_H */
```

### Usage in `frontends/gpu/main.c`

```c
static DebugServer *debug_srv = NULL;

int main(int argc, char *argv[]) {
    /* ... emulator initialization ... */
    
    /* Start debug server if SDL_GPU is initialized */
    debug_srv = debug_server_create(&video_gpu_chain, &audio_gpu,
                                    "/tmp/nes_gpu_debug.sock");
    if (!debug_srv) {
        fprintf(stderr, "Warning: debug server failed to start\n");
    }
    
    while (running) {
        /* ... normal emulation ... */
        
        /* Run GPU chains */
        chain_run(&video_gpu_chain, gpu);
        chain_run(&audio_gpu, gpu);
        
        /* Send snapshots to visualiser and process incoming requests */
        if (debug_srv) debug_server_poll(debug_srv);
        
        /* ... display, audio callback, etc ... */
    }
    
    if (debug_srv) debug_server_destroy(debug_srv);
    return 0;
}
```

---

## Implementation Phases

### Phase 1: Foundation (1 week)
- [ ] Implement `debug_api.c` (socket listener, snapshot building)
- [ ] Create Swift `EmulatorConnection` class
- [ ] Define `ChainSnapshot` decodable struct and binary format

### Phase 2: Core UI (1 week)
- [ ] `NodeGraphView`: stage list with enable/bypass toggles
- [ ] `ParameterEditorView`: basic slider for one RC filter stage
- [ ] `PresetView`: preset selector dropdown

### Phase 3: Oscilloscope (1.5 weeks)
- [ ] `OscilloscopeView` with basic single-trace display
- [ ] Metal shader for line rasterization
- [ ] Before/after overlay mode
- [ ] Basic zoom/pan controls

### Phase 4: Advanced Features (1.5 weeks)
- [ ] FFT frequency-domain display
- [ ] A/B split-screen with synchronized zoom
- [ ] Performance dashboard
- [ ] Shader debug view

### Phase 5: Polish (0.5 weeks)
- [ ] Undo/redo in parameter editor
- [ ] Preset favorites
- [ ] Settings (socket path, update rate, buffer size)
- [ ] Documentation + help menu

---

## Performance Considerations

1. **Socket Bandwidth**: A full snapshot with waveform tap (~50KB) sent at 15fps = ~750 KB/s. Acceptable.

2. **Metal Rendering**: Oscilloscope waveform rasterization is GPU-accelerated but simple (non-antialiased lines). Target: 60fps UI with custom shader.

3. **FFT on Demand**: Frequency-domain conversion only computed when user toggles to FFT mode. Cached until new snapshot arrives.

4. **Ring Buffer for History**: Performance dashboard maintains a ring of 600 entries (10s @ 60fps). Memory: ~30KB.

5. **Background Thread for Socket**: Prevents network I/O from blocking the UI. Snapshot delivery delayed by 1 frame.

---

## Future Extensions

1. **GPU-Side FFT**: Offload FFT computation to a Metal shader (compute) for real-time spectrum analysis.

2. **Waveform Recording**: Record snapshots to disk for offline analysis. Export as CSV or WAV for external tools (Audacity, MATLAB).

3. **Parameter Curves**: Time-series editor for RC filter cutoff vs. frame number. Useful for A/B testing.

4. **Live Preset Morphing**: Smooth interpolation between two presets over N frames.

5. **Remote Connection**: Use mDNS or manual IP entry to connect to emulator running on another machine (e.g., Raspberry Pi running NES).

6. **Automated Testing**: Batch-process a set of ROMs with different presets, measure perceptual metrics (cross-correlation, spectral distance).

---

## Security & Stability

1. **Socket Permissions**: `DebugServer` creates the socket with mode 0600 (owner-only access). Visualiser verifies via `SO_PEERCRED`.

2. **Buffer Overflow Protection**: All messages include size fields. Parser validates before copying.

3. **Graceful Degradation**: If socket closes, visualiser reconnects on next poll. If emulator disconnects, visualiser shows "Connection Lost" and waits.

4. **Crash Dump**: If visualiser crashes, emulator continues running (socket listener is separate thread).

---

## Testing Strategy

1. **Mock Emulator**: Create a test harness that sends pre-recorded snapshots. Verify UI rendering.

2. **Unit Tests**: Test `BufferReader` (binary message decoding) with fuzzy data.

3. **Integration Test**: Run emulator + visualiser, verify lag < 1 frame.

4. **Stress Test**: Send maximum-size snapshots (full waveform tap) at 60fps for 10 minutes. Monitor memory.

---

## References

- Unix Domain Sockets: `man 7 unix`
- Metal Compute Shaders: Apple Metal Programming Guide
- SwiftUI State Management: https://developer.apple.com/documentation/swiftui/managing-user-interface-state
- Binary Protocol Design: https://en.wikipedia.org/wiki/Comparison_of_data_serialization_formats
