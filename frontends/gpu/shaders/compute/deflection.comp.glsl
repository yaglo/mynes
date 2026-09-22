/*
 * Deflection Map — GPU Compute Shader
 * ==================================
 *
 * Builds a coherent beam-landing map at the final beam-output
 * resolution. This stage models the analog raster field itself:
 *
 *   - horizontal PLL jitter and slow sway
 *   - top-of-frame flyback settle / ringing
 *   - S-correction residual, service geometry, and tube-face warp
 *   - per-channel convergence landing
 *   - edge dwell change from non-linear deflection velocity
 *   - focus growth / astigmatism toward the corners
 *   - HV breathing and audio-coupled microphonics
 *
 * The beam-profile stage consumes this landing map and only deposits
 * light; it no longer invents its own geometry or a second convergence
 * pass.
 */

#version 450

layout(local_size_x = 16, local_size_y = 16) in;
layout(set=0,binding=0) readonly buffer CRTLoad { float load_map[]; };

/* Output A: landed signal X for R/G/B + dwell factor.
 *   out_x[pixel*4 + 0] = r_x
 *   out_x[pixel*4 + 1] = g_x
 *   out_x[pixel*4 + 2] = b_x
 *   out_x[pixel*4 + 3] = dwell */
layout(set = 1, binding = 0) writeonly buffer DeflectionX {
    float out_x[];
};

/* Output B: landed output-row Y for R/G/B + sigma scale.
 *   out_y[pixel*4 + 0] = r_y
 *   out_y[pixel*4 + 1] = g_y
 *   out_y[pixel*4 + 2] = b_y
 *   out_y[pixel*4 + 3] = sigma_scale */
layout(set = 1, binding = 1) writeonly buffer DeflectionY {
    float out_y[];
};

layout(set = 2, binding = 0) uniform Params {
    uint  signal_w;
    uint  out_w;
    uint  out_h;
    uint  rows_per_scanline;
    uint  frame_counter;
    float h_jitter;
    float v_jitter;
    float rf_interference;
    float geometry_warp;
    float convergence_static;
    float convergence_dynamic;
    float conv_r_x;
    float conv_r_y;
    float conv_b_x;
    float conv_b_y;
    float edge_focus;
    float velocity_dim;
    float psu_hum;
    float focus_breathing;
    float scanline_wobble;
    float corner_astigmatism;
    float barrel;
    float barrel_v;
    float overscan;
    float keystone;
    float rotation;
    float skew_x;
    float skew_y;
    float hv_sag;
    float frame_brightness;
    float h_pos;
    float v_pos;
    float h_size;
    float v_size;
    float microphonic_amount;
    float audio_bass_rms;
    float top_band_shift;
    float top_edge_skew;
    float top_band_start;
    float top_band_end;
    float top_edge_width;
};

float hash01(uint x) {
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return float(x & 0x00FFFFFFu) / float(0x01000000u);
}

float hashSigned(uint x) {
    return hash01(x) * 2.0 - 1.0;
}

vec2 barrel_distort(vec2 coord, float k_h, float k_v) {
    vec2 centered = coord - 0.5;
    float r2 = dot(centered, centered);
    centered.x *= (1.0 + k_h * r2);
    centered.y *= (1.0 + k_v * r2);
    return centered + 0.5;
}

void main() {
    uint ox = gl_GlobalInvocationID.x;
    uint oy = gl_GlobalInvocationID.y;
    if (ox >= out_w || oy >= out_h) return;

    float fw = max(float(out_w), 1.0);
    float fh = max(float(out_h), 1.0);
    float x_uv = (float(ox) + 0.5) / fw;
    float y_uv = (float(oy) + 0.5) / fh;
    float frame_t = float(frame_counter) * 0.0173;
    float microphonic_phase = float(frame_counter)
                            * (80.0 * 6.283185 / 60.0);

    /* Tube-face geometry happens here, before the beam is deposited.
     * This keeps the beam footprint intact instead of warping an
     * already-rasterized texture in the display pass. */
    vec2 tube_uv = vec2(x_uv, y_uv);
    vec2 uv_wobble = tube_uv;
    if (microphonic_amount > 0.0001 && audio_bass_rms > 0.0001) {
        float amp = microphonic_amount * audio_bass_rms;
        uv_wobble.y += sin(microphonic_phase) * amp;
        uv_wobble.x += sin(microphonic_phase * 0.31) * amp * 0.4;
    }

    float kv = barrel_v > 0.001 ? barrel_v : barrel;
    vec2 warped = barrel_distort(uv_wobble, barrel, kv);

    uint load_line=uint(clamp(y_uv*240.0,0.0,239.0));
    uint load_dot=uint(clamp(x_uv*256.0,0.0,255.0));
    float picture_load=0.65*load_map[256u*240u+load_line]+0.35*load_map[load_line*256u+load_dot];
    // Inverse landing coordinates: positive sag contracts the picture;
    // negative models EHT-dominated expansion. Keep the historical preset sign.
    if (abs(hv_sag) > 0.0001) {
        float size_load = 1.0 + hv_sag * picture_load * 0.16;
        warped = (warped - 0.5) * size_load + 0.5;
    }

    if (abs(rotation) > 0.0001) {
        vec2 c = warped - 0.5;
        float cr = cos(rotation), sr = sin(rotation);
        warped = vec2(c.x * cr - c.y * sr, c.x * sr + c.y * cr) + 0.5;
    }

    if (abs(skew_x) > 0.0001 || abs(skew_y) > 0.0001) {
        vec2 c = warped - 0.5;
        warped = vec2(c.x + skew_x * c.y, c.y + skew_y * c.x) + 0.5;
    }

    if (abs(keystone) > 0.0001) {
        float y_norm = warped.y - 0.5;
        float x_scale = 1.0 + keystone * y_norm * 2.0;
        warped.x = (warped.x - 0.5) / max(x_scale, 0.2) + 0.5;
    }

    if (overscan > 0.001) {
        // Inverse mapping: zooming the raster samples a smaller source
        // interval. Dividing here incorrectly shrank the raster into borders.
        warped = (warped - 0.5) * max(1.0 - overscan * 2.0, 0.2) + 0.5;
    }

    vec2 raster = warped;
    raster.x = (warped.x - 0.5 - h_pos) / max(h_size, 0.01) + 0.5;
    raster.y = (warped.y - 0.5 - v_pos) / max(v_size, 0.01) + 0.5;

    // Source bounds and the finite beam spot define the raster edge. A second
    // UV-space fade would soften it by more display pixels as resolution grows.
    // The fixed face aperture clips the finished light in the display pass.

    float cx = raster.x * 2.0 - 1.0;
    float cy = raster.y * 2.0 - 1.0;
    int sy_i = clamp(int(floor(raster.y * 240.0)), 0, 239);
    uint sy_nom = uint(sy_i);
    float line_uv = (float(sy_nom) + 0.5) / 240.0;
    uint line_seed = sy_nom * 1103515245u + frame_counter * 12345u + 0x9e3779b9u;

    /* Horizontal timebase: combine slow sway, line-locked jitter,
     * PSU ripple, and top-of-frame flyback ringing. */
    float h_shift_px=0.0;
    if(h_jitter!=0.0) {
        float slow_sway=sin(frame_t*.63)*.7 + sin(frame_t*.21+1.7)*.3;
        float pll_jitter=hashSigned(line_seed)+.5*hashSigned(line_seed^0x85ebca6bu);
        float ripple=sin(float(sy_nom)*.19+frame_t*1.4);
        float flyback_ring=exp(-float(sy_nom)/18.0)*sin(float(sy_nom)*.92+frame_t*1.9);
        float group_shift=sin(floor(float(sy_nom)/4.0)*.83+frame_t*.7);
        h_shift_px=h_jitter*(.28*slow_sway+.22*pll_jitter+.18*ripple+.22*flyback_ring+.10*group_shift);
    }
    if (rf_interference > 0.001) {
        float rf_step = sin(float(sy_nom) * 0.47 + frame_t * 0.31);
        h_shift_px += floor(rf_step * rf_interference + 0.5);
    }

    /* Vertical settle + differential linearity. */
    float field_jump_px=0.0, top_settle_px=0.0;
    if(v_jitter!=0.0) {
        field_jump_px=v_jitter*.25*hashSigned(frame_counter*4099u+17u);
        top_settle_px=v_jitter*1.6*exp(-float(sy_nom)/14.0)*sin(float(sy_nom)*.78+frame_t*1.25);
    }
    float bottom_compress=geometry_warp*.016*(cy*cy*cy-.25*cy);
    float s_correction=geometry_warp*.034*(cx*cx*cx-.35*cx);
    float ew_residual=geometry_warp*.024*cx*cy*cy;
    float left_edge_nl=0.0,right_edge_nl=0.0,geometry_slope=0.0;
    if(geometry_warp!=0.0) {
        float left=exp(-max(raster.x,0.0)/.060),right=exp(-max(1.0-raster.x,0.0)/.050);
        left_edge_nl=geometry_warp*.012*left*(.35-raster.x);
        right_edge_nl=geometry_warp*.010*right*(raster.x-.65);
        geometry_slope=geometry_warp*(.102*cx*cx-.0119+.024*cy*cy)
                      +geometry_warp*(-.012*left-.010*right);
    }
    float line_bow_x=0.0,line_bow_y=0.0,bow_slope=0.0;
    if(scanline_wobble!=0.0) {
        float phase=x_uv*6.283185+float(sy_nom)*.63+frame_t*.9;
        line_bow_x=scanline_wobble*.015*sin(phase);
        line_bow_y=scanline_wobble*.020*sin(x_uv*6.283185+float(sy_nom)*.39+frame_t*.9);
        bow_slope=scanline_wobble*.0471239*cos(phase);
    }
    float band_lo = min(top_band_start, top_band_end);
    float band_hi = max(top_band_start, top_band_end);
    float band_soft = 2.5;
    float band_window = smoothstep(band_lo - band_soft, band_lo + band_soft, float(sy_nom))
                      * (1.0 - smoothstep(band_hi - band_soft, band_hi + band_soft,
                                          float(sy_nom)));
    float edge_width = max(top_edge_width, 0.01);
    float top_edge_env = top_edge_skew!=0.0 ? exp(-max(raster.x, 0.0) / edge_width) : 0.0;
    float top_band_px = top_band_shift * band_window;
    float top_edge_px = top_edge_skew * band_window * top_edge_env;

    float x_land_n = cx + s_correction + ew_residual + left_edge_nl + right_edge_nl + line_bow_x
                   + ((h_shift_px + top_band_px + top_edge_px) * 2.0 / fw);
    float y_land_n = cy + bottom_compress + line_bow_y
                   + ((field_jump_px + top_settle_px) * 2.0 / fh);

    float edge_factor = clamp(cx * cx + cy * cy, 0.0, 2.0);
    float conv_edge = clamp(edge_factor * 0.5, 0.0, 1.0);

    /* Generic R/B convergence from the old display shader lives here
     * now, converted from beam-space pixels to signal samples. */
    float generic_conv_px = convergence_static
                          + convergence_dynamic * clamp(sqrt(conv_edge), 0.0, 1.0);
    float generic_conv_signal = generic_conv_px * float(signal_w) / fw;

    float base_x = (x_land_n * 0.5 + 0.5) * float(signal_w);
    float base_y = (y_land_n * 0.5 + 0.5) * fh;

    float r_x = base_x - generic_conv_signal + conv_r_x * conv_edge;
    float g_x = base_x;
    float b_x = base_x + generic_conv_signal + conv_b_x * conv_edge;

    float r_y = base_y + conv_r_y * conv_edge;
    float g_y = base_y;
    float b_y = base_y + conv_b_y * conv_edge;

    /* Focus growth + astigmatism. */
    float focus_scale = 1.0 + edge_factor * edge_focus;
    if (corner_astigmatism > 0.001 && edge_factor > 1e-6) {
        float vy_radial = (cy * cy) / edge_factor;
        focus_scale *= 1.0 + corner_astigmatism * edge_factor * vy_radial;
    }
    if (psu_hum > 0.001) {
        float psu_phase = 6.283185 * (line_uv + float(frame_counter) * 0.002);
        focus_scale *= 1.0 + psu_hum * 0.4 * sin(psu_phase);
    }
    if (focus_breathing > 0.001) {
        focus_scale *= 1.0 + focus_breathing * picture_load;
    }

    /* Dwell / velocity. Derived from the local horizontal deflection
     * slope rather than an ad-hoc edge darkening term. */
    float dx_dcx = 1.0+geometry_slope+bow_slope
                 - (2.0/fw)*top_edge_skew*band_window*top_edge_env/edge_width;
    float dwell = 1.0;
    if (velocity_dim > 0.001) {
        float fastness = clamp(abs(dx_dcx) - 1.0, 0.0, 1.5);
        dwell -= velocity_dim * fastness * (0.35 + 0.65 * conv_edge);
    }
    /* Slight left-edge clamp-settle dim and right-edge retrace lift. */
    float left_settle = exp(-max(raster.x, 0.0) / 0.030);
    float right_retrace = exp(-max(1.0 - raster.x, 0.0) / 0.026);
    float edge_drive = clamp(geometry_warp * 0.45 + velocity_dim * 1.2, 0.0, 1.0);
    dwell *= 1.0 - 0.10 * edge_drive * left_settle;
    dwell *= 1.0 + 0.06 * edge_drive * right_retrace;

    uint idx = (oy * out_w + ox) * 4u;
    out_x[idx + 0u] = r_x;
    out_x[idx + 1u] = g_x;
    out_x[idx + 2u] = b_x;
    out_x[idx + 3u] = clamp(dwell, 0.0, 1.10);

    out_y[idx + 0u] = r_y;
    out_y[idx + 1u] = g_y;
    out_y[idx + 2u] = b_y;
    out_y[idx + 3u] = clamp(focus_scale, 1.0, 3.5);
}
