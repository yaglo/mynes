/*
 * Pointwise Transfer Function — GPU Compute Shader
 * ==================================================
 *
 * y[n] = f(x[n])   — each sample is independent, one thread per sample.
 *
 * The transfer function is selected by the `mode` uniform:
 *   0: gain + offset          y = gain * x + offset
 *   1: tanh soft-clip          y = tanh(x * drive) / tanh(drive)
 *   2: hard clip               y = clamp(x, lo, hi)
 *   3: power curve (gamma)     y = sign(x) * pow(abs(x), exponent)
 *   4: cubic DAC nonlinearity  y = x + k * (x³ - x)
 *   5: additive sinusoidal     y = x + amplitude * sin(phase + n * dp)
 *
 * All functions operate on a 1D float buffer read from binding 0
 * and written to binding 1. The same buffer can be used for both
 * (in-place) if the dispatch is configured correctly.
 */

#version 450

layout(local_size_x = 256) in;

layout(set = 1, binding = 0) buffer InputBuf  { float data_in[];  };
layout(set = 1, binding = 1) buffer OutputBuf { float data_out[]; };

layout(set = 2, binding = 0) uniform Params {
    uint  count;        /* number of samples to process */
    uint  mode;         /* transfer function selector */
    float param_a;      /* mode-dependent: gain, drive, lo, exponent, k, amplitude */
    float param_b;      /* mode-dependent: offset, -, hi, -, -, phase */
    float param_c;      /* mode-dependent: -, -, -, -, -, dp (phase increment) */
};

void main() {
    uint tid = gl_GlobalInvocationID.x;
    if (tid >= count) return;

    float x = data_in[tid];
    float y;

    switch (mode) {
        case 0u: /* gain + offset */
            y = param_a * x + param_b;
            break;

        case 1u: /* tanh soft-clip */
        {
            float drive = param_a;
            float t = tanh(x * drive);
            float norm = (drive > 0.0) ? tanh(drive) : 1.0;
            y = t / norm;
            break;
        }

        case 2u: /* hard clip */
            y = clamp(x, param_a, param_b);
            break;

        case 3u: /* power curve (gamma) */
        {
            float exponent = param_a;
            y = (x >= 0.0) ? pow(x, exponent) : -pow(-x, exponent);
            break;
        }

        case 4u: /* cubic DAC nonlinearity */
        {
            float k = param_a;
            y = x + k * (x * x * x - x);
            break;
        }

        case 5u: /* additive sinusoidal (PSU hum) */
        {
            float amplitude = param_a;
            float phase = param_b + float(tid) * param_c;
            y = x + amplitude * sin(phase);
            break;
        }

        default:
            y = x;
            break;
    }

    data_out[tid] = y;
}
