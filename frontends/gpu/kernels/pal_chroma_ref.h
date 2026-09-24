/*
 * PAL Chroma Correction — CPU Reference
 * ======================================
 *
 * Reference implementation for pal_chroma.comp.glsl. Mirrors the PAL
 * decoder model used in src/nes/composite.h:
 *   - odd-line U sign correction
 *   - 1H delay-line averaging of V and of the parity-corrected U
 */

#ifndef PAL_CHROMA_REF_H
#define PAL_CHROMA_REF_H

static inline void pal_chroma_ref(const float *v_in,
                                  const float *u_in,
                                  float *v_out,
                                  float *u_out,
                                  int count,
                                  int samples_per_line) {
    for (int i = 0; i < count; i++) {
        float v = v_in[i];
        float u = u_in[i];

        if (samples_per_line > 0) {
            int line = i / samples_per_line;
            if (line & 1) u = -u;
            if (line > 0) {
                v = 0.5f * (v + v_in[i - samples_per_line]);
                float previous_u = u_in[i - samples_per_line];
                if ((line - 1) & 1) previous_u = -previous_u;
                u = 0.5f * (u + previous_u);
            }
        }

        v_out[i] = v;
        u_out[i] = u;
    }
}

#endif /* PAL_CHROMA_REF_H */
