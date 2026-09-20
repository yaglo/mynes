/*
 * GPU Display Pipeline — CRT Display-Domain Render Shaders (Implementation)
 * ===========================================================================
 *
 * See gpu_display.h for API documentation and pipeline overview.
 *
 * SDL3 GPU graphics pipeline binding convention for SPIR-V:
 *   Fragment shader:
 *     set 2: Sampled textures (bound via SDL_BindGPUFragmentSamplers)
 *     set 3: Uniform buffers  (pushed via SDL_PushGPUFragmentUniformData)
 *
 * The shaders are compiled from GLSL 450 using glslangValidator with
 * the correct set numbers for SDL3's expected layout.
 */

#include "gpu_display.h"
#include "gpu_log.h"
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Load a SPIR-V file and create an SDL_GPUShader for the given stage.
 * The caller must release the shader after pipeline creation. */
static SDL_GPUShader *load_shader(SDL_GPUDevice *gpu, const char *path,
                                  SDL_GPUShaderStage stage,
                                  Uint32 num_samplers,
                                  Uint32 num_uniform_buffers)
{
    size_t code_size = 0;
    void *code = SDL_LoadFile(path, &code_size);
    if (!code) {
        fprintf(stderr, "gpu_display: failed to load %s: %s\n",
                path, SDL_GetError());
        return NULL;
    }

    if (code_size < 20) {
        fprintf(stderr, "gpu_display: %s too small (%zu bytes)\n",
                path, code_size);
        SDL_free(code);
        return NULL;
    }

    /* Determine shader format. Prefer SPIR-V; fall back to MSL on Metal. */
    SDL_GPUShaderFormat device_fmts = SDL_GetGPUShaderFormats(gpu);
    SDL_GPUShaderFormat use_fmt = SDL_GPU_SHADERFORMAT_SPIRV;
    const char *entry = "main";

    if (!(device_fmts & SDL_GPU_SHADERFORMAT_SPIRV)) {
        SDL_free(code); code = NULL;
        if (device_fmts & SDL_GPU_SHADERFORMAT_MSL) {
            size_t plen = strlen(path);
            char *msl = (char *)malloc(plen + 1);
            if (msl) {
                memcpy(msl, path, plen + 1);
                if (plen >= 4) { msl[plen-3]='m'; msl[plen-2]='s'; msl[plen-1]='l'; }
                code = SDL_LoadFile(msl, &code_size);
                if (code) { use_fmt = SDL_GPU_SHADERFORMAT_MSL; entry = "main0"; }
                else fprintf(stderr, "gpu_display: MSL not found: %s\n", msl);
                free(msl);
            }
        }
        if (!code) {
            fprintf(stderr, "gpu_display: no compatible format for %s\n", path);
            return NULL;
        }
    }

    SDL_GPUShaderCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.code = (const Uint8 *)code;
    ci.code_size = code_size;
    ci.entrypoint = entry;
    ci.format = use_fmt;
    ci.stage = stage;
    ci.num_samplers = num_samplers;
    ci.num_storage_textures = 0;
    ci.num_storage_buffers = 0;
    ci.num_uniform_buffers = num_uniform_buffers;

    SDL_GPUShader *shader = SDL_CreateGPUShader(gpu, &ci);
    SDL_free(code);

    if (!shader) {
        fprintf(stderr, "gpu_display: SDL_CreateGPUShader failed for %s: %s\n",
                path, SDL_GetError());
    }
    return shader;
}

/* Create a graphics pipeline for fullscreen triangle rendering.
 * vertex_shader + fragment_shader are consumed (released after pipeline creation).
 * target_format: the color target format (swapchain format or RGBA16Float).
 * enable_blend: whether to enable alpha blending on the color target. */
static SDL_GPUGraphicsPipeline *create_fullscreen_pipeline(
    SDL_GPUDevice *gpu,
    SDL_GPUShader *vert, SDL_GPUShader *frag,
    SDL_GPUTextureFormat target_format,
    bool enable_blend)
{
    SDL_GPUColorTargetBlendState blend;
    memset(&blend, 0, sizeof(blend));
    if (enable_blend) {
        blend.enable_blend = true;
        blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
        blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

    SDL_GPUColorTargetDescription ct_desc;
    memset(&ct_desc, 0, sizeof(ct_desc));
    ct_desc.format = target_format;
    ct_desc.blend_state = blend;

    SDL_GPUGraphicsPipelineTargetInfo target_info;
    memset(&target_info, 0, sizeof(target_info));
    target_info.color_target_descriptions = &ct_desc;
    target_info.num_color_targets = 1;
    target_info.has_depth_stencil_target = false;

    SDL_GPUGraphicsPipelineCreateInfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.vertex_shader = vert;
    pi.fragment_shader = frag;

    /* No vertex buffers — the vertex shader uses gl_VertexIndex. */
    pi.vertex_input_state.vertex_buffer_descriptions = NULL;
    pi.vertex_input_state.num_vertex_buffers = 0;
    pi.vertex_input_state.vertex_attributes = NULL;
    pi.vertex_input_state.num_vertex_attributes = 0;

    pi.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    /* Rasterizer: fill, no culling (fullscreen triangle). */
    pi.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pi.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pi.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    /* Multisample: disabled. */
    pi.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;

    /* No depth/stencil. */
    /* depth_stencil_state is zero-initialized — no depth test. */

    pi.target_info = target_info;
    pi.props = 0;

    SDL_GPUGraphicsPipeline *pipeline = SDL_CreateGPUGraphicsPipeline(gpu, &pi);
    if (!pipeline) {
        fprintf(stderr, "gpu_display: SDL_CreateGPUGraphicsPipeline failed: %s\n",
                SDL_GetError());
    }

    /* Shaders can be released after pipeline creation. */
    SDL_ReleaseGPUShader(gpu, vert);
    SDL_ReleaseGPUShader(gpu, frag);

    return pipeline;
}

/* Create or recreate a halation FBO texture at the given dimensions. */
static SDL_GPUTexture *create_halation_fbo(SDL_GPUDevice *gpu, int w, int h)
{
    SDL_GPUTextureCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.type = SDL_GPU_TEXTURETYPE_2D;
    ci.format = SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    ci.width = (Uint32)w;
    ci.height = (Uint32)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER | SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;

    SDL_GPUTexture *tex = SDL_CreateGPUTexture(gpu, &ci);
    if (!tex) {
        fprintf(stderr, "gpu_display: failed to create halation FBO %dx%d: %s\n",
                w, h, SDL_GetError());
    }
    return tex;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

bool gpu_display_init(GPUDisplay *d, SDL_GPUDevice *gpu,
                       SDL_Window *window, const char *shader_dir)
{
    int w, h;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    return gpu_display_init_target(d, gpu, SDL_GetGPUSwapchainTextureFormat(gpu, window), w, h, shader_dir);
}

bool gpu_display_init_target(GPUDisplay *d, SDL_GPUDevice *gpu,
    SDL_GPUTextureFormat swapchain_fmt, int win_w, int win_h, const char *shader_dir)
{
    memset(d, 0, sizeof(*d));

    /* Build shader paths. */
    char vert_path[1024], crt_frag_path[1024], blur_frag_path[1024];
    snprintf(vert_path, sizeof(vert_path), "%s/fullscreen.vert.spv", shader_dir);
    snprintf(crt_frag_path, sizeof(crt_frag_path), "%s/crt_display.frag.spv", shader_dir);
    snprintf(blur_frag_path, sizeof(blur_frag_path), "%s/halation_blur.frag.spv", shader_dir);

    /* Get swapchain format for the CRT pipeline's output. */


    /* --- Halation blur pipeline ---
     * Vertex shader: no samplers, no uniforms.
     * Fragment shader: 1 sampler (tex_input), 1 uniform buffer (BlurParams). */
    {
        SDL_GPUShader *vert = load_shader(gpu, vert_path,
                                          SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        SDL_GPUShader *frag = load_shader(gpu, blur_frag_path,
                                          SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        if (!vert || !frag) {
            if (vert) SDL_ReleaseGPUShader(gpu, vert);
            if (frag) SDL_ReleaseGPUShader(gpu, frag);
            return false;
        }
        /* Halation FBOs are RGBA16F — no blending needed. */
        d->pipe_halation = create_fullscreen_pipeline(
            gpu, vert, frag,
            SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT, false);
        if (!d->pipe_halation) return false;
    }

    /* --- CRT display pipeline ---
     * Vertex shader: no samplers, no uniforms.
     * Fragment shader: 2 samplers (composite + halation), 1 UBO (DisplayParams). */
    {
        SDL_GPUShader *vert = load_shader(gpu, vert_path,
                                          SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
        SDL_GPUShader *frag = load_shader(gpu, crt_frag_path,
                                          SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        if (!vert || !frag) {
            if (vert) SDL_ReleaseGPUShader(gpu, vert);
            if (frag) SDL_ReleaseGPUShader(gpu, frag);
            gpu_display_destroy(d, gpu);
            return false;
        }
        /* CRT outputs to swapchain — no blending (shader writes final color). */
        d->pipe_crt = create_fullscreen_pipeline(
            gpu, vert, frag, swapchain_fmt, false);
        if (!d->pipe_crt) {
            gpu_display_destroy(d, gpu);
            return false;
        }
    }

    /* --- Linear sampler --- */
    {
        SDL_GPUSamplerCreateInfo sci;
        memset(&sci, 0, sizeof(sci));
        sci.min_filter = SDL_GPU_FILTER_LINEAR;
        sci.mag_filter = SDL_GPU_FILTER_LINEAR;
        sci.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
        sci.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sci.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sci.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        d->sampler_linear = SDL_CreateGPUSampler(gpu, &sci);
        if (!d->sampler_linear) {
            fprintf(stderr, "gpu_display: failed to create sampler: %s\n",
                    SDL_GetError());
            gpu_display_destroy(d, gpu);
            return false;
        }
    }

    /* --- Halation FBOs at quarter window resolution --- */
    {

        d->halation_w = (win_w > 4) ? win_w / 4 : 1;
        d->halation_h = (win_h > 4) ? win_h / 4 : 1;
        d->tex_halation_a = create_halation_fbo(gpu, d->halation_w, d->halation_h);
        d->tex_halation_b = create_halation_fbo(gpu, d->halation_w, d->halation_h);
        if (!d->tex_halation_a || !d->tex_halation_b) {
            gpu_display_destroy(d, gpu);
            return false;
        }
    }

    d->initialized = true;
    LOGV("gpu_display: initialized (halation %dx%d)\n",
            d->halation_w, d->halation_h);
    return true;
}

void gpu_display_destroy(GPUDisplay *d, SDL_GPUDevice *gpu)
{
    if (!d) return;

    if (d->pipe_halation) {
        SDL_ReleaseGPUGraphicsPipeline(gpu, d->pipe_halation);
        d->pipe_halation = NULL;
    }
    if (d->pipe_crt) {
        SDL_ReleaseGPUGraphicsPipeline(gpu, d->pipe_crt);
        d->pipe_crt = NULL;
    }
    if (d->tex_halation_a) {
        SDL_ReleaseGPUTexture(gpu, d->tex_halation_a);
        d->tex_halation_a = NULL;
    }
    if (d->tex_halation_b) {
        SDL_ReleaseGPUTexture(gpu, d->tex_halation_b);
        d->tex_halation_b = NULL;
    }
    if (d->sampler_linear) {
        SDL_ReleaseGPUSampler(gpu, d->sampler_linear);
        d->sampler_linear = NULL;
    }

    d->initialized = false;
}

void gpu_display_resize(GPUDisplay *d, SDL_GPUDevice *gpu, int win_w, int win_h)
{
    if (!d || !d->initialized) return;

    int new_w = (win_w > 4) ? win_w / 4 : 1;
    int new_h = (win_h > 4) ? win_h / 4 : 1;

    /* Skip if size hasn't changed. */
    if (new_w == d->halation_w && new_h == d->halation_h) return;

    /* Wait for GPU to be idle before releasing textures that may be in flight. */
    SDL_WaitForGPUIdle(gpu);

    if (d->tex_halation_a) SDL_ReleaseGPUTexture(gpu, d->tex_halation_a);
    if (d->tex_halation_b) SDL_ReleaseGPUTexture(gpu, d->tex_halation_b);

    d->halation_w = new_w;
    d->halation_h = new_h;
    d->tex_halation_a = create_halation_fbo(gpu, new_w, new_h);
    d->tex_halation_b = create_halation_fbo(gpu, new_w, new_h);

    LOGV("gpu_display: resized halation FBOs to %dx%d\n", new_w, new_h);
}

void gpu_display_render(GPUDisplay *d, SDL_GPUDevice *gpu,
                         SDL_GPUCommandBuffer *cmd,
                         SDL_GPUTexture *composite_tex, int comp_w, int comp_h,
                         SDL_GPUTexture *swapchain_tex, int sw, int sh,
                         const GPUDisplayParams *params,
                         const SDL_GPUViewport *viewport)
{
    if (!d || !d->initialized || !cmd) return;

    (void)gpu;  /* device not needed for render recording */

    /* -----------------------------------------------------------------------
     * Halation blur uniforms (std140 layout matching BlurParams).
     *
     * BlurParams { vec2 direction; int radius; float threshold; int do_threshold; }
     *
     * std140: vec2 at offset 0 (8 bytes), int at offset 8, float at offset 12,
     *         int at offset 16. Total padded to 32 bytes for safety.
     * ----------------------------------------------------------------------- */
    struct {
        float dir_x, dir_y;   /* vec2  direction     (offset 0)  */
        int   radius;         /* int   radius        (offset 8)  */
        float threshold;      /* float threshold     (offset 12) */
        int   do_threshold;   /* int   do_threshold  (offset 16) */
        float input_gamma;
        int   _pad[2];        /* pad to 32 bytes                 */
    } blur_params;

    /* -----------------------------------------------------------------------
     * Pass 1: Horizontal blur (composite → halation_a)
     *   - Extract bright pixels (do_threshold = 1)
     *   - Blur horizontally
     * ----------------------------------------------------------------------- */
    /* Skip halation blur passes when strength is 0. */
    if (params->halation_strength > 0.001f)
    {
        memset(&blur_params, 0, sizeof(blur_params));
        blur_params.dir_x = 1.0f / (float)d->halation_w;
        blur_params.dir_y = 0.0f;
        blur_params.radius = 16;
        blur_params.threshold = 0.65f;
        blur_params.do_threshold = 1;
        blur_params.input_gamma = params->input_gamma;

        SDL_GPUColorTargetInfo ct;
        memset(&ct, 0, sizeof(ct));
        ct.texture = d->tex_halation_a;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.cycle = true;

        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        if (pass) {
            SDL_BindGPUGraphicsPipeline(pass, d->pipe_halation);

            SDL_GPUTextureSamplerBinding sampler_bind;
            sampler_bind.texture = composite_tex;
            sampler_bind.sampler = d->sampler_linear;
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler_bind, 1);

            SDL_PushGPUFragmentUniformData(cmd, 0, &blur_params,
                                           sizeof(blur_params));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
    }

    /* Pass 2: Vertical blur (halation_a → halation_b). */
    if (params->halation_strength > 0.001f) {
        memset(&blur_params, 0, sizeof(blur_params));
        blur_params.dir_x = 0.0f;
        blur_params.dir_y = 1.0f / (float)d->halation_h;
        blur_params.radius = 16;
        blur_params.threshold = 0.65f;
        blur_params.do_threshold = 0;
        blur_params.input_gamma = 0;

        SDL_GPUColorTargetInfo ct;
        memset(&ct, 0, sizeof(ct));
        ct.texture = d->tex_halation_b;
        ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        ct.cycle = true;

        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        if (pass) {
            SDL_BindGPUGraphicsPipeline(pass, d->pipe_halation);

            SDL_GPUTextureSamplerBinding sampler_bind;
            sampler_bind.texture = d->tex_halation_a;
            sampler_bind.sampler = d->sampler_linear;
            SDL_BindGPUFragmentSamplers(pass, 0, &sampler_bind, 1);

            SDL_PushGPUFragmentUniformData(cmd, 0, &blur_params,
                                           sizeof(blur_params));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
    } /* end halation strength > 0 */

    /* -----------------------------------------------------------------------
     * Pass 3: CRT display composite (composite + halation_b → swapchain)
     *   - Barrel distortion, convergence, phosphor mask, halation blend,
     *     vignette, gamma, black floor
     * -----------------------------------------------------------------------
     *
     * DisplayParams UBO layout (std140, matching crt_display.frag.glsl):
     *
     *   vec2  src_size            offset  0  (8 bytes)
     *   vec2  out_size            offset  8  (8 bytes)
     *   float barrel              offset 16
     *   float barrel_v            offset 20
     *   float convergence_static  offset 24
     *   float convergence_dynamic offset 28
     *   float mask_strength       offset 32
     *   int   mask_type           offset 36
     *   float mask_pitch_pixels   offset 40
     *   float halation_strength   offset 44
     *   float vignette_strength   offset 48
     *   float gamma               offset 52
     *   float black_floor         offset 56
     *   float ambient_light       offset 60
     *   float glass_tint          offset 64
     *   float hdr_gain            offset 68
     *   int   subpixel_layout     offset 72
     *   float overscan            offset 76
     * ----------------------------------------------------------------------- */
    {
        /* Pack the CRT uniform data matching the shader's UBO layout.
         * Use a struct that mirrors the std140 layout exactly. */
        struct {
            float src_w, src_h;         /* vec2 src_size    (offset 0)  */
            float out_w, out_h;         /* vec2 out_size    (offset 8)  */
            float barrel;               /* offset 16 */
            float barrel_v;             /* offset 20 */
            float convergence_static;   /* offset 24 */
            float convergence_dynamic;  /* offset 28 */
            float mask_strength;        /* offset 32 */
            int   mask_type;            /* offset 36 */
            float mask_pitch_pixels;    /* offset 40 */
            float halation_strength;    /* offset 44 */
            float vignette_strength;    /* offset 48 */
            float gamma;                /* offset 52 */
            float black_floor;          /* offset 56 */
            float ambient_light;        /* offset 60 */
            float glass_tint;           /* offset 64 */
            float hdr_gain;             /* offset 68 */
            int   subpixel_layout;      /* offset 72 */
            float overscan;            /* offset 76 */
            float keystone;            /* offset 80 */
            float rotation;            /* offset 84 */
            float skew_x;              /* offset 88 */
            float skew_y;              /* offset 92 */
            float hv_sag;              /* offset 96 */
            float frame_brightness;    /* offset 100 */
            float h_pos;               /* offset 104 */
            float v_pos;               /* offset 108 */
            float h_size;              /* offset 112 */
            float v_size;              /* offset 116 */
            float halation_tint_r;     /* offset 120 */
            float halation_tint_g;     /* offset 124 */
            float halation_tint_b;     /* offset 128 */
            float phosphor_gamma_offset_r;  /* offset 132 */
            float phosphor_gamma_offset_g;  /* offset 136 */
            float phosphor_gamma_offset_b;  /* offset 140 */
            float secondary_scatter;        /* offset 144 */
            float glass_reflection;         /* offset 148 */
            float antiglare_blur;           /* offset 152 */
            float emi_gradient;             /* offset 156 */
            float degauss_tint;             /* offset 160 */
            float phosphor_grain;           /* offset 164 */
            float cathode_center_dim;       /* offset 168 */
            float cathode_gain_r;           /* offset 172 */
            float cathode_gain_g;           /* offset 176 */
            float cathode_gain_b;           /* offset 180 */
            float apl_black_lift;           /* offset 184 */
            float apl_smoothed;             /* offset 188 */
            float thermal_dome_amount;      /* offset 192 */
            float thermal_r;                /* offset 196 */
            float thermal_g;                /* offset 200 */
            float thermal_b;                /* offset 204 */
            float chromaticity_drive_shift; /* offset 208 */
            float microphonic_amount;       /* offset 212 */
            float audio_bass_rms;           /* offset 216 */
            float frame_phase;              /* offset 220 */
            float glass_glare;              /* offset 224 */
            float glass_glare_light_x;      /* offset 228 */
            float glass_glare_light_y;      /* offset 232 */
            float glass_glare_size;         /* offset 236 */
            float glass_glare_temp_k;
            float input_gamma, hdr_headroom, sdr_white_level;
            int output_hdr;       /* offset 240 */
        } crt_ubo;

        crt_ubo.src_w = (float)comp_w;
        crt_ubo.src_h = (float)comp_h;
        crt_ubo.out_w = viewport ? viewport->w : (float)sw;
        crt_ubo.out_h = viewport ? viewport->h : (float)sh;
        crt_ubo.barrel = params->barrel;
        crt_ubo.barrel_v = params->barrel_v;
        crt_ubo.convergence_static = params->convergence_static;
        crt_ubo.convergence_dynamic = params->convergence_dynamic;
        crt_ubo.mask_strength = params->mask_strength;
        crt_ubo.mask_type = params->mask_type;
        crt_ubo.input_gamma = params->input_gamma;
        crt_ubo.hdr_headroom = params->hdr_headroom;
        crt_ubo.sdr_white_level = params->sdr_white_level;
        crt_ubo.output_hdr = params->output_hdr;
        crt_ubo.mask_pitch_pixels = params->mask_pitch_px;
        crt_ubo.halation_strength = params->halation_strength;
        crt_ubo.vignette_strength = params->vignette;
        crt_ubo.gamma = params->gamma;
        crt_ubo.black_floor = params->black_floor;
        crt_ubo.ambient_light = params->ambient_light;
        crt_ubo.glass_tint = params->glass_tint;
        crt_ubo.hdr_gain = params->hdr_gain;
        crt_ubo.subpixel_layout = params->subpixel_layout;
        crt_ubo.overscan = params->overscan;
        crt_ubo.keystone = params->keystone;
        crt_ubo.rotation = params->rotation;
        crt_ubo.skew_x = params->skew_x;
        crt_ubo.skew_y = params->skew_y;
        crt_ubo.hv_sag = params->hv_sag;
        crt_ubo.frame_brightness = params->frame_brightness;
        crt_ubo.h_pos = params->h_pos;
        crt_ubo.v_pos = params->v_pos;
        crt_ubo.h_size = params->h_size > 0.01f ? params->h_size : 1.0f;
        crt_ubo.v_size = params->v_size > 0.01f ? params->v_size : 1.0f;
        crt_ubo.halation_tint_r = params->halation_tint_r;
        crt_ubo.halation_tint_g = params->halation_tint_g;
        crt_ubo.halation_tint_b = params->halation_tint_b;
        crt_ubo.phosphor_gamma_offset_r = params->phosphor_gamma_offset_r;
        crt_ubo.phosphor_gamma_offset_g = params->phosphor_gamma_offset_g;
        crt_ubo.phosphor_gamma_offset_b = params->phosphor_gamma_offset_b;
        crt_ubo.secondary_scatter = params->secondary_scatter;
        crt_ubo.glass_reflection  = params->glass_reflection;
        crt_ubo.antiglare_blur    = params->antiglare_blur;
        crt_ubo.emi_gradient      = params->emi_gradient;
        crt_ubo.degauss_tint      = params->degauss_tint;
        crt_ubo.phosphor_grain    = params->phosphor_grain;
        crt_ubo.cathode_center_dim = params->cathode_center_dim;
        crt_ubo.cathode_gain_r    = params->cathode_gain_r > 0 ? params->cathode_gain_r : 1.0f;
        crt_ubo.cathode_gain_g    = params->cathode_gain_g > 0 ? params->cathode_gain_g : 1.0f;
        crt_ubo.cathode_gain_b    = params->cathode_gain_b > 0 ? params->cathode_gain_b : 1.0f;
        crt_ubo.apl_black_lift    = params->apl_black_lift;
        crt_ubo.apl_smoothed      = params->apl_smoothed;
        crt_ubo.thermal_dome_amount = params->thermal_dome_amount;
        crt_ubo.thermal_r         = params->thermal_r;
        crt_ubo.thermal_g         = params->thermal_g;
        crt_ubo.thermal_b         = params->thermal_b;
        crt_ubo.chromaticity_drive_shift = params->chromaticity_drive_shift;
        crt_ubo.microphonic_amount = params->microphonic_amount;
        crt_ubo.audio_bass_rms     = params->audio_bass_rms;
        crt_ubo.frame_phase        = params->frame_phase;
        crt_ubo.glass_glare        = params->glass_glare;
        crt_ubo.glass_glare_light_x = params->glass_glare_light_x;
        crt_ubo.glass_glare_light_y = params->glass_glare_light_y;
        crt_ubo.glass_glare_size   = params->glass_glare_size;
        crt_ubo.glass_glare_temp_k = params->glass_glare_temp_k;

        SDL_GPUColorTargetInfo ct;
        memset(&ct, 0, sizeof(ct));
        ct.texture = swapchain_tex;
        ct.load_op = SDL_GPU_LOADOP_CLEAR;
        ct.store_op = SDL_GPU_STOREOP_STORE;
        /* Continue the unlit glass level into letterbox/pillarbox margins.
         * Match the final shader's ambient/APL lift and output transfer so
         * preset changes and HDR white-level changes cannot expose black bars.
         * Beam emission, mask and spatial reflections remain inside the CRT. */
        float surround = params->ambient_light * 0.15f;
        if (params->apl_black_lift > 0.001f)
            surround += params->apl_black_lift * (params->apl_smoothed - 0.5f) * 0.15f;
        surround *= params->hdr_gain > 0.0f ? params->hdr_gain : 1.0f;
        surround = fminf(fmaxf(surround, 0.0f), fmaxf(params->hdr_headroom, 1.0f));
        if (params->output_hdr)
            surround *= params->sdr_white_level;
        else
            surround = surround <= 0.0031308f ? 12.92f * surround :
                       1.055f * powf(surround, 1.0f / 2.4f) - 0.055f;
        ct.clear_color.r = surround;
        ct.clear_color.g = surround;
        ct.clear_color.b = surround;
        ct.clear_color.a = 1.0f;

        SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, NULL);
        if (pass) {
            SDL_BindGPUGraphicsPipeline(pass, d->pipe_crt);

            /* Set viewport for 4:3 aspect ratio (letterbox/pillarbox). */
            if (viewport) {
                SDL_SetGPUViewport(pass, viewport);
            }

            /* Bind both samplers: slot 0 = composite, slot 1 = halation. */
            SDL_GPUTextureSamplerBinding sampler_binds[2];
            sampler_binds[0].texture = composite_tex;
            sampler_binds[0].sampler = d->sampler_linear;
            sampler_binds[1].texture = d->tex_halation_b;
            sampler_binds[1].sampler = d->sampler_linear;
            SDL_BindGPUFragmentSamplers(pass, 0, sampler_binds, 2);

            SDL_PushGPUFragmentUniformData(cmd, 0, &crt_ubo, sizeof(crt_ubo));

            SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
    }
}

void gpu_display_params_from_tv(GPUDisplayParams *out, const TVDisplayParams *tv,
                                 int comp_w, int comp_h, int win_w, int win_h)
{
    if (!out || !tv) return;

    out->src_w = (float)comp_w;
    out->src_h = (float)comp_h;
    out->out_w = (float)win_w;
    out->out_h = (float)win_h;

    out->barrel = tv->barrel;
    out->barrel_v = tv->barrel_v;
    out->convergence_static = tv->convergence_static;
    out->convergence_dynamic = tv->convergence_dynamic;
    out->mask_strength = fminf(fmaxf(tv->mask_strength, 0.0f), 1.0f);
    out->mask_type = (int)tv->mask_type;

    /* Phosphor cell spacing in drawable pixels; legacy JSON used a misleading mm key. */
    out->mask_pitch_px = tv->mask_pitch_px;
    if (tv->mask_triads > 0.0f)
        out->mask_pitch_px = (float)win_w / (3.0f * tv->mask_triads);
    out->mask_pitch_px = fmaxf(out->mask_pitch_px, 0.05f);

    out->halation_strength = tv->halation;
    out->halation_tint_r   = tv->halation_tint_r;
    out->halation_tint_g   = tv->halation_tint_g;
    out->halation_tint_b   = tv->halation_tint_b;
    out->glass_tint = tv->glass_tint;
    out->vignette = tv->vignette;
    out->gamma = tv->gamma;

    out->black_floor = tv->black_floor;
    out->ambient_light = tv->ambient_light;
    out->hdr_gain = tv->hdr_gain;
    out->input_gamma = tv->gamma;
    out->hdr_headroom = out->sdr_white_level = 1;
    out->output_hdr = 0;
    out->subpixel_layout = tv->subpixel_layout;
    out->overscan = tv->overscan;
    out->keystone = tv->keystone;
    out->rotation = tv->rotation;
    out->skew_x = tv->skew_x;
    out->skew_y = tv->skew_y;
    out->hv_sag = tv->hv_sag;
    out->h_pos  = tv->h_pos;
    out->v_pos  = tv->v_pos;
    out->h_size = tv->h_size > 0.01f ? tv->h_size : 1.0f;
    out->v_size = tv->v_size > 0.01f ? tv->v_size : 1.0f;
    /* frame_brightness is set per-frame by the caller (gpu_render.c)
     * from the PPU framebuffer average luma. */

    out->phosphor_gamma_offset_r = tv->phosphor_gamma_offset_r;
    out->phosphor_gamma_offset_g = tv->phosphor_gamma_offset_g;
    out->phosphor_gamma_offset_b = tv->phosphor_gamma_offset_b;
    out->secondary_scatter = tv->secondary_scatter;
    out->glass_reflection  = tv->glass_reflection;
    out->antiglare_blur    = tv->antiglare_blur;
    out->emi_gradient      = tv->emi_gradient;
    out->degauss_tint      = tv->degauss_tint;
    out->phosphor_grain    = tv->phosphor_grain;
    out->cathode_center_dim = tv->cathode_center_dim;
    /* Zero-sentinel → 1.0 (no gain change), preserves preset
     * back-compat where the fields don't exist. */
    out->cathode_gain_r    = tv->cathode_gain_r > 0 ? tv->cathode_gain_r : 1.0f;
    out->cathode_gain_g    = tv->cathode_gain_g > 0 ? tv->cathode_gain_g : 1.0f;
    out->cathode_gain_b    = tv->cathode_gain_b > 0 ? tv->cathode_gain_b : 1.0f;

    out->apl_black_lift = tv->apl_black_lift;
    out->thermal_dome_amount = tv->thermal_dome_amount;
    out->chromaticity_drive_shift = tv->chromaticity_drive_shift;
    out->microphonic_amount = tv->microphonic_amount;
    /* apl_smoothed, thermal_r/g/b, audio_bass_rms, frame_phase
     * are set per-frame by gpu_render. */

    out->glass_glare         = tv->glass_glare;
    out->glass_glare_light_x = (tv->glass_glare_light_x > 0.0f
                                || tv->glass_glare_light_y > 0.0f)
                                ? tv->glass_glare_light_x : 0.3f;
    out->glass_glare_light_y = (tv->glass_glare_light_x > 0.0f
                                || tv->glass_glare_light_y > 0.0f)
                                ? tv->glass_glare_light_y : 0.25f;
    out->glass_glare_size    = (tv->glass_glare_size > 0.0f)
                                ? tv->glass_glare_size : 0.18f;
    out->glass_glare_temp_k  = tv->glass_glare_temp_k;
}
