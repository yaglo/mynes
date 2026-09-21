# Bundled SDL3

macOS builds default to `MYNES_BUNDLED_SDL3=ON`. CMake FetchContent downloads
the official SDL 3.4.16 release archive, verifies its SHA-256, applies the
patches here and links SDL statically. No system installation is modified.
The extracted source and SDL's zlib license are under `build/_deps/`.
An offline build may supply the source with
`FETCHCONTENT_SOURCE_DIR_MYNES_SDL3_SOURCE`; the source may be pristine or
already patched. Configuration verifies the Metal file's complete hash.

The upstream fixes are:

- [10309e3](https://github.com/libsdl-org/SDL/commit/10309e32409032d07170a85727b948b5b1f6bb55): return the fence while holding the submission lock. Otherwise another thread can recycle the command buffer before the caller receives its fence.
- [d8451e5](https://github.com/libsdl-org/SDL/commit/d8451e52d51e3c21f03ec81d6b28099386ce7837): keep the command buffer's own fence reference, and clear the Metal handle only on final release. This protects concurrent cleanup and swapchain waits.

The patches retain their upstream author and commit metadata. SDL 3.4.16
without these fixes can race when GPU audio and video submit on separate
threads. Disabling GPU audio only removes one trigger.

`mynes-metal-presentation.patch` is a small local extension, not an upstream
fix. Window properties `mynes.gpu.metal.present_interval_ns` and
`mynes.gpu.metal.source_frame` opt into `presentDrawable:atTime:`. Without
the interval property, SDL's normal presentation behavior is unchanged.
The deadline starts two source intervals ahead; subsequent deadlines follow
source-frame numbers. A long stall rebases the schedule. This gives GPU work
time to finish before presentation instead of sleeping before submission.

`MYNES_METAL_PRESENT_TRACE=1` logs sequence number, `MTLDrawable.presentedTime`,
requested deadline, submission time (all in Core Animation seconds), and
source-frame number. Handlers capture values only, never window pointers.
Zero presented timestamps are excluded from visible-cadence measurements.

`-DMYNES_BUNDLED_SDL3=OFF` explicitly opts into system SDL. Other platforms
default to system SDL. The application polls swapchain capacity and cancels
empty acquisition attempts, allowing events to be processed between retries;
the platform's drawable acquisition may still wait for vblank.

`gpu_fence_tests` verifies 20,000 concurrent upload/readback/fence cycles on
one device. Its timeout also catches a stuck fence. This stress test does not
reproduce every possible interleaving on every run.
