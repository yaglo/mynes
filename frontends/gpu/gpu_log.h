/*
 * gpu_log.h -- Minimal verbosity gate for the GPU frontend.
 *
 * By default the frontend prints only essential startup info (backend,
 * preset name, ROM header, errors). Everything else — per-pipeline
 * shader loads, FIR taps, chain-stage table, beam tuning, demod
 * parameters — is gated behind gpu_verbose, which is set from the
 * --verbose CLI flag in main.c.
 *
 * Usage:
 *   if (gpu_verbose) printf("...");
 *   LOGV("...");  // variadic wrapper, skipped when !gpu_verbose
 */
#ifndef GPU_LOG_H
#define GPU_LOG_H

#include <stdbool.h>
#include <stdio.h>

extern bool gpu_verbose;

#define LOGV(...) do { if (gpu_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

#endif /* GPU_LOG_H */
