/*
 * gpu_process.c — Process a raw composite waveform through the REAL GPU pipeline
 * ================================================================================
 *
 * Usage: gpu_process <waveform.raw> <preset.json> <output.ppm>
 *
 * Runs the exact same GPU shaders as the real-time emulator.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <SDL3/SDL.h>

#include "signal_format.h"
#include "signal_precompute.h"
#include "video_chain.h"
#include "video_gpu.h"
#include "gpu_compute.h"
#include "presets.h"
#include "preset_json.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Build color matrix (replicated from preset_apply.c). */
static void build_color_matrix(float matrix[3][3], float bias[3],
                                const VideoChain *vc,
                                float contrast, float brightness,
                                float chroma_gain) {
    float hue_rad = vc->tv.hue_offset * (float)M_PI / 180.0f;
    float sat = vc->tv.saturation;
    float ch = cosf(hue_rad), sh = sinf(hue_rad);

    const float base[3][3] = {
        { 1.0f,  1.1222f,  0.7391f },
        { 1.0f, -0.3192f, -0.7384f },
        { 1.0f, -1.2374f,  1.9058f },
    };

    float temp_norm = (vc->tv.color_temperature - 6500.0f) / 3500.0f;
    float dr = vc->tv.r_drive * (1.0f + temp_norm * 0.03f);
    float dg = vc->tv.g_drive * (1.0f + temp_norm * 0.01f);
    float db = vc->tv.b_drive * (1.0f - temp_norm * 0.03f);
    float cg = chroma_gain;

    matrix[0][0] = dr * contrast;
    matrix[0][1] = dr * sat * cg * (base[0][1] * ch - base[0][2] * sh);
    matrix[0][2] = dr * sat * cg * (base[0][1] * sh + base[0][2] * ch);
    matrix[1][0] = dg * contrast;
    matrix[1][1] = dg * sat * cg * (base[1][1] * ch - base[1][2] * sh);
    matrix[1][2] = dg * sat * cg * (base[1][1] * sh + base[1][2] * ch);
    matrix[2][0] = db * contrast;
    matrix[2][1] = db * sat * cg * (base[2][1] * ch - base[2][2] * sh);
    matrix[2][2] = db * sat * cg * (base[2][1] * sh + base[2][2] * ch);

    bias[0] = vc->tv.r_cutoff + brightness * dr;
    bias[1] = vc->tv.g_cutoff + brightness * dg;
    bias[2] = vc->tv.b_cutoff + brightness * db;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (h & 0x8000) << 16;
    uint32_t expo = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x03FF;
    if (expo == 0) return 0.0f;
    if (expo == 31) return sign ? -1e30f : 1e30f;
    uint32_t f = sign | ((expo + 112) << 23) | (mant << 13);
    float result;
    memcpy(&result, &f, 4);
    return result;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <waveform.raw> <preset.json> <output.ppm>\n", argv[0]);
        fprintf(stderr, "  preset.json: path to a preset from presets/ (e.g. presets/studio_pvm.json)\n");
        return 1;
    }

    const char *input_path = argv[1];
    const char *preset_path = argv[2];
    const char *output_path = argv[3];

    /* Load waveform */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) { fprintf(stderr, "Cannot open %s\n", input_path); return 1; }
    fseek(fin, 0, SEEK_END);
    long fsize = ftell(fin);
    fseek(fin, 0, SEEK_SET);
    float *waveform = fsize > 0 ? (float *)malloc((size_t)fsize) : NULL;
    if (!waveform || fread(waveform, 1, (size_t)fsize, fin) != (size_t)fsize) {
        fprintf(stderr, "Cannot read %s\n", input_path); return 1;
    }
    fclose(fin);

    /* SDL init */
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_Window *window = SDL_CreateWindow("gpu_process", 320, 240, SDL_WINDOW_HIDDEN);
    SDL_GPUDevice *gpu = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, true, NULL);
    if (!gpu) { fprintf(stderr, "GPU: %s\n", SDL_GetError()); return 1; }
    SDL_ClaimWindowForGPUDevice(gpu, window);
    printf("GPU: %s\n", SDL_GetGPUDeviceDriver(gpu));

    /* Preset (loaded from JSON) */
    PhysicalPreset loaded;
    if (!preset_json_load(&loaded, preset_path)) {
        fprintf(stderr, "Failed to load preset: %s\n", preset_path);
        return 1;
    }
    const PhysicalPreset *preset = &loaded;
    printf("Preset: %s\n", preset->name[0] ? preset->name : preset_path);

    /* Video chain */
    VideoChain chain;
    video_chain_init_preset(&chain, preset->connection, preset->comb_type,
                            preset->region);
    chain.tv = preset->tv;
    chain.cable = preset->video_cable;
    chain.rf = preset->rf;
    chain.console_coupling_R = preset->console_coupling_R;
    chain.console_coupling_C = preset->console_coupling_C;
    chain.console_amp_bw = preset->console_amp_bw;
    chain.console_psu_hum = preset->console_psu_hum;

    /* FIR taps */
    float actual_fs = 3.579545e6f * 12.0f;
    float y_cut = fmaxf(fminf(chain.tv.luma_bandwidth / actual_fs, 0.2f), 0.005f);
    float c_cut = fmaxf(fminf(chain.tv.chroma_bandwidth / actual_fs, 0.2f), 0.005f);
    int y_n = ((int)(0.45f / y_cut)) | 1;
    int c_n = ((int)(0.45f / c_cut)) | 1;
    y_n = y_n < 37 ? 37 : (y_n > 63 ? 63 : y_n);
    c_n = c_n < 21 ? 21 : (c_n > 63 ? 63 : c_n);

    float fir_y[64], fir_c[64], fir_q[64];
    signal_design_fir_ex(fir_y, y_n, y_cut, chain.tv.fir_ringing);
    signal_design_fir_ex(fir_c, c_n, c_cut, chain.tv.fir_ringing);
    /* Q-channel FIR: follow chroma_q_bandwidth if the preset sets one,
     * otherwise equi-band with I (matches pre-split behavior). */
    int q_n = c_n;
    float q_cut = c_cut;
    if (chain.tv.chroma_q_bandwidth > 1.0f) {
        q_cut = chain.tv.chroma_q_bandwidth / actual_fs;
        if (q_cut < 0.005f) q_cut = 0.005f;
        if (q_cut > 0.200f) q_cut = 0.200f;
        q_n = ((int)(0.45f / q_cut)) | 1;
        if (q_n < 5) q_n = 5;
        if (q_n > 63) q_n = 63;
    }
    signal_design_fir_ex(fir_q, q_n, q_cut, chain.tv.fir_ringing);
    printf("FIR: Y %d taps (%.4f), I %d taps (%.4f), Q %d taps (%.4f)\n",
           y_n, y_cut, c_n, c_cut, q_n, q_cut);

    /* Shader dir */
    char shader_dir[512];
    snprintf(shader_dir, sizeof(shader_dir), "%s../shaders/compute", SDL_GetBasePath());

    /* Init GPU chain */
    VideoGPUChain vgc;
    if (!video_gpu_init(&vgc, gpu, &chain, shader_dir, fir_y, y_n, fir_c, c_n,
                        fir_q, q_n)) {
        fprintf(stderr, "video_gpu_init failed\n");
        return 1;
    }

    /* video_gpu_process uploads one whole field in the chain's own format
     * and downloads three floats for each of its samples, so the file must
     * be exactly that field. */
    const SignalFormat *fmt = &vgc.signal_fmt;
    int spl = fmt->samples_per_line;
    int num_lines = fmt->lines;
    long field_bytes = (long)fmt->total_samples * (long)sizeof(float);
    if (fsize != field_bytes) {
        fprintf(stderr, "%s: %ld bytes; this preset's chain takes %d lines x %d float32 samples "
                "(%ld bytes)\n", input_path, fsize, num_lines, spl, field_bytes);
        return 1;
    }
    printf("Loaded: %d lines × %d samples\n", num_lines, spl);

    /* Color matrix */
    float contrast = preset->contrast > 0 ? preset->contrast : 1.0f;
    float brightness = preset->brightness;
    float chroma_gain = preset->chroma_gain > 0 ? preset->chroma_gain : 1.3f;
    float mat[3][3], bias[3];
    build_color_matrix(mat, bias, &chain, contrast, brightness, chroma_gain);
    video_gpu_set_color_matrix(&vgc, mat, bias);

    /* Beam params */
    int beam_w = 1024, beam_rps = 12;
    int beam_h = num_lines * beam_rps;
    float sig_n = 0.35f - chain.tv.beam_sharpness * 0.20f;
    float sig_w = 0.30f + chain.tv.beam_height_max * 0.33f;
    video_gpu_set_beam_params(&vgc, gpu, beam_w, beam_h, beam_rps, sig_n, sig_w);
    vgc.beam_h_blur_sigma = chain.tv.beam_spot_size > 0 ? chain.tv.beam_spot_size : 6.0f;
    printf("Beam: %dx%d, σn=%.2f σw=%.2f spot=%.1f\n",
           beam_w, beam_h, sig_n, sig_w, vgc.beam_h_blur_sigma);

    /* Process */
    float *rgb_out = (float *)calloc((size_t)fmt->total_samples * 3, sizeof(float));
    if (!rgb_out) { fprintf(stderr, "Out of memory\n"); return 1; }
    printf("Processing...\n");
    if (!video_gpu_process(&vgc, gpu, waveform, rgb_out)) {
        fprintf(stderr, "video_gpu_process failed\n");
        return 1;
    }

    /* Download beam */
    int beam_bytes = beam_w * beam_h * 8;
    uint8_t *beam = (uint8_t *)calloc(beam_bytes, 1);
    video_gpu_download_beam(&vgc, gpu, beam);
    printf("Downloaded %d bytes\n", beam_bytes);

    /* Write PPM */
    FILE *fout = fopen(output_path, "wb");
    if (!fout) { fprintf(stderr, "Cannot write %s\n", output_path); return 1; }
    fprintf(fout, "P6\n%d %d\n255\n", beam_w, beam_h);
    for (int i = 0; i < beam_w * beam_h; i++) {
        uint32_t rg, ba;
        memcpy(&rg, beam + i * 8, 4);
        memcpy(&ba, beam + i * 8 + 4, 4);
        uint8_t rgb[3] = {
            (uint8_t)fminf(fmaxf(half_to_float(rg & 0xFFFF) * 255, 0), 255),
            (uint8_t)fminf(fmaxf(half_to_float((rg >> 16) & 0xFFFF) * 255, 0), 255),
            (uint8_t)fminf(fmaxf(half_to_float(ba & 0xFFFF) * 255, 0), 255),
        };
        fwrite(rgb, 3, 1, fout);
    }
    bool write_failed = ferror(fout) != 0;
    if (fclose(fout) != 0 || write_failed) {
        fprintf(stderr, "Cannot write %s\n", output_path);
        return 1;
    }
    printf("Saved: %s (%dx%d)\n", output_path, beam_w, beam_h);

    free(beam); free(rgb_out); free(waveform);
    video_gpu_destroy(&vgc, gpu);
    SDL_DestroyGPUDevice(gpu);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
