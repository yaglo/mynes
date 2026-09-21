# Signal Studio

Native macOS editor for the SDL3 GPU frontend. The graph follows five physical sections: connection, decoder, beam, phosphor, and glass. Controls change the live emulator through the same update functions as its OSD. The separate Audio controls button opens amplifier, mains-hum and noise controls; these values participate in preset save/load and dirty-state tracking.

[See the rendered results](../../docs/nes-visual-showcase.md) · [Four CRT references and native close-ups](../../docs/gpu-beam-closeups.md)

## Run

From the repository root:

```sh
./build/bin/mynes_gpu --debug-server
swift run --package-path tools/visualiser -c release
```

The emulator opens its ROM browser when no ROM path is supplied. Both programs use `/tmp/mynes_gpu_debug.sock`; set `MYNES_DEBUG_SOCKET` to the same alternate path in both processes to isolate a worktree.

## Presets

The selector shows the active preset and whether its settings have been edited. Switch between bundled and user presets, save changes to a user preset, save a copy, rename, or delete a user preset. Bundled files are read-only through this UI. Switching presets preserves the running game's NTSC/PAL region. Deleting the active user preset leaves its live settings available as an unsaved setup.

Files are managed by the emulator, under `$XDG_CONFIG_HOME/mynes/presets` or `~/.config/mynes/presets`. Writes use a temporary file and atomic rename. The editor does not need filesystem access to that directory. Catalog revisions reject commands referring to a stale list.

The execution list reports CPU command-encoding time, **not GPU execution time**. The footer estimates frame rate from received frame counters. Do not use it as a GPU benchmark on a contended machine.

## Protocol and checks

Little-endian messages use a two-word header: type and payload byte count.

| Type | Direction | Payload |
| --- | --- | --- |
| 0 | server → editor | Existing execution-stage snapshot |
| 5 | server → editor | Version 1, count, 72-byte physical-control records |
| 6 | editor → server | Control ID and float32 value |
| 7 | server → editor | Version 1, revision, count, signed active ID, modified flag; 136-byte preset records (ID, user flag, name[128]) |
| 8 | editor → server | Operation, ID, revision, UTF-8 name[128]; operations 1 load, 2 save, 3 save-as, 4 rename, 5 delete |
| 9 | server → editor | Operation, success flag, error[128] |

Incoming values are bounded and must be finite. Incomplete messages remain queued until complete. The catalog refreshes twice a second; execution/controls update at most 4 Hz. Waveform-tap protocol types remain reserved; the current editor does not expose waveform traces.

```sh
swift test --package-path tools/visualiser
python3 frontends/gpu/tests/test_editor_ipc.py build/bin/mynes_gpu
```

The IPC test launches a separate frontend with temporary user configuration and tests live edits, preset CRUD, topology changes, read-only bundled files, and stale-command rejection.

The UI uses property-level Observation. Topology, controls and catalogs publish
only when their values change. Timing updates affect only the timing view; the
frame/FPS footer refreshes once a second using server timestamps. To verify a
CPU sample covers a live connection, launch with `MYNES_EDITOR_LOG_FRAMES=1` and
check that received frame numbers keep advancing throughout the sample.
