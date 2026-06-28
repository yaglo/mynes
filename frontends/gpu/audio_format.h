/*
 * Audio Format — CPU→GPU Interface Contract for the Audio Signal Path
 * ====================================================================
 *
 * This header defines the buffer format for the raw APU output that the
 * CPU produces and the GPU consumes for analog signal chain simulation.
 *
 * Physical model:
 *   The NES 2A03 has 5 audio channels (2 pulse, 1 triangle, 1 noise,
 *   1 DMC) that output digital values at the CPU clock rate (~1.789773
 *   MHz for NTSC). These feed through two separate DAC circuits (pulse
 *   DAC and TND DAC) whose outputs are summed through a resistor network.
 *
 *   The combined analog signal then passes through:
 *   1. Coupling capacitor (DC blocking, RC high-pass ~16-90 Hz)
 *   2. Output amplifier (gain, bandwidth limit ~8-14 kHz, feedback
 *      network with RC high-pass at ~440 Hz)
 *   3. Cable (capacitance → RC low-pass, varies with length)
 *   4. TV audio input (coupling cap, tone controls, power amp)
 *   5. Speaker (resonance, cabinet, cone breakup)
 *   6. Decimation to output sample rate (44.1 or 48 kHz)
 *
 *   Our CPU already handles channel state machines + DAC + mixing at
 *   cycle-accurate timing. The GPU audio chain picks up AFTER the DAC
 *   — it receives the mixed analog voltage and runs it through the
 *   analog signal path (stages 3-6 above).
 *
 * Upload granularity:
 *   One frame's worth of samples uploaded per frame (~29780 for NTSC,
 *   ~33248 for PAL). The GPU processes the entire frame as one dispatch.
 *
 * IMPORTANT: The GPU audio chain is for verification and offline
 * rendering — NOT real-time playback. CPU audio is the primary real-time
 * path (avoids GPU readback latency). The GPU chain enables A/B testing,
 * batch recording, and future use when readback latency can be hidden.
 */

#ifndef AUDIO_FORMAT_H
#define AUDIO_FORMAT_H

#include <stdint.h>

/* ============================================================================
 * Audio buffer format
 * ============================================================================ */

/* NES CPU clock rates (Hz). The APU generates one raw sample per CPU cycle. */
#define AUDIO_NTSC_CPU_CLOCK   1789773
#define AUDIO_PAL_CPU_CLOCK    1662607

/* Frames per second. */
#define AUDIO_NTSC_FPS         60
#define AUDIO_PAL_FPS          50

/* Raw APU samples per frame (at CPU clock rate, before any decimation).
 * This is the INPUT to the GPU audio chain. */
#define AUDIO_NTSC_SAMPLES_PER_FRAME   (AUDIO_NTSC_CPU_CLOCK / AUDIO_NTSC_FPS)  /* ~29,829 */
#define AUDIO_PAL_SAMPLES_PER_FRAME    (AUDIO_PAL_CPU_CLOCK / AUDIO_PAL_FPS)    /* ~33,252 */
#define AUDIO_MAX_SAMPLES_PER_FRAME    AUDIO_PAL_SAMPLES_PER_FRAME

/* Output sample rate (what the speaker/headphones receive). */
#define AUDIO_OUTPUT_SAMPLE_RATE       48000

/* Output samples per frame (after decimation). */
#define AUDIO_OUTPUT_NTSC_SAMPLES      (AUDIO_OUTPUT_SAMPLE_RATE / AUDIO_NTSC_FPS)  /* 800 */
#define AUDIO_OUTPUT_PAL_SAMPLES       (AUDIO_OUTPUT_SAMPLE_RATE / AUDIO_PAL_FPS)   /* 960 */

/* ============================================================================
 * Buffer layout
 * ============================================================================
 *
 * Input buffer (CPU → GPU):
 *   1D array of float32, one sample per CPU cycle.
 *   Value range: [-1.0, 1.0] (normalized from the DAC output voltage).
 *   The CPU's existing APU pipeline produces these via:
 *     channel state machines → nonlinear DAC LUT → mixer sum
 *
 *   The buffer includes the FULL analog character that the CPU already
 *   models: DAC nonlinearity, mixer interaction, DMC crosstalk. The GPU
 *   chain adds: coupling cap, amp stages, cable, TV, speaker.
 *
 * Output buffer (GPU → CPU readback):
 *   1D array of float32, decimated to output sample rate.
 *   Value range: [-1.0, 1.0] (ready for audio callback).
 *   ~800 samples for NTSC, ~960 for PAL.
 *   Uses storageModeShared for CPU readback (or blit to shared buffer).
 *
 * Intermediate buffers (GPU only, ping-pong):
 *   Same size as input (~30K floats). Two buffers for sequential stages.
 */

#define AUDIO_INPUT_BYTES(n)    ((n) * sizeof(float))
#define AUDIO_NTSC_INPUT_BYTES  AUDIO_INPUT_BYTES(AUDIO_NTSC_SAMPLES_PER_FRAME)   /* ~116 KB */
#define AUDIO_PAL_INPUT_BYTES   AUDIO_INPUT_BYTES(AUDIO_PAL_SAMPLES_PER_FRAME)    /* ~130 KB */

#define AUDIO_OUTPUT_BYTES(n)   ((n) * sizeof(float))
#define AUDIO_NTSC_OUTPUT_BYTES AUDIO_OUTPUT_BYTES(AUDIO_OUTPUT_NTSC_SAMPLES)      /* ~3.2 KB */
#define AUDIO_PAL_OUTPUT_BYTES  AUDIO_OUTPUT_BYTES(AUDIO_OUTPUT_PAL_SAMPLES)       /* ~3.8 KB */

/* ============================================================================
 * GPU dispatch dimensions
 * ============================================================================
 * Audio buffers are small enough (~30K samples) that a single workgroup
 * can process the entire frame for pointwise operations. For prefix scan
 * (RC filter), we use AUDIO_WORKGROUP_SIZE and dispatch
 * ceil(samples / workgroup_size) groups.
 *
 * Each RC filter stage within a frame has inter-block carry dependency
 * (sequential between blocks) but the blocks themselves are parallel
 * within the Blelloch scan. For ~30K samples at 1024-sample blocks,
 * that's ~30 blocks × 2·log₂(1024) = ~600 scan steps. Fast.
 */
#define AUDIO_WORKGROUP_SIZE    1024
#define AUDIO_NTSC_BLOCKS       ((AUDIO_NTSC_SAMPLES_PER_FRAME + AUDIO_WORKGROUP_SIZE - 1) / AUDIO_WORKGROUP_SIZE)  /* ~30 */
#define AUDIO_PAL_BLOCKS        ((AUDIO_PAL_SAMPLES_PER_FRAME + AUDIO_WORKGROUP_SIZE - 1) / AUDIO_WORKGROUP_SIZE)   /* ~33 */

/* ============================================================================
 * Component parameter structures
 * ============================================================================
 * These define the physical components in the audio signal chain.
 * R and C values are user-facing (physically meaningful); the conversion
 * to IIR coefficients (a, b) happens at parameter-set time, not per-sample.
 *
 * Each structure maps to one or more compute kernel dispatches.
 */

/* RC filter coefficients (precomputed from R, C, sample_rate). */
typedef struct {
    float a;        /* feedback coefficient: exp(-dt/(R·C)) for LP, or 1/(1+2π·fc·dt) for HP */
    float b;        /* input coefficient: 1-a for LP, a for HP */
} RCCoeffs;

/* Hardware variant filter corners (Hz). These are the physical values
 * from NES hardware schematics. The GPU chain converts them to
 * RCCoeffs at the audio processing sample rate (~1.79 MHz). */
typedef struct {
    double hp1_hz;      /* coupling cap high-pass (DC blocking): 16-90 Hz */
    double hp2_hz;      /* feedback network high-pass: ~440 Hz */
    double lp_hz;       /* amplifier bandwidth low-pass: 8000-14000 Hz */
} AudioFilterCorners;

/* Predefined corners per hardware variant. */
#define AUDIO_CORNERS_FAMICOM       ((AudioFilterCorners){ 16.0,  440.0, 10000.0 })
#define AUDIO_CORNERS_NES_FRONT     ((AudioFilterCorners){ 90.0,  440.0, 14000.0 })
#define AUDIO_CORNERS_NES_TOP       ((AudioFilterCorners){ 90.0,  440.0, 12000.0 })
#define AUDIO_CORNERS_DENDY         ((AudioFilterCorners){ 37.0,  440.0,  8000.0 })

/* Speaker model parameters. */
typedef struct {
    float resonance_hz;     /* fundamental resonance (200-400 Hz for small TV) */
    float bandwidth_low;    /* usable low-frequency limit (Hz) */
    float bandwidth_high;   /* usable high-frequency limit (Hz) */
    float cabinet_q;        /* cabinet resonance Q factor (0.5-5.0) */
    float cone_breakup_hz;  /* frequency where cone breaks up (~5 kHz) */
} SpeakerParams;

/* Predefined speaker profiles. */
#define SPEAKER_SMALL_TV     ((SpeakerParams){ 350.0f, 400.0f, 6000.0f, 2.0f, 5000.0f })
#define SPEAKER_CONSOLE_TV   ((SpeakerParams){ 150.0f,  80.0f, 10000.0f, 1.5f, 6000.0f })
#define SPEAKER_PVM          ((SpeakerParams){ 100.0f,  60.0f, 15000.0f, 0.8f, 8000.0f })
#define SPEAKER_ARCADE       ((SpeakerParams){ 200.0f, 100.0f, 8000.0f, 3.0f, 4000.0f })
#define SPEAKER_HEADPHONES   ((SpeakerParams){  20.0f,  20.0f, 20000.0f, 0.7f, 15000.0f })
#define SPEAKER_FAMICOM_RF   ((SpeakerParams){ 400.0f, 500.0f, 4000.0f, 2.5f, 3500.0f })

/* Runtime audio format descriptor. */
typedef struct {
    int region;                 /* SIGNAL_REGION_NTSC or SIGNAL_REGION_PAL */
    int cpu_clock;              /* 1789773 (NTSC) or 1662607 (PAL) */
    int samples_per_frame;      /* ~29829 (NTSC) or ~33252 (PAL) */
    int output_sample_rate;     /* 48000 */
    int output_samples_per_frame; /* 800 (NTSC) or 960 (PAL) */
    int blocks;                 /* dispatch blocks for prefix scan */
} AudioFormat;

/* Initialize an AudioFormat for a given region. */
static inline void audio_format_init(AudioFormat *fmt, int region) {
    fmt->region = region;
    if (region == 1 /* PAL */) {
        fmt->cpu_clock = AUDIO_PAL_CPU_CLOCK;
        fmt->samples_per_frame = AUDIO_PAL_SAMPLES_PER_FRAME;
        fmt->output_samples_per_frame = AUDIO_OUTPUT_PAL_SAMPLES;
    } else {
        fmt->cpu_clock = AUDIO_NTSC_CPU_CLOCK;
        fmt->samples_per_frame = AUDIO_NTSC_SAMPLES_PER_FRAME;
        fmt->output_samples_per_frame = AUDIO_OUTPUT_NTSC_SAMPLES;
    }
    fmt->output_sample_rate = AUDIO_OUTPUT_SAMPLE_RATE;
    fmt->blocks = (fmt->samples_per_frame + AUDIO_WORKGROUP_SIZE - 1)
                  / AUDIO_WORKGROUP_SIZE;
}

#endif /* AUDIO_FORMAT_H */
