# ChainVisualiser

A macOS SwiftUI app that connects to the experimental GPU frontend's
debug IPC socket and shows live oscilloscope traces, per-stage
waveforms, a node-graph of the signal chain, and a performance
dashboard.

This is a development tool — not needed for normal use of the
emulator.

## Build

```bash
cd tools/visualiser
swift build -c release
```

Produces `.build/release/ChainVisualiser`.

## Run

1. Start the GPU frontend with the debug socket enabled:

   ```bash
   ./build/bin/nes_gpu --debug-server path/to/game.nes
   ```

2. In a separate terminal, launch the visualiser:

   ```bash
   ./tools/visualiser/.build/release/ChainVisualiser
   ```

The app connects to `/tmp/mynes_gpu_debug.sock` by default.

## Architecture

`EmulatorConnection.swift` handles the Unix-socket protocol (see
`frontends/gpu/debug_server.h` for the wire format). Each signal-chain
stage publishes a snapshot buffer; the app polls at ~30 Hz and renders
via SwiftUI + Charts.

Platform: macOS 14+ only (uses SwiftUI macOS idioms).
