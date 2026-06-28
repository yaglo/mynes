/*
 * GPU Compute Dispatch Infrastructure — Implementation
 * ======================================================
 *
 * See gpu_compute.h for API documentation and design rationale.
 */

#include "gpu_compute.h"
#include "gpu_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Pipeline creation / destruction
 * ============================================================================ */

bool gpu_pipeline_create(
    SDL_GPUDevice *device,
    GpuPipeline *pipeline,
    const char *spv_path,
    const char *entrypoint,
    Uint32 threadcount_x,
    Uint32 threadcount_y,
    Uint32 threadcount_z,
    const GpuPipelineResources *resources,
    const char *name)
{
    memset(pipeline, 0, sizeof(*pipeline));

    /* Load SPIR-V bytecode from disk. */
    size_t code_size = 0;
    void *code = SDL_LoadFile(spv_path, &code_size);
    if (!code) {
        fprintf(stderr, "gpu_pipeline_create(%s): failed to load %s: %s\n",
                name ? name : "?", spv_path, SDL_GetError());
        return false;
    }

    /* Verify minimum SPIR-V size and magic number. */
    if (code_size < 20) {
        fprintf(stderr, "gpu_pipeline_create(%s): %s is too small (%zu bytes)\n",
                name ? name : "?", spv_path, code_size);
        SDL_free(code);
        return false;
    }
    const uint32_t spirv_magic = 0x07230203;
    uint32_t file_magic;
    memcpy(&file_magic, code, sizeof(uint32_t));
    if (file_magic != spirv_magic) {
        fprintf(stderr, "gpu_pipeline_create(%s): %s does not have SPIR-V magic "
                "(got 0x%08X, expected 0x%08X)\n",
                name ? name : "?", spv_path, file_magic, spirv_magic);
        SDL_free(code);
        return false;
    }

    /* Check what shader format the device supports. If SPIR-V is not
     * natively supported (e.g., Metal backend), try loading a pre-compiled
     * MSL file at the same path with .msl extension (produced by
     * spirv-cross during the build). */
    SDL_GPUShaderFormat device_formats = SDL_GetGPUShaderFormats(device);
    SDL_GPUShaderFormat use_format = SDL_GPU_SHADERFORMAT_SPIRV;

    if (!(device_formats & SDL_GPU_SHADERFORMAT_SPIRV)) {
        /* SPIR-V not supported. Try MSL (Metal Shading Language). */
        SDL_free(code);
        code = NULL;

        if (device_formats & SDL_GPU_SHADERFORMAT_MSL) {
            /* Build MSL path: replace .spv with .msl */
            size_t path_len = strlen(spv_path);
            char *msl_path = (char *)malloc(path_len + 1);
            if (msl_path) {
                memcpy(msl_path, spv_path, path_len + 1);
                /* Replace last 3 chars (.spv → .msl) */
                if (path_len >= 4) {
                    msl_path[path_len - 3] = 'm';
                    msl_path[path_len - 2] = 's';
                    msl_path[path_len - 1] = 'l';
                }
                code = SDL_LoadFile(msl_path, &code_size);
                if (code) {
                    use_format = SDL_GPU_SHADERFORMAT_MSL;
                    LOGV("gpu_pipeline_create(%s): using MSL shader %s\n",
                         name ? name : "?", msl_path);
                } else {
                    fprintf(stderr, "gpu_pipeline_create(%s): MSL shader not found: %s\n",
                            name ? name : "?", msl_path);
                }
                free(msl_path);
            }
        }

        if (!code) {
            fprintf(stderr, "gpu_pipeline_create(%s): no compatible shader format. "
                    "Backend: %s. Supported: 0x%X. Need SPIR-V or MSL.\n",
                    name ? name : "?",
                    SDL_GetGPUDeviceDriver(device),
                    (unsigned)device_formats);
            return false;
        }
    }

    /* Fill in the compute pipeline create info. */
    SDL_GPUComputePipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.code = (const Uint8 *)code;
    ci.code_size = code_size;
    ci.entrypoint = (use_format == SDL_GPU_SHADERFORMAT_MSL) ? "main0" : entrypoint;
    ci.format = use_format;
    ci.format = use_format;

    ci.num_samplers                   = resources->num_samplers;
    ci.num_readonly_storage_textures  = resources->num_readonly_storage_textures;
    ci.num_readonly_storage_buffers   = resources->num_readonly_storage_buffers;
    ci.num_readwrite_storage_textures = resources->num_readwrite_storage_textures;
    ci.num_readwrite_storage_buffers  = resources->num_readwrite_storage_buffers;
    ci.num_uniform_buffers            = resources->num_uniform_buffers;

    ci.threadcount_x = threadcount_x;
    ci.threadcount_y = threadcount_y;
    ci.threadcount_z = threadcount_z;

    ci.props = 0;

    pipeline->pipeline = SDL_CreateGPUComputePipeline(device, &ci);
    SDL_free(code);

    if (!pipeline->pipeline) {
        fprintf(stderr, "gpu_pipeline_create(%s): SDL_CreateGPUComputePipeline "
                "failed: %s\n", name ? name : "?", SDL_GetError());
        return false;
    }

    pipeline->threadcount_x = threadcount_x;
    pipeline->threadcount_y = threadcount_y;
    pipeline->threadcount_z = threadcount_z;

    /* Copy the debug name. */
    if (name) {
        size_t len = strlen(name);
        char *copy = (char *)malloc(len + 1);
        if (copy) {
            memcpy(copy, name, len + 1);
        }
        pipeline->name = copy;
    }

    LOGV("gpu_pipeline_create: loaded %s (%zu bytes, workgroup %u/%u/%u)\n",
            name ? name : spv_path, code_size,
            threadcount_x, threadcount_y, threadcount_z);

    return true;
}

void gpu_pipeline_destroy(SDL_GPUDevice *device, GpuPipeline *pipeline)
{
    if (!pipeline) return;
    if (pipeline->pipeline) {
        SDL_ReleaseGPUComputePipeline(device, pipeline->pipeline);
        pipeline->pipeline = NULL;
    }
    if (pipeline->name) {
        free((void *)pipeline->name);
        pipeline->name = NULL;
    }
}

/* ============================================================================
 * Buffer management
 * ============================================================================ */

SDL_GPUBuffer *gpu_buffer_create(
    SDL_GPUDevice *device,
    Uint32 size_bytes,
    GpuBufferUsage usage)
{
    SDL_GPUBufferCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.size = size_bytes;
    ci.props = 0;

    switch (usage) {
        case GPU_BUF_READONLY:
            ci.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ;
            break;
        case GPU_BUF_WRITEONLY:
            ci.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
            break;
        case GPU_BUF_READWRITE:
            ci.usage = SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_READ
                     | SDL_GPU_BUFFERUSAGE_COMPUTE_STORAGE_WRITE;
            break;
    }

    SDL_GPUBuffer *buf = SDL_CreateGPUBuffer(device, &ci);
    if (!buf) {
        fprintf(stderr, "gpu_buffer_create(%u bytes): %s\n",
                size_bytes, SDL_GetError());
    }
    return buf;
}

bool gpu_buffer_upload(
    SDL_GPUDevice *device,
    SDL_GPUBuffer *buffer,
    const void *data,
    Uint32 size_bytes)
{
    /* Create a transfer buffer for the upload. */
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof(tbci));
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbci.size = size_bytes;
    tbci.props = 0;

    SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(device, &tbci);
    if (!tbuf) {
        fprintf(stderr, "gpu_buffer_upload: transfer buffer creation failed: %s\n",
                SDL_GetError());
        return false;
    }

    /* Map, copy, unmap. */
    void *mapped = SDL_MapGPUTransferBuffer(device, tbuf, false);
    if (!mapped) {
        fprintf(stderr, "gpu_buffer_upload: map failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, tbuf);
        return false;
    }
    memcpy(mapped, data, size_bytes);
    SDL_UnmapGPUTransferBuffer(device, tbuf);

    /* Submit a copy pass to transfer data to the GPU buffer. */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    if (!cmd) {
        fprintf(stderr, "gpu_buffer_upload: acquire cmd failed: %s\n",
                SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, tbuf);
        return false;
    }

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        fprintf(stderr, "gpu_buffer_upload: begin copy pass failed: %s\n",
                SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device, tbuf);
        return false;
    }

    SDL_GPUTransferBufferLocation src;
    memset(&src, 0, sizeof(src));
    src.transfer_buffer = tbuf;
    src.offset = 0;

    SDL_GPUBufferRegion dst;
    memset(&dst, 0, sizeof(dst));
    dst.buffer = buffer;
    dst.offset = 0;
    dst.size = size_bytes;

    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);

    SDL_ReleaseGPUTransferBuffer(device, tbuf);
    return true;
}

bool gpu_buffer_download(
    SDL_GPUDevice *device,
    SDL_GPUBuffer *buffer,
    void *out_data,
    Uint32 size_bytes)
{
    /* Create a download transfer buffer. */
    SDL_GPUTransferBufferCreateInfo tbci;
    memset(&tbci, 0, sizeof(tbci));
    tbci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbci.size = size_bytes;
    tbci.props = 0;

    SDL_GPUTransferBuffer *tbuf = SDL_CreateGPUTransferBuffer(device, &tbci);
    if (!tbuf) {
        fprintf(stderr, "gpu_buffer_download: transfer buffer creation failed: %s\n",
                SDL_GetError());
        return false;
    }

    /* Submit a copy pass to download from GPU buffer to transfer buffer. */
    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(device);
    if (!cmd) {
        fprintf(stderr, "gpu_buffer_download: acquire cmd failed: %s\n",
                SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, tbuf);
        return false;
    }

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (!copy) {
        fprintf(stderr, "gpu_buffer_download: begin copy pass failed: %s\n",
                SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd);
        SDL_ReleaseGPUTransferBuffer(device, tbuf);
        return false;
    }

    SDL_GPUBufferRegion src;
    memset(&src, 0, sizeof(src));
    src.buffer = buffer;
    src.offset = 0;
    src.size = size_bytes;

    SDL_GPUTransferBufferLocation dst;
    memset(&dst, 0, sizeof(dst));
    dst.transfer_buffer = tbuf;
    dst.offset = 0;

    SDL_DownloadFromGPUBuffer(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);

    /* Submit with a fence so we can wait for the GPU to finish the copy
     * before reading back the data. */
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        fprintf(stderr, "gpu_buffer_download: submit+fence failed: %s\n",
                SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, tbuf);
        return false;
    }

    /* Block until the download is complete. */
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);

    /* Map the transfer buffer (now populated) and copy to output. */
    void *mapped = SDL_MapGPUTransferBuffer(device, tbuf, false);
    if (!mapped) {
        fprintf(stderr, "gpu_buffer_download: map failed: %s\n",
                SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(device, tbuf);
        return false;
    }
    memcpy(out_data, mapped, size_bytes);
    SDL_UnmapGPUTransferBuffer(device, tbuf);

    SDL_ReleaseGPUTransferBuffer(device, tbuf);
    return true;
}

/* ============================================================================
 * Compute dispatch
 * ============================================================================ */

bool gpu_dispatch(SDL_GPUCommandBuffer *cmd, const GpuDispatchDesc *desc)
{
    if (!cmd || !desc || !desc->pipeline || !desc->pipeline->pipeline) {
        fprintf(stderr, "gpu_dispatch: invalid arguments\n");
        return false;
    }

    /* Build the read-write storage buffer bindings for BeginGPUComputePass.
     * These are the output buffers that the shader writes to. */
    SDL_GPUStorageBufferReadWriteBinding rw_bindings[GPU_MAX_READWRITE_BUFFERS];
    for (Uint32 i = 0; i < desc->num_readwrite_buffers; i++) {
        memset(&rw_bindings[i], 0, sizeof(rw_bindings[i]));
        rw_bindings[i].buffer = desc->readwrite_buffers[i];
        rw_bindings[i].cycle = false;
    }

    /* Begin compute pass. Read-write buffers are bound here as outputs. */
    SDL_GPUComputePass *pass = SDL_BeginGPUComputePass(
        cmd,
        NULL,                          /* no storage texture bindings */
        0,
        desc->num_readwrite_buffers > 0 ? rw_bindings : NULL,
        desc->num_readwrite_buffers);

    if (!pass) {
        fprintf(stderr, "gpu_dispatch(%s): BeginGPUComputePass failed: %s\n",
                desc->pipeline->name ? desc->pipeline->name : "?",
                SDL_GetError());
        return false;
    }

    /* Bind the compute pipeline. */
    SDL_BindGPUComputePipeline(pass, desc->pipeline->pipeline);

    /* Bind read-only storage buffers (inputs). */
    if (desc->num_readonly_buffers > 0) {
        SDL_BindGPUComputeStorageBuffers(
            pass,
            0,  /* first_slot */
            desc->readonly_buffers,
            desc->num_readonly_buffers);
    }

    /* Push uniform data. */
    for (Uint32 i = 0; i < desc->num_uniforms; i++) {
        if (desc->uniforms[i].data && desc->uniforms[i].size > 0) {
            SDL_PushGPUComputeUniformData(
                cmd,
                i,  /* slot_index */
                desc->uniforms[i].data,
                desc->uniforms[i].size);
        }
    }

    /* Dispatch workgroups. */
    SDL_DispatchGPUCompute(
        pass,
        desc->groupcount_x,
        desc->groupcount_y,
        desc->groupcount_z);

    /* End the compute pass. This acts as a barrier for the written buffers. */
    SDL_EndGPUComputePass(pass);

    return true;
}

/* ============================================================================
 * Convenience dispatchers
 * ============================================================================ */

/*
 * Shader binding summary (from shaders/compute/*.comp.glsl):
 *
 * pointwise.comp.glsl:
 *   set=0, binding=0: InputBuf  (buffer, read)
 *   set=0, binding=1: OutputBuf (buffer, write)
 *   set=1, binding=0: Params    (uniform)
 *   -> 0 readonly, 2 readwrite, 1 uniform
 *
 *   NOTE: The shader uses `buffer` (no readonly/writeonly) for both input
 *   and output at set=0. SDL3's SPIR-V mapping puts all set=0 buffers
 *   without explicit readonly into the readonly category based on the
 *   pipeline's resource counts. Since both are in set=0, and the shader
 *   writes to binding=1, we declare them as readwrite storage buffers
 *   (set=1 in SDL3's model). The SDL3 SPIR-V translator handles the
 *   mapping from the shader's actual descriptor set layout.
 *
 *   In practice for SDL3: the buffers at set=0 are treated based on the
 *   num_readonly_storage_buffers and num_readwrite_storage_buffers counts.
 *   Since pointwise writes to one of them, both are readwrite.
 *
 * rc_filter.comp.glsl:
 *   set=0, binding=0: DataBuf  (buffer, read+write in-place)
 *   set=0, binding=1: CarryBuf (buffer, read+write)
 *   set=1, binding=0: Params   (uniform)
 *   -> 0 readonly, 2 readwrite, 1 uniform
 *
 * fir.comp.glsl:
 *   set=0, binding=0: InputBuf  (readonly buffer)
 *   set=0, binding=1: OutputBuf (writeonly buffer)
 *   set=0, binding=2: TapsBuf   (readonly buffer)
 *   set=1, binding=0: Params    (uniform)
 *   -> 2 readonly, 1 readwrite, 1 uniform
 *
 * delay.comp.glsl:
 *   set=0, binding=0: InputBuf  (readonly buffer)
 *   set=0, binding=1: OutputBuf (writeonly buffer)
 *   set=0, binding=2: PrevBuf   (readonly buffer)
 *   set=1, binding=0: Params    (uniform)
 *   -> 2 readonly, 1 readwrite, 1 uniform
 *
 * modulator.comp.glsl:
 *   set=0, binding=0: InputBuf   (readonly buffer)
 *   set=0, binding=1: OutputBuf  (writeonly buffer)
 *   set=0, binding=2: OutputBuf2 (writeonly buffer)
 *   set=1, binding=0: Params     (uniform)
 *   -> 1 readonly, 2 readwrite, 1 uniform
 */

bool gpu_dispatch_pointwise(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    const GpuPointwiseParams *params)
{
    GpuDispatchDesc desc;
    memset(&desc, 0, sizeof(desc));

    desc.pipeline = pipeline;

    /* Both buffers are in set 0 and the shader writes to one of them,
     * so they are both bound as read-write at pass begin. */
    desc.readwrite_buffers[0] = buf_in;
    desc.readwrite_buffers[1] = buf_out;
    desc.num_readwrite_buffers = 2;

    desc.uniforms[0].data = params;
    desc.uniforms[0].size = sizeof(*params);
    desc.num_uniforms = 1;

    desc.groupcount_x = gpu_workgroup_count(params->count,
                                             pipeline->threadcount_x);
    desc.groupcount_y = 1;
    desc.groupcount_z = 1;

    return gpu_dispatch(cmd, &desc);
}

bool gpu_dispatch_rc_filter(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_data,
    SDL_GPUBuffer *buf_carry,
    const GpuRCFilterParams *params,
    Uint32 num_workgroups)
{
    GpuDispatchDesc desc;
    memset(&desc, 0, sizeof(desc));

    desc.pipeline = pipeline;

    /* Both data and carry buffers are read-write (in-place filter). */
    desc.readwrite_buffers[0] = buf_data;
    desc.readwrite_buffers[1] = buf_carry;
    desc.num_readwrite_buffers = 2;

    desc.uniforms[0].data = params;
    desc.uniforms[0].size = sizeof(*params);
    desc.num_uniforms = 1;

    desc.groupcount_x = num_workgroups;
    desc.groupcount_y = 1;
    desc.groupcount_z = 1;

    return gpu_dispatch(cmd, &desc);
}

bool gpu_dispatch_fir(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    SDL_GPUBuffer *buf_taps,
    const GpuFIRParams *params)
{
    GpuDispatchDesc desc;
    memset(&desc, 0, sizeof(desc));

    desc.pipeline = pipeline;

    /* Input and taps are readonly (set 0 in SPIR-V). */
    desc.readonly_buffers[0] = buf_in;
    desc.readonly_buffers[1] = buf_taps;
    desc.num_readonly_buffers = 2;

    /* Output is read-write (set 1 in SPIR-V). */
    desc.readwrite_buffers[0] = buf_out;
    desc.num_readwrite_buffers = 1;

    desc.uniforms[0].data = params;
    desc.uniforms[0].size = sizeof(*params);
    desc.num_uniforms = 1;

    desc.groupcount_x = gpu_workgroup_count(params->output_count,
                                             pipeline->threadcount_x);
    desc.groupcount_y = 1;
    desc.groupcount_z = 1;

    return gpu_dispatch(cmd, &desc);
}

bool gpu_dispatch_delay(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    SDL_GPUBuffer *buf_prev,
    const GpuDelayParams *params)
{
    GpuDispatchDesc desc;
    memset(&desc, 0, sizeof(desc));

    desc.pipeline = pipeline;

    /* Input and prev are readonly (set 0 in SPIR-V). */
    desc.readonly_buffers[0] = buf_in;
    desc.readonly_buffers[1] = buf_prev;
    desc.num_readonly_buffers = 2;

    /* Output is read-write (set 1 in SPIR-V). */
    desc.readwrite_buffers[0] = buf_out;
    desc.num_readwrite_buffers = 1;

    desc.uniforms[0].data = params;
    desc.uniforms[0].size = sizeof(*params);
    desc.num_uniforms = 1;

    desc.groupcount_x = gpu_workgroup_count(params->count,
                                             pipeline->threadcount_x);
    desc.groupcount_y = 1;
    desc.groupcount_z = 1;

    return gpu_dispatch(cmd, &desc);
}

bool gpu_dispatch_modulator(
    SDL_GPUCommandBuffer *cmd,
    const GpuPipeline *pipeline,
    SDL_GPUBuffer *buf_in,
    SDL_GPUBuffer *buf_out,
    SDL_GPUBuffer *buf_out2,
    const GpuModulatorParams *params)
{
    GpuDispatchDesc desc;
    memset(&desc, 0, sizeof(desc));

    desc.pipeline = pipeline;

    /* Input is readonly (set 0 in SPIR-V). */
    desc.readonly_buffers[0] = buf_in;
    desc.num_readonly_buffers = 1;

    /* Both outputs are read-write (set 1 in SPIR-V). */
    desc.readwrite_buffers[0] = buf_out;
    desc.readwrite_buffers[1] = buf_out2;
    desc.num_readwrite_buffers = 2;

    desc.uniforms[0].data = params;
    desc.uniforms[0].size = sizeof(*params);
    desc.num_uniforms = 1;

    desc.groupcount_x = gpu_workgroup_count(params->count,
                                             pipeline->threadcount_x);
    desc.groupcount_y = 1;
    desc.groupcount_z = 1;

    return gpu_dispatch(cmd, &desc);
}

/* ============================================================================
 * Performance measurement
 * ============================================================================ */

void gpu_timing_reset(GpuTimingLog *log)
{
    memset(log, 0, sizeof(*log));
}

bool gpu_submit_and_time(
    SDL_GPUDevice *device,
    SDL_GPUCommandBuffer *cmd,
    const char *name,
    GpuTimingLog *log)
{
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 start = SDL_GetPerformanceCounter();

    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence) {
        fprintf(stderr, "gpu_submit_and_time(%s): submit+fence failed: %s\n",
                name ? name : "?", SDL_GetError());
        return false;
    }

    /* Block until the GPU finishes this command buffer. */
    SDL_WaitForGPUFences(device, true, &fence, 1);
    SDL_ReleaseGPUFence(device, fence);

    Uint64 end = SDL_GetPerformanceCounter();
    double elapsed_us = (double)(end - start) * 1000000.0 / (double)freq;

    if (log && log->count < GPU_MAX_TIMING_ENTRIES) {
        GpuTimingEntry *entry = &log->entries[log->count++];
        entry->name = name;
        entry->elapsed_us = elapsed_us;
        log->total_us += elapsed_us;
    }

    return true;
}

void gpu_timing_print(const GpuTimingLog *log)
{
    if (!log || log->count == 0) {
        fprintf(stderr, "gpu_timing: no entries recorded\n");
        return;
    }

    fprintf(stderr, "--- GPU timing (%d entries) ---\n", log->count);
    for (int i = 0; i < log->count; i++) {
        fprintf(stderr, "  %-30s  %8.1f us\n",
                log->entries[i].name ? log->entries[i].name : "(null)",
                log->entries[i].elapsed_us);
    }
    fprintf(stderr, "  %-30s  %8.1f us\n", "TOTAL", log->total_us);
    fprintf(stderr, "------------------------------\n");
}
