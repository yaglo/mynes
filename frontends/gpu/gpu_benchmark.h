#ifndef GPU_BENCHMARK_H
#define GPU_BENCHMARK_H
#include "video_gpu.h"
#include "signal_precompute.h"

/* Complete resident DAC -> CRT render, fenced per frame; no vsync or audio. */
bool gpu_benchmark(VideoGPUChain *video, SDL_GPUDevice *gpu,
                   const SignalPrecompute *signal, const char *render_shader_dir, bool pixel_aligned,
                   int panel_subpixels);
#endif
