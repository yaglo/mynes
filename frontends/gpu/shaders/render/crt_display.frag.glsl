/*
 * CRT Display — Fragment Shader
 * ===============================
 *
 * Linear-light beam and phosphor history enter this pass. It applies the
 * fixed phosphor mask, glass scattering and room reflection, then encodes
 * SDR sRGB or extended linear sRGB for the swapchain.
 *
 * Dispatch: bind fullscreen.vert, draw 3 vertices.
 */

#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 frag_color;

/* Textures. */
layout(set = 2, binding = 0) uniform sampler2D tex_composite;  /* linear beam/history */
layout(set = 2, binding = 1) uniform sampler2D tex_halation;   /* scattered linear beam light */

layout(set = 2, binding = 2) uniform sampler2D tex_mask; /* linear phosphor coverage, mip filtered */

/* Uniforms matching TVDisplayParams fields. */
layout(set = 3, binding = 0) uniform DisplayParams {
    vec2  src_size;               /* composite texture dimensions (1170, 960) */
    vec2  out_size;               /* display/viewport dimensions */
    float barrel;                 /* horizontal curvature */
    float barrel_v;               /* vertical curvature (0=same as barrel) */
    float convergence_static;     /* legacy UBO slot; beam path owns convergence */
    float convergence_dynamic;    /* legacy UBO slot; beam path owns convergence */
    float mask_strength;          /* phosphor mask modulation depth (0-1) */
    int   mask_type;              /* 0=shadow, 1=aperture_grille, 2=slot */
    float mask_pitch_pixels;      /* mask pitch in display pixels */
    float halation_strength;      /* halation blend amount */
    float vignette_strength;      /* corner darkening (0-0.3) */
    float gamma;                  /* CRT gamma (2.2-2.5) */
    float black_floor;            /* minimum black level (0-1, normalized) */
    float ambient_light;          /* reflected room light */
    float glass_tint;             /* glass transmittance multiplier */
    float hdr_gain;               /* output multiplier (compensate mask/grille darkening) */
    int   subpixel_layout;        /* 0=none, 1=RGB stripe, 2=BGR stripe */
    float overscan;               /* bezel crop fraction per edge (0-0.08) */
    float keystone;               /* trapezoidal distortion (-0.1 to +0.1) */
    float rotation;               /* image rotation in radians (-0.05 to +0.05) */
    float skew_x;                 /* horizontal parallelogram shear (-0.1 to +0.1) */
    float skew_y;                 /* vertical parallelogram shear (-0.1 to +0.1) */
    float hv_sag;                 /* HV-supply-sag intensity (0=none, 0.3=visible breathing) */
    float frame_brightness;       /* average frame luminance 0-1 (computed on CPU each frame) */
    float h_pos;                  /* horizontal image position (-0.5 to +0.5 of tube) */
    float v_pos;                  /* vertical image position (-0.5 to +0.5 of tube) */
    float h_size;                 /* horizontal image size (0.5 = half, 1.0 = fill, 1.5 = overscan) */
    float v_size;                 /* vertical image size (0.5 = half, 1.0 = fill, 1.5 = overscan) */
    float halation_tint_r;        /* halation bloom per-channel tint */
    float halation_tint_g;
    float halation_tint_b;
    /* Phase-B/C additions (reference §3.6 / §4.8 / §5.6 / §5.9 / §6.1 / §5.1 / §5.3). */
    float phosphor_gamma_offset_r;
    float phosphor_gamma_offset_g;
    float phosphor_gamma_offset_b;
    float secondary_scatter;      /* §4.8: cross-phosphor desat */
    float glass_reflection;       /* generic internal-scatter fraction / .08 */
    float antiglare_blur;         /* §5.6: sub-pixel matte scatter */
    float emi_gradient;           /* §5.9: deflection-EMI brightness bar */
    float degauss_tint;           /* §6.1: residual corner color tint */
    float phosphor_grain;         /* §5.1: fixed-pattern grain noise */
    float cathode_center_dim;     /* §5.3: centre dimmer than rim */
    float cathode_gain_r;         /* §5.3: per-gun aging gain (1=no aging) */
    float cathode_gain_g;
    float cathode_gain_b;
    vec2 reserved_apl; // DC restoration now belongs to gun drive.
    /* §5.2 thermal-mask doming approximation. */
    float thermal_dome_amount;
    float thermal_r;
    float thermal_g;
    float thermal_b;
    /* §5.4 Phosphor chromaticity shift with drive level. */
    float chromaticity_drive_shift;
    /* §6.2 microphonic audio-coupled raster wobble. */
    float microphonic_amount;
    float audio_bass_rms;
    float frame_phase;
    /* §6.3 glass-face specular + diffuse reflection of the room. */
    float glass_glare;
    float glass_glare_light_x;
    float glass_glare_light_y;
    float glass_glare_size;
    float glass_glare_temp_k;
    float input_gamma, hdr_headroom, sdr_white_level;
    int output_hdr;
    float mask_row_pitch;
    vec2 mask_scale, mask_origin;
    vec4 phosphor_to_display[3];
    vec4 presentation; // x: host-refresh emission multiplier
    vec4 monitor; // x: 1 = FW900 physical variable-pitch grille
};

/* Mask coordinates are local to the CRT viewport. Each stripe is one
 * phosphor; three stripes form an RGB triad. The beam has already spread
 * upstream: mask coverage must not blur RGB samples a second time. */
// Band-limited aperture stripes. A positive Fejer reconstruction filter
// removes unresolved harmonics without erasing the resolved RGB fundamental.
// Interpolating successive filter orders makes resize continuous; both have
// unit integral and nonnegative coverage. The final sinc integrates the
// output pixel footprint. This filters the phosphor face, not the beam.
float sinc_pi(float x) {
    return abs(x)<0.0001 ? 1.0 : sin(3.14159265359*x)/(3.14159265359*x);
}
vec3 aperture_mask(float x, float pitch) {
    float period=3.0*max(pitch,0.05);
    float footprint=max(1.0,mask_scale.x);
    float sampled_period=period/footprint;
    int order=min(int(floor(sampled_period*0.5)),16);
    if(order<1) return vec3(1.0);
    float transition=1.0-smoothstep(0.45,0.5,float(order)/sampled_period);
    // A grille wire separates RGB groups more than neighbouring phosphors.
    // Equal gaps at every colour erased the achromatic triad structure.
    // These normalized stripe dimensions are nominal, not a Sony tube fit.
    vec3 phase=6.28318530718*(x/period-vec3(0.21,0.50,0.79));
    vec3 coverage=vec3(1.0);
    for(int n=1;n<=order;n++) {
        float k=float(n);
        float previous=max(1.0-k/float(order),0.0);
        float current=1.0-k/float(order+1);
        float weight=mix(previous,current,transition);
        float amplitude=2.0*sinc_pi(k*0.28)*sinc_pi(k/sampled_period)*weight;
        coverage+=amplitude*cos(k*phase);
    }
    return subpixel_layout==2 ? coverage.bgr : coverage;
}
vec3 phosphor_mask(vec2 pos, vec2 face_uv) {
    if (monitor.x == 1.0) {
        // Published centre/edge pitch; quadratic variation is an assumption.
        float t=face_uv.x-0.5;
        float pitch_mm=0.23+0.16*t*t;
        float phase=479.298/(2.0*sqrt(0.23*0.04))*atan(2.0*t*sqrt(0.04/0.23));
        float pitch=out_size.x*mask_scale.x*pitch_mm/(479.298*3.0);
        return aperture_mask(phase*3.0*pitch,pitch);
    }
    if(mask_type==1) return aperture_mask(pos.x,mask_pitch_pixels);
    float pitch=max(mask_pitch_pixels,0.05);
    float row_pitch=mask_row_pitch>0.0 ? mask_row_pitch : pitch*(mask_type==2 ? 2.4 : 0.8660254);
    vec2 period=vec2((mask_type==2 ? 6.0 : 3.0)*pitch,2.0*row_pitch);
    vec2 coord=pos/period;
    // The coarsest mip is the actual per-colour mean of the generated tile.
    // Dividing by it keeps each primary's field energy at one after filtering.
    vec3 dc=textureLod(tex_mask,vec2(0.5),7.0).rgb;
    // Filter for whichever is coarser: drawable pixels or physical panel pixels.
    vec2 footprint=max(vec2(1.0),mask_scale)/period;
    vec3 coverage=textureGrad(tex_mask,coord,vec2(footprint.x,0.0),vec2(0.0,footprint.y)).rgb/max(dc,vec3(0.001));
    float unresolved=smoothstep(0.40,0.50,max(1.0,mask_scale.x)/(3.0*pitch));
    coverage=mix(coverage,vec3(1.0),unresolved);
    return subpixel_layout==2 ? coverage.bgr : coverage;
}
vec3 beam_light(vec2 p) {
    vec3 v = max(texture(tex_composite,p).rgb,vec3(0.0));
    return input_gamma > 0.0 ? pow(v,vec3(input_gamma)) : v;
}
vec3 srgb_encode(vec3 v) {
    return mix(12.92*v,1.055*pow(v,vec3(1.0/2.4))-0.055,greaterThan(v,vec3(0.0031308)));
}

/* -----------------------------------------------------------------------
 * Hash-based tube-fingerprint noise (§5.1 phosphor grain, §6.1
 * degauss tint). A cheap 2D hash gives each screen position a stable
 * pseudo-random value — same every frame, so the artefact looks like
 * a property of the tube rather than swimming noise.
 */
float tube_hash(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

/* -----------------------------------------------------------------------
 * (f) Vignette
 * -----------------------------------------------------------------------
 * cos^4 law approximation of light falloff from center to edges.
 */
float vignette_factor(vec2 coord, float strength) {
    vec2 centered = coord - 0.5;
    float r2 = dot(centered, centered);
    /* cos^4 approximation: (1 - r^2)^2 for r in [0, ~0.7] */
    float v = 1.0 - r2 * strength * 4.0;
    return clamp(v * v, 0.0, 1.0);
}

// Excitation and emitted spectral light at one physical phosphor location.
// Optical surface scattering samples this result, including the grille.
vec3 phosphor_light(vec2 p, vec2 face_pos) {
    vec3 color=beam_light(p);
    // Generic purity error: redistribute excitation between phosphors,
    // before their spatial coverage is applied. Magnetic mislanding cannot
    // emit light from a black input. Smooth Cartesian lobes avoid an atan
    // seam; this is not a measured magnetic field or electron-optics model.
    if (degauss_tint > 0.001) {
        vec2 pos = (p - 0.5) * 2.0;
        vec3 lobes = 0.5 + 0.25 * vec3(pos.x, -0.5*pos.x + 0.8660254*pos.y,
                                      -0.5*pos.x - 0.8660254*pos.y);
        vec3 lost = color * clamp(degauss_tint * dot(pos,pos) * 0.12 * lobes, 0.0, 0.5);
        color += 0.5 * (lost.yzx + lost.zxy) - lost;
    }
    // Approximate cross-phosphor excitation before the mask, so the light
    // appears at the receiving phosphor's sites rather than in its gaps.
    if (secondary_scatter > 0.001) {
        float mean_excitation = (color.r + color.g + color.b) / 3.0;
        color = mix(color, vec3(mean_excitation), clamp(secondary_scatter, 0.0, 1.0));
    }

    vec3 drive=color;
    if (mask_strength > 0.01)
        color *= mix(vec3(1.0), phosphor_mask(face_pos,p), mask_strength);
    // Generic legacy material-response control, not a measured phosphor fit.
    // Use unmasked excitation: changing host pitch must not change the
    // response curve. Shifted emission remains at its originating stripe.
    if (chromaticity_drive_shift > 0.001) {
        float s=clamp(chromaticity_drive_shift,0.0,1.0);
        vec3 response=drive/(1.0+drive);
        color.b=color.b*(1.0-0.03*s*response.b)+0.04*s*response.g*color.g;
        color.r*=1.0+0.02*s*response.r;
    }
    return color;
}

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */
void main() {
    /* Beam/raster geometry now lives in deflection.comp.glsl, so this
     * pass only samples the already-landed beam texture and applies
     * fixed screen/glass optics at the physical tube face. */
    vec2 sample_uv = clamp(uv, vec2(0.0), vec2(1.0));
    vec3 color;

    vec2 face_pos=gl_FragCoord.xy*mask_scale+mask_origin;
    color=phosphor_light(sample_uv,face_pos);
    // Fine surface scatter acts AFTER emission through the fixed grille.
    // Radius follows phosphor pitch, not source texture or UI-point size.
    if (antiglare_blur > 0.001) {
        float amount=clamp(antiglare_blur,0.0,1.0);
        vec2 radius=vec2(amount*mask_pitch_pixels)/max(mask_scale,vec2(0.001));
        vec2 dx=vec2(radius.x,0),dy=vec2(0,radius.y);
        vec3 blurred=0.25*(
            phosphor_light(sample_uv+dx/out_size,face_pos+dx*mask_scale)+
            phosphor_light(sample_uv-dx/out_size,face_pos-dx*mask_scale)+
            phosphor_light(sample_uv+dy/out_size,face_pos+dy*mask_scale)+
            phosphor_light(sample_uv-dy/out_size,face_pos-dy*mask_scale));
        color=mix(color,blurred,amount);
    }

    // Glass transports emitted light into neighbouring areas. Both controls
    // use the generic faceplate PSF; neither creates light from a local luma
    // pedestal. Tint sets wavelength-dependent scatter fractions, so changing
    // it redistributes each primary without changing a uniform field's colour.
    if (halation_strength > 0.001 || glass_reflection > 0.001) {
        vec3 halo = texture(tex_halation, sample_uv).rgb;
        vec3 tint = vec3(halation_tint_r, halation_tint_g, halation_tint_b);
        if (tint.r + tint.g + tint.b < 1e-4) tint = vec3(1.0);
        vec3 h=clamp(halation_strength*tint,vec3(0.0),vec3(1.0));
        float r=clamp(glass_reflection*0.08,0.0,1.0);
        color=mix(color,halo,1.0-(1.0-h)*(1.0-r));
    }

    /* §5.3 cathode aging / non-uniformity — center dims faster than
     * edges, and the three guns age at different rates. Multiplicative
     * shape, so zero amplitude = identity. */
    if (cathode_center_dim > 0.001 ||
        abs(cathode_gain_r - 1.0) > 0.001 ||
        abs(cathode_gain_g - 1.0) > 0.001 ||
        abs(cathode_gain_b - 1.0) > 0.001) {
        vec2 cd = uv - 0.5;
        float r2 = dot(cd, cd) * 4.0;   /* 0 at center, ~1 at corners */
        float center_shape = 1.0 - cathode_center_dim * (1.0 - r2);
        color *= vec3(cathode_gain_r, cathode_gain_g, cathode_gain_b)
               * center_shape;
    }

    /* §5.9 EMI brightness gradient — deflection-yoke field coupling
     * produces a smooth left-to-right lift peaking near the left
     * retrace edge. Amplitude scaled by emi_gradient. */
    if (emi_gradient > 0.001) {
        float bar = 1.0 - smoothstep(0.0, 0.6, uv.x);
        color *= (1.0 + emi_gradient * 0.1 * bar);
    }

    /* §5.1 phosphor grain — fine high-frequency multiplicative noise,
     * static per tube, correlated across R/G/B so it acts as a
     * brightness perturbation rather than per-channel colour noise. */
    if (phosphor_grain > 0.001) {
        float g = tube_hash(uv * vec2(1800.0, 1400.0));
        color *= (1.0 + phosphor_grain * (g - 0.5) * 2.0);
    }

    /* §5.2 thermal-mask doming — the long-timescale per-channel
     * brightness averages shift the image's overall tint as the
     * shadow mask warms unevenly. Very slow EMA on the CPU side
     * (~15 s time constant); here we compute each channel's drift
     * away from the mean thermal load and lift/dim accordingly. */
    if (thermal_dome_amount > 0.001) {
        float mean_t = (thermal_r + thermal_g + thermal_b) * (1.0 / 3.0);
        vec3 drift = vec3(thermal_r - mean_t,
                          thermal_g - mean_t,
                          thermal_b - mean_t);
        color *= (vec3(1.0) + drift * thermal_dome_amount * 1.5);
    }

    // Excited phosphors cannot emit negative light. Signed components are
    // valid only after converting this light to the host colour space.
    color=max(color,vec3(0.0));

    // Express phosphor emission in the host's linear-sRGB colour space,
    // after the mask: a red phosphor is not an LCD's ideal red primary.
    color=vec3(dot(phosphor_to_display[0].rgb,color),
               dot(phosphor_to_display[1].rgb,color),
               dot(phosphor_to_display[2].rgb,color));

    /* Apply glass tint (phosphor light attenuated through glass). */
    color *= glass_tint;

    /* (f) Vignette. */
    color *= vignette_factor(uv, vignette_strength);

    // Linear emission gain uses HDR headroom for phosphor peaks. Reflected
    // room light is independent of tube drive and must not rise with it.
    color *= (hdr_gain > 0.0 ? hdr_gain : 1.0) * presentation.x;

    /* (h) Black floor already applied in beam shader — don't double it.
     *     Only add ambient light reflection on the glass surface. */
    color += vec3(ambient_light * 0.15);

    /* §6.3 Glass-face glare — external reflection of the viewer's
     * room on the outer glass surface.
     *
     * A CRT tube face is a RECTANGLE (with slightly rounded
     * corners), not a disc. Consumer CRTs are nearly flat at the
     * viewing area, so Fresnel is barely position-dependent across
     * the visible glass — what you actually see is the room
     * reflected over the whole rectangular surface, with a small
     * extra lift along the bezel edges where the curvature picks
     * up.
     *
     * Build-up:
     *   1. Fill the whole tube rectangle with the procedurally
     *      rendered room environment (vertical brightness gradient
     *      + rectangular key light).
     *   2. Add a rectangular edge accent (distance-to-nearest-edge,
     *      NOT radial distance) — models the slight bezel-shadow
     *      boost real CRTs show along their four edges.
     *   3. Multiply the whole thing by glass_glare amplitude.
     *
     * Distinct from:
     *   glass_tint        — absorption THROUGH the glass
     *   glass_reflection / halation — spatially scattered phosphor light */
    if (glass_glare > 0.001) {
        /* Reflection UV: the whole tube face, not a disc. */
        vec2 env_uv = uv;

        /* Colour-temperature tint (Planckian locus approximation). */
        vec3 warm = vec3(1.0);
        if (glass_glare_temp_k > 100.0) {
            float t = glass_glare_temp_k * 0.01;
            if (glass_glare_temp_k <= 6600.0) {
                warm.r = 1.0;
                warm.g = clamp(0.390 * log(t) - 0.631, 0.0, 1.0);
                warm.b = (glass_glare_temp_k < 1900.0)
                         ? 0.0
                         : clamp(0.543 * log(t - 10.0) - 1.196, 0.0, 1.0);
            } else {
                warm.r = clamp(1.293 * pow(t - 60.0, -0.1332), 0.0, 1.0);
                warm.g = clamp(1.129 * pow(t - 60.0, -0.0755), 0.0, 1.0);
                warm.b = 1.0;
            }
        }

        /* Room environment — vertical brightness gradient.
         *
         * A viewer sits in front of the CRT, slightly above screen
         * centre, looking down. Reflection geometry inverts, so:
         *   top of glass    ← reflects desk / floor (dim)
         *   middle of glass ← reflects walls (dark-neutral)
         *   bottom of glass ← reflects ceiling / lights (bright)
         *
         * The gradient runs INVERTED from "normal" pictures of a
         * room because of this reflection inversion. */
        float h = env_uv.y;
        vec3 floor_c   = warm * 0.03;
        vec3 wall_c    = warm * 0.10;
        vec3 ceiling_c = warm * 0.45;
        vec3 env = mix(floor_c, wall_c, smoothstep(0.0, 0.55, h));
        env      = mix(env, ceiling_c, smoothstep(0.55, 1.0, h));

        /* Key light (overhead lamp / window). Elongated horizontal
         * shape — most real indoor key sources are rectangular bars
         * or windows, not round dots. Defaults put it in the lower
         * third of the glass (= the ceiling's reflected position). */
        vec2 light_xy = vec2(glass_glare_light_x, 1.0 - glass_glare_light_y);
        vec2 d_light  = env_uv - light_xy;
        float sz      = max(glass_glare_size, 0.02);
        float key_g = exp(-(d_light.x * d_light.x) / (2.0 * sz * sz * 6.0)
                         -(d_light.y * d_light.y) / (2.0 * sz * sz * 1.0));
        env += warm * key_g * 1.8;

        /* Matte screens diffuse the reflection toward its mean. */
        float matte = clamp(antiglare_blur, 0.0, 1.0);
        if (matte > 0.001) {
            vec3 mean = (floor_c + wall_c + ceiling_c) * 0.333
                      + warm * 0.15;
            env = mix(env, mean, matte * 0.7);
        }

        /* Rectangular edge accent — NOT a radial Fresnel disc. The
         * nearest edge of the tube rectangle picks up extra glare
         * because the glass curves away there and the bezel shadow
         * falls on the outer glass. Distance-to-nearest-edge maps
         * to [0..0.5] where 0.5 is at the screen center.
         *
         * The accent is a smooth rectangular ring with a ~12 %
         * lift within 6 % of any edge — the signature "framed
         * brightening" you see on a well-lit CRT. */
        vec2 edge_vec = min(env_uv, 1.0 - env_uv);       /* 0..0.5 */
        float edge_d  = min(edge_vec.x, edge_vec.y);      /* L∞ norm */
        float edge_boost = 1.0 + 0.25 *
                           (1.0 - smoothstep(0.0, 0.08, edge_d));

        /* Soft rounded-corner falloff: at the extreme corners of a
         * CRT the bezel covers more and the visible glass shrinks
         * smoothly — dampen glare where both X and Y edge distances
         * are small simultaneously. */
        float corner_fade = smoothstep(0.0, 0.04,
                                        sqrt(edge_vec.x * edge_vec.y));

        /* Apply uniformly across the rectangle. No radial gating. */
        color += env * edge_boost * corner_fade * glass_glare * 1.6;
    }

    // Negative components after the phosphor-primary transform can describe
    // real colours outside sRGB. Extended-linear HDR carries them to the
    // host colour manager; clipping them here changes their chromaticity.
    // SDR has no signed representation. Reduce chroma towards an equal-Y
    // neutral only as much as needed, instead of clipping channels separately.
    if (output_hdr == 0) {
        float low = min(min(color.r,color.g),color.b);
        if (low < 0.0) {
            float y = max(dot(color,vec3(0.2126,0.7152,0.0722)),0.0);
            color = mix(vec3(y),color,y / max(y-low,0.000001));
        }
        color = max(color,vec3(0.0));
    }
    // Output adaptation, not tube physics. A continuous shoulder preserves
    // highlight gradients and RGB ratios when the host lacks phosphor peak
    // headroom. Independent channel clipping used to wash out the grille.
    float peak=max(max(color.r,color.g),color.b);
    float limit=max(hdr_headroom,1.0), knee=0.75*limit;
    if (peak>knee) {
        float mapped=knee+(limit-knee)*(peak-knee)/(peak-knee+limit-knee);
        color*=mapped/peak;
    }
    color = output_hdr != 0 ? color * sdr_white_level : srgb_encode(color);

    frag_color = vec4(color, 1.0);
}
