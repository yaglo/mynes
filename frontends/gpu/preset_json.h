/*
 * preset_json.h -- JSON save/load for PhysicalPreset structs
 *
 * Header-only. Provides:
 *   preset_json_save()     -- write a preset as pretty-printed JSON
 *   preset_json_load()     -- read a JSON file into a preset
 *   preset_json_scan_dir() -- list .json files in a directory
 */
#ifndef PRESET_JSON_H
#define PRESET_JSON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include <dirent.h>
#include "presets.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================================
 * Enum ↔ string tables (resilient to enum reordering)
 * ============================================================================ */

/* Map enum ints to strings. Unknown values fall through to "unknown". */
static inline const char *preset_json__connection_name(int v) {
    switch (v) {
    case VIDEO_CONN_RF:        return "rf";
    case VIDEO_CONN_COMPOSITE: return "composite";
    case VIDEO_CONN_SVIDEO:    return "svideo";
    case VIDEO_CONN_COMPONENT: return "component";
    case VIDEO_CONN_RGB:       return "rgb";
    case VIDEO_CONN_DIRECT:    return "direct";
    default: return "composite";
    }
}
static inline int preset_json__connection_from(const char *s) {
    if (!s) return VIDEO_CONN_COMPOSITE;
    if (!strcmp(s, "rf"))        return VIDEO_CONN_RF;
    if (!strcmp(s, "composite")) return VIDEO_CONN_COMPOSITE;
    if (!strcmp(s, "svideo"))    return VIDEO_CONN_SVIDEO;
    if (!strcmp(s, "component")) return VIDEO_CONN_COMPONENT;
    if (!strcmp(s, "rgb"))       return VIDEO_CONN_RGB;
    if (!strcmp(s, "direct"))    return VIDEO_CONN_DIRECT;
    /* Numeric fallback for backward compat with integer-valued JSONs. */
    char *end; long v = strtol(s, &end, 10);
    return (end != s) ? (int)v : VIDEO_CONN_COMPOSITE;
}

static inline const char *preset_json__comb_name(int v) {
    switch (v) {
    case VIDEO_COMB_NONE:   return "none";
    case VIDEO_COMB_1LINE:  return "1line";
    case VIDEO_COMB_2LINE:  return "2line";
    case VIDEO_COMB_3LINE:  return "3line";
    case VIDEO_COMB_BYPASS: return "bypass";
    default: return "none";
    }
}
static inline int preset_json__comb_from(const char *s) {
    if (!s) return VIDEO_COMB_NONE;
    if (!strcmp(s, "none"))   return VIDEO_COMB_NONE;
    if (!strcmp(s, "1line"))  return VIDEO_COMB_1LINE;
    if (!strcmp(s, "2line"))  return VIDEO_COMB_2LINE;
    if (!strcmp(s, "3line"))  return VIDEO_COMB_3LINE;
    if (!strcmp(s, "bypass")) return VIDEO_COMB_BYPASS;
    char *end; long v = strtol(s, &end, 10);
    return (end != s) ? (int)v : VIDEO_COMB_NONE;
}

static inline const char *preset_json__mask_name(int v) {
    switch (v) {
    case VIDEO_MASK_SHADOW:          return "shadow";
    case VIDEO_MASK_APERTURE_GRILLE: return "aperture_grille";
    case VIDEO_MASK_SLOT:            return "slot";
    default: return "shadow";
    }
}
static inline int preset_json__mask_from(const char *s) {
    if (!s) return VIDEO_MASK_SHADOW;
    if (!strcmp(s, "shadow"))          return VIDEO_MASK_SHADOW;
    if (!strcmp(s, "aperture_grille")) return VIDEO_MASK_APERTURE_GRILLE;
    if (!strcmp(s, "slot"))            return VIDEO_MASK_SLOT;
    char *end; long v = strtol(s, &end, 10);
    return (end != s) ? (int)v : VIDEO_MASK_SHADOW;
}

static inline const char *preset_json__region_name(int v) {
    return (v == SIGNAL_REGION_PAL) ? "pal" : "ntsc";
}
static inline int preset_json__region_from(const char *s) {
    if (!s) return SIGNAL_REGION_NTSC;
    if (!strcmp(s, "pal"))  return SIGNAL_REGION_PAL;
    if (!strcmp(s, "ntsc")) return SIGNAL_REGION_NTSC;
    char *end; long v = strtol(s, &end, 10);
    return (end != s) ? (int)v : SIGNAL_REGION_NTSC;
}

/* ============================================================================
 * Internal helpers
 * ============================================================================ */

/* Write a JSON-escaped string (handles quotes and backslashes). */
static inline void preset_json__write_escaped(FILE *f, const char *s)
{
    if (!s) { fprintf(f, "\"\""); return; }
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case '"':  fprintf(f, "\\\""); break;
        case '\\': fprintf(f, "\\\\"); break;
        case '\n': fprintf(f, "\\n");  break;
        case '\r': fprintf(f, "\\r");  break;
        case '\t': fprintf(f, "\\t");  break;
        default:   fputc(*p, f);       break;
        }
    }
    fputc('"', f);
}

/* Strip leading/trailing whitespace in-place, return pointer into buf. */
static inline char *preset_json__strip(char *buf)
{
    while (*buf == ' ' || *buf == '\t') buf++;
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\t' ||
                       buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    return buf;
}

static inline unsigned preset_json__hex4(const char *s)
{
    unsigned value=0;
    for (int i=0;i<4;i++) {
        unsigned c=(unsigned char)s[i];
        value=(value<<4) | (c<='9' ? c-'0' : (c|32)-'a'+10);
    }
    return value;
}

/* Unescape a validated JSON string in-place, including Unicode to UTF-8.
 * Returns a pointer past the surrounding quote. */
static inline char *preset_json__unescape(char *buf)
{
    /* Strip surrounding quotes. */
    size_t len = strlen(buf);
    if (len >= 2 && buf[0] == '"' && buf[len-1] == '"') {
        buf[len-1] = '\0';
        buf++;
    }
    /* Process escape sequences in-place. */
    char *dst = buf;
    for (const char *src = buf; *src; src++) {
        if (*src == '\\' && src[1]) {
            src++;
            switch (*src) {
            case '"':  *dst++ = '"';  break;
            case '\\': *dst++ = '\\'; break;
            case 'n':  *dst++ = '\n'; break;
            case 'r':  *dst++ = '\r'; break;
            case 't':  *dst++ = '\t'; break;
            case 'b':  *dst++ = '\b'; break;
            case 'f':  *dst++ = '\f'; break;
            case 'u': {
                /* The lexer has validated the four hex digits. UTF-8 is
                 * never longer than the consumed JSON escape(s). */
                unsigned cp=preset_json__hex4(src+1); src+=4;
                if (cp>=0xd800 && cp<=0xdbff && src[1]=='\\' && src[2]=='u') {
                    unsigned low=preset_json__hex4(src+3);
                    if (low>=0xdc00 && low<=0xdfff) {
                        cp=0x10000+((cp-0xd800)<<10)+(low-0xdc00); src+=6;
                    }
                }
                if (cp>=0xd800 && cp<=0xdfff) cp=0xfffd;
                if (cp<0x80) *dst++=(char)cp;
                else if (cp<0x800) {
                    *dst++=(char)(0xc0|(cp>>6)); *dst++=(char)(0x80|(cp&63));
                } else if (cp<0x10000) {
                    *dst++=(char)(0xe0|(cp>>12)); *dst++=(char)(0x80|((cp>>6)&63));
                    *dst++=(char)(0x80|(cp&63));
                } else {
                    *dst++=(char)(0xf0|(cp>>18)); *dst++=(char)(0x80|((cp>>12)&63));
                    *dst++=(char)(0x80|((cp>>6)&63)); *dst++=(char)(0x80|(cp&63));
                }
                break;
            }
            default:   *dst++ = *src; break;
            }
        } else {
            *dst++ = *src;
        }
    }
    *dst = '\0';
    return buf;
}

/* ============================================================================
 * preset_json_save -- write a PhysicalPreset as pretty-printed JSON
 * ============================================================================ */

static inline bool preset_json_save(const PhysicalPreset *p, const char *path)
{
    if (!p || !path) return false;

    FILE *f = fopen(path, "w");
    if (!f) return false;

    fprintf(f, "{\n");

    /* ---- Identity ---- */
    fprintf(f, "    \"name\": ");
    preset_json__write_escaped(f, p->name);
    fprintf(f, ",\n");

    fprintf(f, "    \"description\": ");
    preset_json__write_escaped(f, p->description);
    fprintf(f, ",\n");

    /* ---- Signal path topology (enums as strings for resilience) ---- */
    fprintf(f, "    \"connection\": \"%s\",\n",   preset_json__connection_name((int)p->connection));
    fprintf(f, "    \"comb_type\": \"%s\",\n",    preset_json__comb_name((int)p->comb_type));
    fprintf(f, "    \"comb_notch_depth\": %.6f,\n", p->comb_notch_depth);
    fprintf(f, "    \"console_variant\": %d,\n",  (int)p->console_variant);
    fprintf(f, "    \"speaker_type\": %d,\n",     p->speaker_type);
    fprintf(f, "    \"region\": \"%s\",\n",       preset_json__region_name(p->region));

    /* ---- Video cable ---- */
    fprintf(f, "    \"video_cable\": {\n");
    fprintf(f, "        \"length_meters\": %.6f,\n",        p->video_cable.length_meters);
    fprintf(f, "        \"resistance_per_m\": %.6f,\n",     p->video_cable.resistance_per_m);
    fprintf(f, "        \"capacitance_per_m\": %.12g,\n",   p->video_cable.capacitance_per_m);
    fprintf(f, "        \"num_sections\": %d,\n",           p->video_cable.num_sections);
    fprintf(f, "        \"connector_resistance\": %.6f,\n", p->video_cable.connector_resistance);
    fprintf(f, "        \"impedance\": %.6f,\n",            p->video_cable.impedance);
    fprintf(f, "        \"shield_effectiveness\": %.6f,\n", p->video_cable.shield_effectiveness);
    fprintf(f, "        \"ghost_delay\": %d,\n",            p->video_cable.ghost_delay);
    fprintf(f, "        \"ghost_level\": %.6f\n",           p->video_cable.ghost_level);
    fprintf(f, "    },\n");

    /* ---- Audio cable ---- */
    fprintf(f, "    \"audio_cable\": {\n");
    fprintf(f, "        \"length_meters\": %.6f,\n",        p->audio_cable.length_meters);
    fprintf(f, "        \"resistance_per_m\": %.6f,\n",     p->audio_cable.resistance_per_m);
    fprintf(f, "        \"capacitance_per_m\": %.12g,\n",   p->audio_cable.capacitance_per_m);
    fprintf(f, "        \"num_sections\": %d,\n",           p->audio_cable.num_sections);
    fprintf(f, "        \"connector_resistance\": %.6f,\n", p->audio_cable.connector_resistance);
    fprintf(f, "        \"impedance\": %.6f,\n",            p->audio_cable.impedance);
    fprintf(f, "        \"shield_effectiveness\": %.6f,\n", p->audio_cable.shield_effectiveness);
    fprintf(f, "        \"ghost_delay\": %d,\n",            p->audio_cable.ghost_delay);
    fprintf(f, "        \"ghost_level\": %.6f\n",           p->audio_cable.ghost_level);
    fprintf(f, "    },\n");

    /* ---- TV / CRT display ---- */
    fprintf(f, "    \"tv\": {\n");
    /* Chroma demodulator. */
    fprintf(f, "        \"chroma_bandwidth\": %.1f,\n",     p->tv.chroma_bandwidth);
    fprintf(f, "        \"chroma_q_bandwidth\": %.1f,\n",   p->tv.chroma_q_bandwidth);
    fprintf(f, "        \"luma_bandwidth\": %.1f,\n",       p->tv.luma_bandwidth);
    fprintf(f, "        \"hue_offset\": %.6f,\n",           p->tv.hue_offset);
    fprintf(f, "        \"saturation\": %.6f,\n",           p->tv.saturation);
    fprintf(f, "        \"fir_ringing\": %.6f,\n",          p->tv.fir_ringing);
    fprintf(f, "        \"rf_interference\": %.6f,\n",      p->tv.rf_interference);
    fprintf(f, "        \"geometry_warp\": %.6f,\n",        p->tv.geometry_warp);
    /* Matrix decode. */
    fprintf(f, "        \"color_temperature\": %.1f,\n",    p->tv.color_temperature);
    fprintf(f, "        \"monitor_model\": %d,\n", p->tv.monitor_model);
    fprintf(f, "        \"phosphor_gamut\": %d,\n", p->tv.phosphor_gamut);
    fprintf(f, "        \"rgb_bandwidth_3db\": %d,\n", p->tv.rgb_bandwidth_3db);
    fprintf(f, "        \"beam_spot_growth\": %.6f,\n", p->tv.beam_spot_growth);
    fprintf(f, "        \"decoder_blue_gain\": %.6f,\n", p->tv.decoder_blue_gain);
    fprintf(f, "        \"decoder_red_gain\": %.6f,\n", p->tv.decoder_red_gain);
    fprintf(f, "        \"r_drive\": %.6f,\n",              p->tv.r_drive);
    fprintf(f, "        \"g_drive\": %.6f,\n",              p->tv.g_drive);
    fprintf(f, "        \"b_drive\": %.6f,\n",              p->tv.b_drive);
    fprintf(f, "        \"r_cutoff\": %.6f,\n",             p->tv.r_cutoff);
    fprintf(f, "        \"g_cutoff\": %.6f,\n",             p->tv.g_cutoff);
    fprintf(f, "        \"b_cutoff\": %.6f,\n",             p->tv.b_cutoff);
    /* Video amplifier. */
    fprintf(f, "        \"r_bandwidth\": %.1f,\n",          p->tv.r_bandwidth);
    fprintf(f, "        \"g_bandwidth\": %.1f,\n",          p->tv.g_bandwidth);
    fprintf(f, "        \"b_bandwidth\": %.1f,\n",          p->tv.b_bandwidth);
    fprintf(f, "        \"gamma\": %.6f,\n",                p->tv.gamma);
    /* Beam. */
    fprintf(f, "        \"beam_sharpness\": %.6f,\n",       p->tv.beam_sharpness);
    fprintf(f, "        \"beam_height_min\": %.6f,\n",      p->tv.beam_height_min);
    fprintf(f, "        \"beam_height_max\": %.6f,\n",      p->tv.beam_height_max);
    fprintf(f, "        \"beam_fwhm_min\": %.6f,\n",       p->tv.beam_fwhm_min);
    fprintf(f, "        \"beam_fwhm_max\": %.6f,\n",       p->tv.beam_fwhm_max);
    fprintf(f, "        \"beam_spot_size\": %.6f,\n",       p->tv.beam_spot_size);
    fprintf(f, "        \"convergence_static\": %.6f,\n",   p->tv.convergence_static);
    fprintf(f, "        \"convergence_dynamic\": %.6f,\n",  p->tv.convergence_dynamic);
    fprintf(f, "        \"conv_r_x\": %.6f,\n",             p->tv.conv_r_x);
    fprintf(f, "        \"conv_r_y\": %.6f,\n",             p->tv.conv_r_y);
    fprintf(f, "        \"conv_b_x\": %.6f,\n",             p->tv.conv_b_x);
    fprintf(f, "        \"conv_b_y\": %.6f,\n",             p->tv.conv_b_y);
    fprintf(f, "        \"h_jitter\": %.6f,\n",             p->tv.h_jitter);
    fprintf(f, "        \"v_jitter\": %.6f,\n",             p->tv.v_jitter);
    /* Phosphor. */
    fprintf(f, "        \"mask_type\": \"%s\",\n",          preset_json__mask_name((int)p->tv.mask_type));
    fprintf(f, "        \"mask_triads\": %.6f,\n",        p->tv.mask_triads);
    fprintf(f, "        \"mask_pitch_px\": %.6f,\n",        p->tv.mask_pitch_px);
    fprintf(f, "        \"mask_strength\": %.6f,\n",        p->tv.mask_strength);
    fprintf(f, "        \"subpixel_layout\": %d,\n",        p->tv.subpixel_layout);
    fprintf(f, "        \"face_height_mm\": %.6f,\n",        p->tv.face_height_mm);
    fprintf(f, "        \"damper_wires\": %d,\n",           p->tv.damper_wires);
    fprintf(f, "        \"damper_y1\": %.6f,\n",            p->tv.damper_y1);
    fprintf(f, "        \"damper_y2\": %.6f,\n",            p->tv.damper_y2);
    fprintf(f, "        \"damper_wire_um\": %.6f,\n",       p->tv.damper_wire_um);
    fprintf(f, "        \"persistence_ms\": %.6f,\n",       p->tv.persistence_ms);
    /* Glass. */
    fprintf(f, "        \"halation\": %.6f,\n",             p->tv.halation);
    fprintf(f, "        \"halation_sigma\": %.6f,\n",       p->tv.halation_sigma);
    fprintf(f, "        \"glass_tint\": %.6f,\n",           p->tv.glass_tint);
    fprintf(f, "        \"barrel\": %.6f,\n",               p->tv.barrel);
    /* Environment. */
    fprintf(f, "        \"vignette\": %.6f,\n",             p->tv.vignette);
    fprintf(f, "        \"ambient_light\": %.6f,\n",        p->tv.ambient_light);
    /* Black level. */
    fprintf(f, "        \"black_floor\": %.6f,\n",          p->tv.black_floor);
    /* Noise. */
    fprintf(f, "        \"noise_level\": %.6f,\n",          p->tv.noise_level);
    /* Mains hum. */
    fprintf(f, "        \"hum_bar_amplitude\": %.6f,\n",    p->tv.hum_bar_amplitude);
    /* Beam bloom. */
    fprintf(f, "        \"bloom_gamma\": %.6f,\n",          p->tv.bloom_gamma);
    /* Beam physics. */
    fprintf(f, "        \"edge_focus\": %.6f,\n",           p->tv.edge_focus);
    fprintf(f, "        \"velocity_dim\": %.6f,\n",         p->tv.velocity_dim);
    fprintf(f, "        \"halation_tint_r\": %.6f,\n",          p->tv.halation_tint_r);
    fprintf(f, "        \"halation_tint_g\": %.6f,\n",          p->tv.halation_tint_g);
    fprintf(f, "        \"halation_tint_b\": %.6f,\n",          p->tv.halation_tint_b);
    fprintf(f, "        \"beam_current_load\": %.6f,\n",        p->tv.beam_current_load);
    fprintf(f, "        \"video_black_droop\": %.6f,\n",        p->tv.video_black_droop);
    fprintf(f, "        \"video_recovery_us\": %.6f,\n",        p->tv.video_recovery_us);
    fprintf(f, "        \"beam_edge_fade\": %.6f,\n",           p->tv.beam_edge_fade);
    fprintf(f, "        \"beam_edge_overshoot\": %.6f,\n",      p->tv.beam_edge_overshoot);
    fprintf(f, "        \"burst_lock_drift\": %.6f,\n",         p->tv.burst_lock_drift);
    fprintf(f, "        \"burst_lock_drift_width\": %.6f,\n",   p->tv.burst_lock_drift_width);
    /* Barrel distortion. */
    fprintf(f, "        \"barrel_v\": %.6f,\n",             p->tv.barrel_v);
    /* Motion-adaptive comb. */
    fprintf(f, "        \"motion_threshold\": %.6f,\n",     p->tv.motion_threshold);
    fprintf(f, "        \"persistence_tail_ms\": %.6f,\n", p->tv.persistence_tail_ms);
    fprintf(f, "        \"persistence_tail_weight\": %.6f,\n", p->tv.persistence_tail_weight);
    /* Per-channel persistence. */
    fprintf(f, "        \"persistence_r\": %.6f,\n",        p->tv.persistence_r);
    fprintf(f, "        \"persistence_g\": %.6f,\n",        p->tv.persistence_g);
    fprintf(f, "        \"persistence_b\": %.6f,\n",        p->tv.persistence_b);
    /* Colour killer. */
    fprintf(f, "        \"color_killer\": %.6f,\n",         p->tv.color_killer);
    /* HDR gain. */
    fprintf(f, "        \"hdr_gain\": %.6f,\n",             p->tv.hdr_gain);
    /* Luma peaking (TV sharpness). */
    fprintf(f, "        \"luma_peaking\": %.6f,\n",         p->tv.luma_peaking);
    fprintf(f, "        \"h_afc_tau_ms\": %.6f,\n",         p->tv.h_afc_tau_ms);
    fprintf(f, "        \"h_pll_hz\": %.6f,\n",             p->tv.h_pll_hz);
    fprintf(f, "        \"h_pll_damping\": %.6f,\n",        p->tv.h_pll_damping);
    fprintf(f, "        \"h_pll_vblank_gain\": %.6f,\n",    p->tv.h_pll_vblank_gain);
    fprintf(f, "        \"aperture_max_db\": %.6f,\n",         p->tv.aperture_max_db);
    fprintf(f, "        \"luma_notch_depth\": %.6f,\n",     p->tv.luma_notch_depth);
    /* Overscan / bezel crop. */
    fprintf(f, "        \"overscan\": %.6f,\n",             p->tv.overscan);
    /* Additional geometry (keystone, rotation, shear, HV sag). */
    fprintf(f, "        \"keystone\": %.6f,\n",             p->tv.keystone);
    fprintf(f, "        \"rotation\": %.6f,\n",             p->tv.rotation);
    fprintf(f, "        \"skew_x\": %.6f,\n",               p->tv.skew_x);
    fprintf(f, "        \"skew_y\": %.6f,\n",               p->tv.skew_y);
    fprintf(f, "        \"hv_sag\": %.6f,\n",               p->tv.hv_sag);
    /* PSU-driven beam instability. */
    fprintf(f, "        \"focus_breathing\": %.6f,\n",      p->tv.focus_breathing);
    fprintf(f, "        \"scanline_wobble\": %.6f,\n",      p->tv.scanline_wobble);
    fprintf(f, "        \"top_band_shift\": %.6f,\n",       p->tv.top_band_shift);
    fprintf(f, "        \"top_edge_skew\": %.6f,\n",        p->tv.top_edge_skew);
    fprintf(f, "        \"top_band_start\": %.6f,\n",       p->tv.top_band_start);
    fprintf(f, "        \"top_band_end\": %.6f,\n",         p->tv.top_band_end);
    fprintf(f, "        \"top_edge_width\": %.6f,\n",       p->tv.top_edge_width);
    /* Service-menu raster geometry (HPOS/VPOS/HSIZE/VSIZE). */
    fprintf(f, "        \"h_pos\": %.6f,\n",                p->tv.h_pos);
    fprintf(f, "        \"v_pos\": %.6f,\n",                p->tv.v_pos);
    fprintf(f, "        \"h_size\": %.6f,\n",               p->tv.h_size);
    fprintf(f, "        \"v_size\": %.6f,\n",               p->tv.v_size);
    /* Phase-B/C physics extensions (§3.6, §4.8, §5.1, §5.3, §5.6,
     * §5.9, §6.1). All default to 0 — preset backward compat. */
    fprintf(f, "        \"phosphor_gamma_offset_r\": %.6f,\n", p->tv.phosphor_gamma_offset_r);
    fprintf(f, "        \"phosphor_gamma_offset_g\": %.6f,\n", p->tv.phosphor_gamma_offset_g);
    fprintf(f, "        \"phosphor_gamma_offset_b\": %.6f,\n", p->tv.phosphor_gamma_offset_b);
    fprintf(f, "        \"secondary_scatter\": %.6f,\n",       p->tv.secondary_scatter);
    fprintf(f, "        \"glass_reflection\": %.6f,\n",        p->tv.glass_reflection);
    fprintf(f, "        \"antiglare_blur\": %.6f,\n",          p->tv.antiglare_blur);
    fprintf(f, "        \"emi_gradient\": %.6f,\n",            p->tv.emi_gradient);
    fprintf(f, "        \"degauss_tint\": %.6f,\n",            p->tv.degauss_tint);
    fprintf(f, "        \"phosphor_grain\": %.6f,\n",          p->tv.phosphor_grain);
    fprintf(f, "        \"cathode_center_dim\": %.6f,\n",      p->tv.cathode_center_dim);
    fprintf(f, "        \"cathode_gain_r\": %.6f,\n",          p->tv.cathode_gain_r);
    fprintf(f, "        \"cathode_gain_g\": %.6f,\n",          p->tv.cathode_gain_g);
    fprintf(f, "        \"cathode_gain_b\": %.6f,\n",          p->tv.cathode_gain_b);
    fprintf(f, "        \"apl_black_lift\": %.6f,\n",          p->tv.apl_black_lift);
    fprintf(f, "        \"thermal_dome_amount\": %.6f,\n",     p->tv.thermal_dome_amount);
    fprintf(f, "        \"corner_astigmatism\": %.6f,\n",      p->tv.corner_astigmatism);
    fprintf(f, "        \"chromaticity_drive_shift\": %.6f,\n", p->tv.chromaticity_drive_shift);
    fprintf(f, "        \"velocity_mod\": %.6f,\n",            p->tv.velocity_mod);
    fprintf(f, "        \"asym_rise_fall\": %.6f,\n",          p->tv.asym_rise_fall);
    fprintf(f, "        \"vertical_smear\": %.6f,\n",          p->tv.vertical_smear);
    fprintf(f, "        \"microphonic_amount\": %.6f,\n",      p->tv.microphonic_amount);
    fprintf(f, "        \"glass_glare\": %.6f,\n",             p->tv.glass_glare);
    fprintf(f, "        \"glass_glare_light_x\": %.6f,\n",     p->tv.glass_glare_light_x);
    fprintf(f, "        \"glass_glare_light_y\": %.6f,\n",     p->tv.glass_glare_light_y);
    fprintf(f, "        \"glass_glare_size\": %.6f,\n",        p->tv.glass_glare_size);
    fprintf(f, "        \"glass_glare_temp_k\": %.6f\n",       p->tv.glass_glare_temp_k);
    fprintf(f, "    },\n");

    /* ---- Console output stage ---- */
    fprintf(f, "    \"console_coupling_R\": %.6f,\n",       p->console_coupling_R);
    fprintf(f, "    \"console_coupling_C\": %.12g,\n",      p->console_coupling_C);
    fprintf(f, "    \"console_amp_bw\": %.1f,\n",           p->console_amp_bw);
    fprintf(f, "    \"console_phase_distortion_ns\": %.1f,\n", p->console_phase_distortion_ns);
    fprintf(f, "    \"console_follower_tau_ns\": %.1f,\n", p->console_follower_tau_ns);
    fprintf(f, "    \"console_psu_hum\": %.6f,\n",          p->console_psu_hum);

    /* ---- RF modulator ---- */
    fprintf(f, "    \"rf\": {\n");
    fprintf(f, "        \"enabled\": %s,\n",                p->rf.enabled ? "true" : "false");
    fprintf(f, "        \"carrier_freq\": %.2f,\n",         p->rf.carrier_freq);
    fprintf(f, "        \"carrier_level_dbm\": %.6f,\n", p->rf.carrier_level_dbm);
    fprintf(f, "        \"mod_bandwidth\": %.2f,\n",        p->rf.mod_bandwidth);
    fprintf(f, "        \"noise_floor_dbm\": %.2f,\n",      p->rf.noise_floor_dbm);
    fprintf(f, "        \"if_asymmetry\": %.6f,\n", p->rf.if_asymmetry);
    fprintf(f, "        \"tuning_offset_hz\": %.6f,\n", p->rf.tuning_offset_hz);
    fprintf(f, "        \"agc_attack_ms\": %.2f,\n",        p->rf.agc_attack_ms);
    fprintf(f, "        \"agc_release_ms\": %.2f,\n",       p->rf.agc_release_ms);
    fprintf(f, "        \"sound_am_rejection_db\": %.2f,\n", p->rf.sound_am_rejection_db);
    fprintf(f, "        \"icpm_deg\": %.2f\n",              p->rf.icpm_deg);
    fprintf(f, "    },\n");

    /* ---- Console supply ---- */
    fprintf(f, "    \"psu\": {\n");
    fprintf(f, "        \"adaptor_vac\": %.2f,\n",          p->psu.adaptor_vac);
    fprintf(f, "        \"reservoir_uf\": %.1f,\n",         p->psu.reservoir_uf);
    fprintf(f, "        \"load_ma\": %.1f,\n",              p->psu.load_ma);
    fprintf(f, "        \"regulator_rejection_db\": %.1f\n", p->psu.regulator_rejection_db);
    fprintf(f, "    },\n");

    /* ---- Signal decode overrides ---- */
    fprintf(f, "    \"brightness\": %.6f,\n",               p->brightness);
    fprintf(f, "    \"contrast\": %.6f,\n",                 p->contrast);
    fprintf(f, "    \"chroma_gain\": %.6f,\n",              p->chroma_gain);

    /* ---- VHS deck (NTSC SP) ---- */
    const VHSParams *v = &p->vhs;
    fprintf(f, "    \"vhs\": {\n        \"enabled\": %s,\n", v->enabled ? "true" : "false");
    fprintf(f, "        \"model\": %d,\n", v->model);
    fprintf(f, "        \"doc\": %s,\n", v->doc ? "true" : "false");
    fprintf(f, "        \"deck_seed\": %d,\n", v->deck_seed);
    const struct { const char *key; float value; } vhs_fields[] = {
        {"white_clip_pct", v->white_clip_pct}, {"dark_clip_pct", v->dark_clip_pct},
        {"fm_sync_hz", v->fm_sync_hz}, {"fm_white_hz", v->fm_white_hz},
        {"rf_cnr_dbhz", v->rf_cnr_dbhz}, {"tape_tilt_db_per_mhz", v->tape_tilt_db_per_mhz},
        {"mod_noise_hz", v->mod_noise_hz}, {"head_b_noise_db", v->head_b_noise_db},
        {"chroma_noise_ire", v->chroma_noise_ire}, {"dropout_scale", v->dropout_scale},
        {"doc_threshold_db", v->doc_threshold_db}, {"canceller_split_hz", v->canceller_split_hz},
        {"canceller_limit_ire", v->canceller_limit_ire}, {"sharpness", v->sharpness},
        {"detail_limit_ire", v->detail_limit_ire}, {"apc_loop_hz", v->apc_loop_hz},
        {"yc_delay_ns", v->yc_delay_ns}, {"bow_scale", v->bow_scale},
        {"tbe_varying_ns", v->tbe_varying_ns}, {"tbe_slow_fraction", v->tbe_slow_fraction},
        {"tbe_slow_tau_ms", v->tbe_slow_tau_ms}, {"line_jitter_ns", v->line_jitter_ns},
        {"switch_lines_before_vsync", v->switch_lines_before_vsync},
        {"skew_ba_ns", v->skew_ba_ns}, {"skew_ab_ns", v->skew_ab_ns},
    };
    const size_t vhs_count = sizeof(vhs_fields) / sizeof(vhs_fields[0]);
    /* The last member has no trailing comma. */
    for (size_t i = 0; i < vhs_count; i++)
        fprintf(f, "        \"%s\": %.6f%s\n", vhs_fields[i].key, vhs_fields[i].value,
                i + 1 < vhs_count ? "," : "");
    fprintf(f, "    },\n");

    fprintf(f, "    \"audio_noise_floor\": %.6f,\n",        p->audio_noise_floor);
    fprintf(f, "    \"audio_saturation_drive\": %.6f,\n",   p->audio_saturation_drive);
    fprintf(f, "    \"audio_cable_length_m\": %.6f,\n",      p->audio_cable_length_m);
    fprintf(f, "    \"audio_pickup_mv\": %.6f,\n",           p->audio_pickup_mv);
    fprintf(f, "    \"audio_tv_input_kohm\": %.6f,\n",       p->audio_tv_input_kohm);
    fprintf(f, "    \"audio_rf_deemphasis\": %d\n",          p->audio_rf_deemphasis);

    fprintf(f, "}\n");

    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* ============================================================================
 * preset_json_load -- read a JSON file into a PhysicalPreset
 * ============================================================================ */

/* Supported preset objects. Unknown objects are validated and ignored. */
typedef enum {
    PJSON_SEC_TOP = 0,
    PJSON_SEC_VIDEO_CABLE,
    PJSON_SEC_AUDIO_CABLE,
    PJSON_SEC_TV,
    PJSON_SEC_RF,
    PJSON_SEC_VHS,
    PJSON_SEC_PSU,
} PresetJsonSection;

/* The field mapping is independent of whitespace and object order. */
static inline void preset_json__assign(PhysicalPreset *p, PresetJsonSection section,
                                       const char *key, char *val)
{
#define MATCH_FLOAT(sec, fname, field) \
    if (section == (sec) && strcmp(key, (fname)) == 0) { (field) = strtof(val, NULL); }
#define MATCH_INT(sec, fname, field) \
    if (section == (sec) && strcmp(key, (fname)) == 0) { (field) = (int)strtol(val, NULL, 10); }
#define MATCH_BOOL(sec, fname, field) \
    if (section == (sec) && strcmp(key, (fname)) == 0) { (field) = (strstr(val, "true") != NULL); }

        /* ---- Top-level fields ---- */
        if (section == PJSON_SEC_TOP && strcmp(key, "name") == 0) {
            char *u = preset_json__unescape(val);
            strncpy(p->name, u, sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';
        }
        else if (section == PJSON_SEC_TOP && strcmp(key, "description") == 0) {
            char *u = preset_json__unescape(val);
            strncpy(p->description, u, sizeof(p->description) - 1);
            p->description[sizeof(p->description) - 1] = '\0';
        }
        /* Enum fields accept both string names ("composite", "1line", ...)
         * and raw integers for backward compat with older preset files. */
        else if (section == PJSON_SEC_TOP && strcmp(key, "connection") == 0) {
            char *u = preset_json__unescape(val);
            p->connection = (VideoConnectionType)preset_json__connection_from(u);
        }
        else if (section == PJSON_SEC_TOP && strcmp(key, "comb_type") == 0) {
            char *u = preset_json__unescape(val);
            p->comb_type = (VideoCombType)preset_json__comb_from(u);
        }
        else MATCH_FLOAT(PJSON_SEC_TOP, "comb_notch_depth", p->comb_notch_depth)
        else if (section == PJSON_SEC_TOP && strcmp(key, "region") == 0) {
            char *u = preset_json__unescape(val);
            p->region = preset_json__region_from(u);
        }
        else if (section == PJSON_SEC_TOP && strcmp(key, "console_variant") == 0) {
            char *u = preset_json__unescape(val);
            char *end; long v = strtol(u, &end, 10);
            p->console_variant = (PresetConsoleVariant)((end != u) ? (int)v : 0);
        }
        else MATCH_INT(PJSON_SEC_TOP, "speaker_type",     p->speaker_type)

        /* Console output stage. */
        else MATCH_FLOAT(PJSON_SEC_TOP, "console_coupling_R", p->console_coupling_R)
        else MATCH_FLOAT(PJSON_SEC_TOP, "console_coupling_C", p->console_coupling_C)
        else MATCH_FLOAT(PJSON_SEC_TOP, "console_amp_bw",     p->console_amp_bw)
        else MATCH_FLOAT(PJSON_SEC_TOP, "console_phase_distortion_ns", p->console_phase_distortion_ns)
        else MATCH_FLOAT(PJSON_SEC_TOP, "console_follower_tau_ns", p->console_follower_tau_ns)
        else MATCH_FLOAT(PJSON_SEC_TOP, "console_psu_hum",    p->console_psu_hum)

        /* Signal decode overrides. */
        else MATCH_FLOAT(PJSON_SEC_TOP, "brightness",    p->brightness)
        else MATCH_FLOAT(PJSON_SEC_TOP, "contrast",      p->contrast)
        else MATCH_FLOAT(PJSON_SEC_TOP, "chroma_gain",   p->chroma_gain)

        /* Audio overrides. */
        else MATCH_FLOAT(PJSON_SEC_TOP, "audio_noise_floor",        p->audio_noise_floor)
        else MATCH_FLOAT(PJSON_SEC_TOP, "audio_saturation_drive",   p->audio_saturation_drive)
        else MATCH_FLOAT(PJSON_SEC_TOP, "audio_cable_length_m",     p->audio_cable_length_m)
        else MATCH_FLOAT(PJSON_SEC_TOP, "audio_pickup_mv",          p->audio_pickup_mv)
        else MATCH_FLOAT(PJSON_SEC_TOP, "audio_tv_input_kohm",      p->audio_tv_input_kohm)
        else MATCH_INT(PJSON_SEC_TOP, "audio_rf_deemphasis",        p->audio_rf_deemphasis)

        /* ---- Video cable ---- */
        else MATCH_FLOAT(PJSON_SEC_VIDEO_CABLE, "length_meters",        p->video_cable.length_meters)
        else MATCH_FLOAT(PJSON_SEC_VIDEO_CABLE, "resistance_per_m",     p->video_cable.resistance_per_m)
        else MATCH_FLOAT(PJSON_SEC_VIDEO_CABLE, "capacitance_per_m",    p->video_cable.capacitance_per_m)
        else MATCH_INT  (PJSON_SEC_VIDEO_CABLE, "num_sections",         p->video_cable.num_sections)
        else MATCH_FLOAT(PJSON_SEC_VIDEO_CABLE, "connector_resistance", p->video_cable.connector_resistance)
        else MATCH_FLOAT(PJSON_SEC_VIDEO_CABLE, "impedance",            p->video_cable.impedance)
        else MATCH_FLOAT(PJSON_SEC_VIDEO_CABLE, "shield_effectiveness", p->video_cable.shield_effectiveness)
        else MATCH_INT  (PJSON_SEC_VIDEO_CABLE, "ghost_delay",          p->video_cable.ghost_delay)
        else MATCH_FLOAT(PJSON_SEC_VIDEO_CABLE, "ghost_level",          p->video_cable.ghost_level)

        /* ---- Audio cable ---- */
        else MATCH_FLOAT(PJSON_SEC_AUDIO_CABLE, "length_meters",        p->audio_cable.length_meters)
        else MATCH_FLOAT(PJSON_SEC_AUDIO_CABLE, "resistance_per_m",     p->audio_cable.resistance_per_m)
        else MATCH_FLOAT(PJSON_SEC_AUDIO_CABLE, "capacitance_per_m",    p->audio_cable.capacitance_per_m)
        else MATCH_INT  (PJSON_SEC_AUDIO_CABLE, "num_sections",         p->audio_cable.num_sections)
        else MATCH_FLOAT(PJSON_SEC_AUDIO_CABLE, "connector_resistance", p->audio_cable.connector_resistance)
        else MATCH_FLOAT(PJSON_SEC_AUDIO_CABLE, "impedance",            p->audio_cable.impedance)
        else MATCH_FLOAT(PJSON_SEC_AUDIO_CABLE, "shield_effectiveness", p->audio_cable.shield_effectiveness)
        else MATCH_INT  (PJSON_SEC_AUDIO_CABLE, "ghost_delay",          p->audio_cable.ghost_delay)
        else MATCH_FLOAT(PJSON_SEC_AUDIO_CABLE, "ghost_level",          p->audio_cable.ghost_level)

        /* ---- TV / CRT display ---- */
        /* Chroma demodulator. */
        else MATCH_FLOAT(PJSON_SEC_TV, "chroma_bandwidth",     p->tv.chroma_bandwidth)
        else MATCH_FLOAT(PJSON_SEC_TV, "chroma_q_bandwidth",   p->tv.chroma_q_bandwidth)
        else MATCH_FLOAT(PJSON_SEC_TV, "luma_bandwidth",       p->tv.luma_bandwidth)
        else MATCH_FLOAT(PJSON_SEC_TV, "hue_offset",           p->tv.hue_offset)
        else MATCH_FLOAT(PJSON_SEC_TV, "saturation",           p->tv.saturation)
        else MATCH_FLOAT(PJSON_SEC_TV, "fir_ringing",          p->tv.fir_ringing)
        else MATCH_FLOAT(PJSON_SEC_TV, "rf_interference",      p->tv.rf_interference)
        else MATCH_FLOAT(PJSON_SEC_TV, "geometry_warp",        p->tv.geometry_warp)
        /* Matrix decode. */
        else MATCH_FLOAT(PJSON_SEC_TV, "color_temperature",    p->tv.color_temperature)
        else MATCH_INT(PJSON_SEC_TV, "monitor_model", p->tv.monitor_model)
        else MATCH_INT(PJSON_SEC_TV, "phosphor_gamut", p->tv.phosphor_gamut)
        else MATCH_INT(PJSON_SEC_TV, "rgb_bandwidth_3db", p->tv.rgb_bandwidth_3db)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_spot_growth", p->tv.beam_spot_growth)
        else MATCH_FLOAT(PJSON_SEC_TV, "decoder_blue_gain", p->tv.decoder_blue_gain)
        else MATCH_FLOAT(PJSON_SEC_TV, "decoder_red_gain", p->tv.decoder_red_gain)
        else MATCH_FLOAT(PJSON_SEC_TV, "r_drive",              p->tv.r_drive)
        else MATCH_FLOAT(PJSON_SEC_TV, "g_drive",              p->tv.g_drive)
        else MATCH_FLOAT(PJSON_SEC_TV, "b_drive",              p->tv.b_drive)
        else MATCH_FLOAT(PJSON_SEC_TV, "r_cutoff",             p->tv.r_cutoff)
        else MATCH_FLOAT(PJSON_SEC_TV, "g_cutoff",             p->tv.g_cutoff)
        else MATCH_FLOAT(PJSON_SEC_TV, "b_cutoff",             p->tv.b_cutoff)
        /* Video amplifier. */
        else MATCH_FLOAT(PJSON_SEC_TV, "r_bandwidth",          p->tv.r_bandwidth)
        else MATCH_FLOAT(PJSON_SEC_TV, "g_bandwidth",          p->tv.g_bandwidth)
        else MATCH_FLOAT(PJSON_SEC_TV, "b_bandwidth",          p->tv.b_bandwidth)
        else MATCH_FLOAT(PJSON_SEC_TV, "gamma",                p->tv.gamma)
        /* Beam. */
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_sharpness",       p->tv.beam_sharpness)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_height_min",      p->tv.beam_height_min)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_height_max",      p->tv.beam_height_max)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_fwhm_min",       p->tv.beam_fwhm_min)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_fwhm_max",       p->tv.beam_fwhm_max)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_spot_size",       p->tv.beam_spot_size)
        else MATCH_FLOAT(PJSON_SEC_TV, "convergence_static",   p->tv.convergence_static)
        else MATCH_FLOAT(PJSON_SEC_TV, "convergence_dynamic",  p->tv.convergence_dynamic)
        else MATCH_FLOAT(PJSON_SEC_TV, "conv_r_x",             p->tv.conv_r_x)
        else MATCH_FLOAT(PJSON_SEC_TV, "conv_r_y",             p->tv.conv_r_y)
        else MATCH_FLOAT(PJSON_SEC_TV, "conv_b_x",             p->tv.conv_b_x)
        else MATCH_FLOAT(PJSON_SEC_TV, "conv_b_y",             p->tv.conv_b_y)
        else MATCH_FLOAT(PJSON_SEC_TV, "h_jitter",             p->tv.h_jitter)
        else MATCH_FLOAT(PJSON_SEC_TV, "v_jitter",             p->tv.v_jitter)
        /* Phosphor. */
        else if (section == PJSON_SEC_TV && strcmp(key, "mask_type") == 0) {
            char *u = preset_json__unescape(val);
            p->tv.mask_type = (VideoMaskType)preset_json__mask_from(u);
        }
        else MATCH_FLOAT(PJSON_SEC_TV, "mask_triads",        p->tv.mask_triads)
        else MATCH_FLOAT(PJSON_SEC_TV, "mask_pitch_px",        p->tv.mask_pitch_px)
        else MATCH_FLOAT(PJSON_SEC_TV, "mask_pitch_mm",        p->tv.mask_pitch_px)
        else MATCH_FLOAT(PJSON_SEC_TV, "mask_strength",        p->tv.mask_strength)
        else MATCH_INT  (PJSON_SEC_TV, "subpixel_layout",      p->tv.subpixel_layout)
        else MATCH_FLOAT(PJSON_SEC_TV, "face_height_mm",       p->tv.face_height_mm)
        else MATCH_INT  (PJSON_SEC_TV, "damper_wires",         p->tv.damper_wires)
        else MATCH_FLOAT(PJSON_SEC_TV, "damper_y1",            p->tv.damper_y1)
        else MATCH_FLOAT(PJSON_SEC_TV, "damper_y2",            p->tv.damper_y2)
        else MATCH_FLOAT(PJSON_SEC_TV, "damper_wire_um",       p->tv.damper_wire_um)
        else MATCH_FLOAT(PJSON_SEC_TV, "persistence_ms",       p->tv.persistence_ms)
        /* Glass. */
        else MATCH_FLOAT(PJSON_SEC_TV, "halation",             p->tv.halation)
        else MATCH_FLOAT(PJSON_SEC_TV, "halation_sigma",       p->tv.halation_sigma)
        else MATCH_FLOAT(PJSON_SEC_TV, "glass_tint",           p->tv.glass_tint)
        else MATCH_FLOAT(PJSON_SEC_TV, "barrel",               p->tv.barrel)
        /* Environment. */
        else MATCH_FLOAT(PJSON_SEC_TV, "vignette",             p->tv.vignette)
        else MATCH_FLOAT(PJSON_SEC_TV, "ambient_light",        p->tv.ambient_light)
        /* Black level. */
        else MATCH_FLOAT(PJSON_SEC_TV, "black_floor",          p->tv.black_floor)
        /* Noise. */
        else MATCH_FLOAT(PJSON_SEC_TV, "noise_level",          p->tv.noise_level)
        /* Mains hum. */
        else MATCH_FLOAT(PJSON_SEC_TV, "hum_bar_amplitude",    p->tv.hum_bar_amplitude)
        /* Beam bloom. */
        else MATCH_FLOAT(PJSON_SEC_TV, "bloom_gamma",          p->tv.bloom_gamma)
        /* Beam physics. */
        else MATCH_FLOAT(PJSON_SEC_TV, "edge_focus",           p->tv.edge_focus)
        else MATCH_FLOAT(PJSON_SEC_TV, "velocity_dim",         p->tv.velocity_dim)
        else MATCH_FLOAT(PJSON_SEC_TV, "halation_tint_r",          p->tv.halation_tint_r)
        else MATCH_FLOAT(PJSON_SEC_TV, "halation_tint_g",          p->tv.halation_tint_g)
        else MATCH_FLOAT(PJSON_SEC_TV, "halation_tint_b",          p->tv.halation_tint_b)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_current_load",        p->tv.beam_current_load)
        else MATCH_FLOAT(PJSON_SEC_TV, "video_black_droop",        p->tv.video_black_droop)
        else MATCH_FLOAT(PJSON_SEC_TV, "video_recovery_us",        p->tv.video_recovery_us)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_edge_fade",           p->tv.beam_edge_fade)
        else MATCH_FLOAT(PJSON_SEC_TV, "beam_edge_overshoot",      p->tv.beam_edge_overshoot)
        else MATCH_FLOAT(PJSON_SEC_TV, "burst_lock_drift",         p->tv.burst_lock_drift)
        else MATCH_FLOAT(PJSON_SEC_TV, "burst_lock_drift_width",   p->tv.burst_lock_drift_width)
        /* Barrel distortion. */
        else MATCH_FLOAT(PJSON_SEC_TV, "barrel_v",             p->tv.barrel_v)
        /* Motion-adaptive comb. */
        else MATCH_FLOAT(PJSON_SEC_TV, "motion_threshold",     p->tv.motion_threshold)
        else MATCH_FLOAT(PJSON_SEC_TV, "persistence_tail_ms", p->tv.persistence_tail_ms)
        else MATCH_FLOAT(PJSON_SEC_TV, "persistence_tail_weight", p->tv.persistence_tail_weight)
        /* Per-channel persistence. */
        else MATCH_FLOAT(PJSON_SEC_TV, "persistence_r",        p->tv.persistence_r)
        else MATCH_FLOAT(PJSON_SEC_TV, "persistence_g",        p->tv.persistence_g)
        else MATCH_FLOAT(PJSON_SEC_TV, "persistence_b",        p->tv.persistence_b)
        /* Colour killer. */
        else MATCH_FLOAT(PJSON_SEC_TV, "color_killer",         p->tv.color_killer)
        /* HDR gain. */
        else MATCH_FLOAT(PJSON_SEC_TV, "hdr_gain",             p->tv.hdr_gain)
        /* Luma peaking / sharpness. */
        else MATCH_FLOAT(PJSON_SEC_TV, "luma_peaking",         p->tv.luma_peaking)
        else MATCH_FLOAT(PJSON_SEC_TV, "h_afc_tau_ms",         p->tv.h_afc_tau_ms)
        else MATCH_FLOAT(PJSON_SEC_TV, "h_pll_hz",             p->tv.h_pll_hz)
        else MATCH_FLOAT(PJSON_SEC_TV, "h_pll_damping",        p->tv.h_pll_damping)
        else MATCH_FLOAT(PJSON_SEC_TV, "h_pll_vblank_gain",    p->tv.h_pll_vblank_gain)
        else MATCH_FLOAT(PJSON_SEC_TV, "aperture_max_db",         p->tv.aperture_max_db)
        else MATCH_FLOAT(PJSON_SEC_TV, "luma_notch_depth",     p->tv.luma_notch_depth)
        /* Overscan / bezel crop. */
        else MATCH_FLOAT(PJSON_SEC_TV, "overscan",             p->tv.overscan)
        /* Additional geometry distortions. */
        else MATCH_FLOAT(PJSON_SEC_TV, "keystone",             p->tv.keystone)
        else MATCH_FLOAT(PJSON_SEC_TV, "rotation",             p->tv.rotation)
        else MATCH_FLOAT(PJSON_SEC_TV, "skew_x",               p->tv.skew_x)
        else MATCH_FLOAT(PJSON_SEC_TV, "skew_y",               p->tv.skew_y)
        else MATCH_FLOAT(PJSON_SEC_TV, "hv_sag",               p->tv.hv_sag)
        /* PSU-driven beam instability. */
        else MATCH_FLOAT(PJSON_SEC_TV, "focus_breathing",      p->tv.focus_breathing)
        else MATCH_FLOAT(PJSON_SEC_TV, "scanline_wobble",      p->tv.scanline_wobble)
        else MATCH_FLOAT(PJSON_SEC_TV, "top_band_shift",       p->tv.top_band_shift)
        else MATCH_FLOAT(PJSON_SEC_TV, "top_edge_skew",        p->tv.top_edge_skew)
        else MATCH_FLOAT(PJSON_SEC_TV, "top_band_start",       p->tv.top_band_start)
        else MATCH_FLOAT(PJSON_SEC_TV, "top_band_end",         p->tv.top_band_end)
        else MATCH_FLOAT(PJSON_SEC_TV, "top_edge_width",       p->tv.top_edge_width)
        /* Service-menu raster geometry (HPOS/VPOS/HSIZE/VSIZE). */
        else MATCH_FLOAT(PJSON_SEC_TV, "h_pos",                p->tv.h_pos)
        else MATCH_FLOAT(PJSON_SEC_TV, "v_pos",                p->tv.v_pos)
        else MATCH_FLOAT(PJSON_SEC_TV, "h_size",               p->tv.h_size)
        else MATCH_FLOAT(PJSON_SEC_TV, "v_size",               p->tv.v_size)
        else MATCH_FLOAT(PJSON_SEC_TV, "phosphor_gamma_offset_r", p->tv.phosphor_gamma_offset_r)
        else MATCH_FLOAT(PJSON_SEC_TV, "phosphor_gamma_offset_g", p->tv.phosphor_gamma_offset_g)
        else MATCH_FLOAT(PJSON_SEC_TV, "phosphor_gamma_offset_b", p->tv.phosphor_gamma_offset_b)
        else MATCH_FLOAT(PJSON_SEC_TV, "secondary_scatter",       p->tv.secondary_scatter)
        else MATCH_FLOAT(PJSON_SEC_TV, "glass_reflection",        p->tv.glass_reflection)
        else MATCH_FLOAT(PJSON_SEC_TV, "antiglare_blur",          p->tv.antiglare_blur)
        else MATCH_FLOAT(PJSON_SEC_TV, "emi_gradient",            p->tv.emi_gradient)
        else MATCH_FLOAT(PJSON_SEC_TV, "degauss_tint",            p->tv.degauss_tint)
        else MATCH_FLOAT(PJSON_SEC_TV, "phosphor_grain",          p->tv.phosphor_grain)
        else MATCH_FLOAT(PJSON_SEC_TV, "cathode_center_dim",      p->tv.cathode_center_dim)
        else MATCH_FLOAT(PJSON_SEC_TV, "cathode_gain_r",          p->tv.cathode_gain_r)
        else MATCH_FLOAT(PJSON_SEC_TV, "cathode_gain_g",          p->tv.cathode_gain_g)
        else MATCH_FLOAT(PJSON_SEC_TV, "cathode_gain_b",          p->tv.cathode_gain_b)
        else MATCH_FLOAT(PJSON_SEC_TV, "apl_black_lift",          p->tv.apl_black_lift)
        else MATCH_FLOAT(PJSON_SEC_TV, "thermal_dome_amount",     p->tv.thermal_dome_amount)
        else MATCH_FLOAT(PJSON_SEC_TV, "corner_astigmatism",      p->tv.corner_astigmatism)
        else MATCH_FLOAT(PJSON_SEC_TV, "chromaticity_drive_shift", p->tv.chromaticity_drive_shift)
        else MATCH_FLOAT(PJSON_SEC_TV, "velocity_mod",            p->tv.velocity_mod)
        else MATCH_FLOAT(PJSON_SEC_TV, "asym_rise_fall",          p->tv.asym_rise_fall)
        else MATCH_FLOAT(PJSON_SEC_TV, "vertical_smear",          p->tv.vertical_smear)
        else MATCH_FLOAT(PJSON_SEC_TV, "microphonic_amount",      p->tv.microphonic_amount)
        else MATCH_FLOAT(PJSON_SEC_TV, "glass_glare",             p->tv.glass_glare)
        else MATCH_FLOAT(PJSON_SEC_TV, "glass_glare_light_x",     p->tv.glass_glare_light_x)
        else MATCH_FLOAT(PJSON_SEC_TV, "glass_glare_light_y",     p->tv.glass_glare_light_y)
        else MATCH_FLOAT(PJSON_SEC_TV, "glass_glare_size",        p->tv.glass_glare_size)
        else MATCH_FLOAT(PJSON_SEC_TV, "glass_glare_temp_k",      p->tv.glass_glare_temp_k)

        /* VHS toggles accept the earlier numeric spelling too. The model
         * number marks the file format, not a setting: pre-2 blocks are
         * replaced with the deck defaults at apply time. */
        else if (section == PJSON_SEC_VHS && strcmp(key, "enabled") == 0) {
            p->vhs.enabled = strcmp(val, "true") == 0 || strtol(val, NULL, 10) != 0;
        }
        else if (section == PJSON_SEC_VHS && strcmp(key, "doc") == 0) {
            p->vhs.doc = strcmp(val, "true") == 0 || strtol(val, NULL, 10) != 0;
        }
        else if (section == PJSON_SEC_VHS && strcmp(key, "model") == 0) {
            p->vhs.model = (int)strtol(val, NULL, 10);
        }
        else MATCH_INT  (PJSON_SEC_VHS, "deck_seed",            p->vhs.deck_seed)
        else MATCH_FLOAT(PJSON_SEC_VHS, "white_clip_pct",       p->vhs.white_clip_pct)
        else MATCH_FLOAT(PJSON_SEC_VHS, "dark_clip_pct",        p->vhs.dark_clip_pct)
        else MATCH_FLOAT(PJSON_SEC_VHS, "fm_sync_hz",           p->vhs.fm_sync_hz)
        else MATCH_FLOAT(PJSON_SEC_VHS, "fm_white_hz",          p->vhs.fm_white_hz)
        else MATCH_FLOAT(PJSON_SEC_VHS, "rf_cnr_dbhz",          p->vhs.rf_cnr_dbhz)
        else MATCH_FLOAT(PJSON_SEC_VHS, "tape_tilt_db_per_mhz", p->vhs.tape_tilt_db_per_mhz)
        else MATCH_FLOAT(PJSON_SEC_VHS, "mod_noise_hz",         p->vhs.mod_noise_hz)
        else MATCH_FLOAT(PJSON_SEC_VHS, "head_b_noise_db",      p->vhs.head_b_noise_db)
        else MATCH_FLOAT(PJSON_SEC_VHS, "chroma_noise_ire",     p->vhs.chroma_noise_ire)
        else MATCH_FLOAT(PJSON_SEC_VHS, "dropout_scale",        p->vhs.dropout_scale)
        else MATCH_FLOAT(PJSON_SEC_VHS, "doc_threshold_db",     p->vhs.doc_threshold_db)
        else MATCH_FLOAT(PJSON_SEC_VHS, "canceller_split_hz",   p->vhs.canceller_split_hz)
        else MATCH_FLOAT(PJSON_SEC_VHS, "canceller_limit_ire",  p->vhs.canceller_limit_ire)
        else MATCH_FLOAT(PJSON_SEC_VHS, "sharpness",            p->vhs.sharpness)
        else MATCH_FLOAT(PJSON_SEC_VHS, "detail_limit_ire",     p->vhs.detail_limit_ire)
        else MATCH_FLOAT(PJSON_SEC_VHS, "apc_loop_hz",          p->vhs.apc_loop_hz)
        else MATCH_FLOAT(PJSON_SEC_VHS, "yc_delay_ns",          p->vhs.yc_delay_ns)
        else MATCH_FLOAT(PJSON_SEC_VHS, "bow_scale",            p->vhs.bow_scale)
        else MATCH_FLOAT(PJSON_SEC_VHS, "tbe_varying_ns",       p->vhs.tbe_varying_ns)
        else MATCH_FLOAT(PJSON_SEC_VHS, "tbe_slow_fraction",    p->vhs.tbe_slow_fraction)
        else MATCH_FLOAT(PJSON_SEC_VHS, "tbe_slow_tau_ms",      p->vhs.tbe_slow_tau_ms)
        else MATCH_FLOAT(PJSON_SEC_VHS, "line_jitter_ns",       p->vhs.line_jitter_ns)
        else MATCH_FLOAT(PJSON_SEC_VHS, "switch_lines_before_vsync", p->vhs.switch_lines_before_vsync)
        else MATCH_FLOAT(PJSON_SEC_VHS, "skew_ba_ns",           p->vhs.skew_ba_ns)
        else MATCH_FLOAT(PJSON_SEC_VHS, "skew_ab_ns",           p->vhs.skew_ab_ns)

        /* ---- RF modulator ---- */
        else MATCH_BOOL (PJSON_SEC_RF, "enabled",              p->rf.enabled)
        else MATCH_FLOAT(PJSON_SEC_RF, "carrier_freq",         p->rf.carrier_freq)
        else MATCH_FLOAT(PJSON_SEC_RF, "carrier_level_dbm", p->rf.carrier_level_dbm)
        else MATCH_FLOAT(PJSON_SEC_RF, "mod_bandwidth",        p->rf.mod_bandwidth)
        else MATCH_FLOAT(PJSON_SEC_RF, "noise_floor_dbm",      p->rf.noise_floor_dbm)
        else MATCH_FLOAT(PJSON_SEC_RF, "if_asymmetry", p->rf.if_asymmetry)
        else MATCH_FLOAT(PJSON_SEC_RF, "tuning_offset_hz", p->rf.tuning_offset_hz)
        else MATCH_FLOAT(PJSON_SEC_RF, "agc_attack_ms",        p->rf.agc_attack_ms)
        else MATCH_FLOAT(PJSON_SEC_RF, "agc_release_ms",       p->rf.agc_release_ms)
        else MATCH_FLOAT(PJSON_SEC_RF, "sound_am_rejection_db", p->rf.sound_am_rejection_db)
        else MATCH_FLOAT(PJSON_SEC_RF, "icpm_deg",             p->rf.icpm_deg)

        /* ---- Console supply ---- */
        else MATCH_FLOAT(PJSON_SEC_PSU, "adaptor_vac",            p->psu.adaptor_vac)
        else MATCH_FLOAT(PJSON_SEC_PSU, "reservoir_uf",           p->psu.reservoir_uf)
        else MATCH_FLOAT(PJSON_SEC_PSU, "load_ma",                p->psu.load_ma)
        else MATCH_FLOAT(PJSON_SEC_PSU, "regulator_rejection_db", p->psu.regulator_rejection_db)

#undef MATCH_FLOAT
#undef MATCH_INT
#undef MATCH_BOOL
}

static inline void preset_json__space(char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' || **cursor == '\n') ++*cursor;
}

/* Return the end of one JSON string, leaving its quotes intact for the
 * existing field decoder. Escaped punctuation never acts as structure. */
static inline char *preset_json__string_end(char *s)
{
    if (*s++ != '"') return NULL;
    while (*s) {
        if (*s == '"') return s + 1;
        if ((unsigned char)*s < 32) return NULL;
        if (*s++ == '\\') {
            if (!*s) return NULL;
            if (*s == 'u') {
                ++s;
                for (int i=0; i<4; ++i, ++s)
                    if (!((*s>='0' && *s<='9') || (*s>='a' && *s<='f') || (*s>='A' && *s<='F'))) return NULL;
            } else {
                if (!strchr("\"\\/bfnrt", *s)) return NULL;
                ++s;
            }
        }
    }
    return NULL;
}

static inline int preset_json__section(const char *key)
{
    if (!strcmp(key,"video_cable")) return PJSON_SEC_VIDEO_CABLE;
    if (!strcmp(key,"audio_cable")) return PJSON_SEC_AUDIO_CABLE;
    if (!strcmp(key,"tv")) return PJSON_SEC_TV;
    if (!strcmp(key,"rf")) return PJSON_SEC_RF;
    if (!strcmp(key,"vhs")) return PJSON_SEC_VHS;
    if (!strcmp(key,"psu")) return PJSON_SEC_PSU;
    return -1;
}

/* Validate nested unknown properties too, so additions can be ignored without
 * accidentally applying their fields to the surrounding preset section. */
static inline bool preset_json__value(char **cursor, PhysicalPreset *p,
                                      int section, const char *key, int depth)
{
    if (depth > 16) return false;
    preset_json__space(cursor);
    char *s=*cursor;
    if (*s=='{' || *s=='[') {
        bool object=*s=='{'; char close=object ? '}' : ']';
        int child_section=depth==0 ? PJSON_SEC_TOP :
            (depth==1 && object && section==PJSON_SEC_TOP ? preset_json__section(key) : -1);
        *cursor=s+1; preset_json__space(cursor);
        if (**cursor==close) { ++*cursor; return true; }
        for (;;) {
            char member[128]="";
            if (object) {
                s=*cursor; char *end=preset_json__string_end(s);
                if (!end) return false;
                char saved=*end; *end=0;
                const char *decoded=preset_json__unescape(s);
                snprintf(member,sizeof(member),"%s",decoded);
                *end=saved; *cursor=end; preset_json__space(cursor);
                if (**cursor!=':') return false;
                ++*cursor;
            }
            if (!preset_json__value(cursor,p,object ? child_section : -1,member,depth+1)) return false;
            preset_json__space(cursor);
            if (**cursor==close) { ++*cursor; return true; }
            if (**cursor!=',') return false;
            ++*cursor; preset_json__space(cursor);
        }
    }
    char *end=NULL;
    if (*s=='"') end=preset_json__string_end(s);
    else if (!strncmp(s,"true",4)) end=s+4;
    else if (!strncmp(s,"false",5)) end=s+5;
    else if (!strncmp(s,"null",4)) end=s+4;
    else {
        end=s;
        if (*end=='-') ++end;
        if (*end=='0') ++end;
        else {
            if (*end<'1' || *end>'9') return false;
            while (*end>='0' && *end<='9') ++end;
        }
        if (*end=='.') {
            ++end;
            if (*end<'0' || *end>'9') return false;
            while (*end>='0' && *end<='9') ++end;
        }
        if (*end=='e' || *end=='E') {
            ++end; if (*end=='+' || *end=='-') ++end;
            if (*end<'0' || *end>'9') return false;
            while (*end>='0' && *end<='9') ++end;
        }
        if (!isfinite(strtof(s,NULL))) return false;
    }
    if (!end) return false;
    char saved=*end; *end=0;
    if (section>=0 && strcmp(s,"null")) preset_json__assign(p,(PresetJsonSection)section,key,s);
    *end=saved; *cursor=end;
    return true;
}

static inline bool preset_json_load(PhysicalPreset *p, const char *path)
{
    if (!p || !path) return false;
    FILE *f=fopen(path,"rb");
    if (!f) return false;
    if (fseek(f,0,SEEK_END)) { fclose(f); return false; }
    long size=ftell(f);
    if (size<=0 || size>1024*1024 || fseek(f,0,SEEK_SET)) { fclose(f); return false; }
    char *buf=malloc((size_t)size+1);
    if (!buf) { fclose(f); return false; }
    size_t read=fread(buf,1,(size_t)size,f);
    fclose(f); buf[read]=0;
    PhysicalPreset parsed={0};
    parsed.chroma_gain=1;
    parsed.tv.luma_notch_depth=.95f;
    parsed.tv.h_size=parsed.tv.v_size=1;
    parsed.tv.top_band_start=18;
    parsed.tv.top_band_end=34;
    parsed.tv.top_edge_width=.08f;
    char *cursor=buf;
    preset_json__space(&cursor);
    bool ok=read==(size_t)size && strlen(buf)==read && *cursor=='{' &&
        preset_json__value(&cursor,&parsed,PJSON_SEC_TOP,"",0);
    preset_json__space(&cursor);
    ok=ok && !*cursor;
    if (ok) *p=parsed;  /* An invalid file must not corrupt a live preset. */
    free(buf);
    return ok;
}

/* ============================================================================
 * preset_json_scan_dir -- list .json preset files in a directory
 * ============================================================================ */

static inline int preset_json_scan_dir(const char *dir,
                                       char names[][128],
                                       char paths[][512],
                                       int max_count)
{
    if (!dir || !names || !paths || max_count <= 0) return 0;

    DIR *d = opendir(dir);
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < max_count) {
        const char *fname = ent->d_name;
        size_t len = strlen(fname);
        if (len < 6) continue; /* minimum: "x.json" */
        if (strcmp(fname + len - 5, ".json") != 0) continue;

        /* Fill name (without .json extension). */
        size_t name_len = len - 5;
        if (name_len >= 128) name_len = 127;
        memcpy(names[count], fname, name_len);
        names[count][name_len] = '\0';

        /* Fill full path; a name that does not fit cannot be opened later. */
        if (snprintf(paths[count], 512, "%s/%s", dir, fname) >= 512) continue;

        count++;
    }

    closedir(d);
    return count;
}

#endif /* PRESET_JSON_H */
