# Tang Nano USB video output (SDL2 frontend)

This optional output sends each completed 256×240 PPU frame, including its nine
colour/emphasis bits, to a Tang Nano 20K running the custom **CRT Tang Bridge**
BL616 firmware and matching FPGA video image. It keeps the Mac preview running.
Stock Sipeed debugger firmware is not a video receiver. The GPU frontend does
not implement this output.

## Build and run

Install SDL2, libusb and pkg-config (macOS: `brew install sdl2 libusb pkg-config`).
The transport sources are included here; no separate checkout is needed to build.

```sh
cmake -S . -B build-crt -DNES_CRT_USB=ON
cmake --build build-crt --target mynes -j
./build-crt/bin/mynes --crt-usb your-ntsc-rom.nes
```

Omit the ROM path to open the ROM browser. `MYNES_CRT_USB=1` is equivalent to
`--crt-usb`. USB is disabled unless requested at launch. A build without
`NES_CRT_USB` reports a clear error if USB output is requested. Build systems
without Chicken Scheme/SRFI dependencies can use
`-DCSI_EXECUTABLE=CSI_EXECUTABLE-NOTFOUND` to use committed generated CPU code.

Connect only one programmed bridge. Do not run another USB sender or emulator
instance against it simultaneously. An unavailable device fails startup with an
error. A transfer/FPGA validation failure during emulation stops USB output and
lets the Mac preview continue; restart mynes after reconnecting the device.

Only emulated PPU frames are sent: Mac menus, shaders, ROM browser and overlays
are not part of the CRT output. Pausing or entering the ROM browser stops new
PPU submissions; the FPGA retains its last picture. Use O to change ROMs.

## Transport and limits

Frames are packed into 69,120 bytes with CRC32 and sequence checks. A background
worker keeps one in-flight frame and one replaceable pending frame, so a slow
receiver does not accumulate an ever-growing queue. The protocol accepts the
32-byte legacy and 40-byte extended acknowledgements; FPGA memory/missed-line
and extended overflow errors stop the stream. Shutdown statistics are sampled
before joining the last in-flight transfer and may undercount that final frame.

This prototype targets NTSC-like 240p. The current FPGA raster is 60.0895 Hz,
not exactly the NES clock; there is no CRT clock feedback or audio correction.
Independent clocks can repeat/drop frames. PAL output is not supported by this
FPGA profile. The FPGA palette is fixed at build time and is not synchronized
to mynes palette selection. USB acknowledgement verifies reception, not that a
frame appeared on the physical CRT.

The board needs the separate CRT project's firmware, FPGA image, and analog
RGB DAC/buffer circuit. Do not wire FPGA GPIO directly to monitor BNC inputs.

Optional `MYNES_CRT_CAPTURE=/path/frames.bin` writes packed PPU frames for replay
with the CRT project's sender (overwrites the file, roughly 249 MB/minute).
Capture has no timestamps or palette metadata and is synchronous file I/O.

## Validation

```sh
cmake -S . -B build-crt -DNES_CRT_USB=ON -DNES_BUILD_TESTS=ON
cmake --build build-crt --target test_crt_protocol
ctest --test-dir build-crt -R '^crt_protocol$' --output-on-failure
```

The protocol test checks every packed pixel, all emphasis values, CRC32/header
fields, both acknowledgement lengths, and rejection of faults. Hardware testing
requires the programmed Tang; host tests do not establish analog signal levels.
