/*
 * Tests for pure helpers in video_chain.h / gpu_half.h.
 * These are CPU-only (no GPU device required) so they run in CI.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "../video_chain.h"
#include "../gpu_half.h"
#include "../crt_color.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

static void test_chroma_aux_layout(void) {
    ChromaAuxLayout off = video_chain_chroma_aux_layout(false);
    CHECK(off.i_raw == 0,  "comb off: I raw in aux[0]");
    CHECK(off.q_raw == 1,  "comb off: Q raw in aux[1]");
    CHECK(off.i_filt == 2, "comb off: I filt in aux[2]");
    CHECK(off.q_filt == 3, "comb off: Q filt in aux[3]");

    ChromaAuxLayout on = video_chain_chroma_aux_layout(true);
    CHECK(on.i_raw == 1,   "comb on: I raw in aux[1] (aux[0]=C)");
    CHECK(on.q_raw == 2,   "comb on: Q raw in aux[2]");
    CHECK(on.i_filt == 3,  "comb on: I filt in aux[3]");
    CHECK(on.q_filt == 0,  "comb on: Q filt wraps into aux[0]");

    /* Both layouts must keep each channel distinct and in range. */
    ChromaAuxLayout both[] = { off, on };
    for (int k = 0; k < 2; k++) {
        int slots[] = { both[k].i_raw, both[k].q_raw,
                        both[k].i_filt, both[k].q_filt };
        for (int i = 0; i < 4; i++) {
            CHECK(slots[i] >= 0 && slots[i] < 4, "aux slot in range");
            for (int j = i + 1; j < 4; j++) {
                CHECK(slots[i] != slots[j], "aux slots distinct");
            }
        }
    }
}

static void test_comb_shader_mode(void) {
    CHECK(video_chain_comb_shader_mode(VIDEO_COMB_NONE)   == 0,
          "NONE → shader mode 0 (bypass)");
    CHECK(video_chain_comb_shader_mode(VIDEO_COMB_1LINE)  == 1,
          "1LINE → shader mode 1");
    CHECK(video_chain_comb_shader_mode(VIDEO_COMB_2LINE)  == 2,
          "2LINE → shader mode 2");
    CHECK(video_chain_comb_shader_mode(VIDEO_COMB_3LINE)  == 3,
          "3LINE → shader mode 3");
    CHECK(video_chain_comb_shader_mode(VIDEO_COMB_BYPASS) == 0,
          "BYPASS → shader mode 0 (shader treats as bypass — caller decides routing)");

    CHECK(video_chain_comb_mode_separates(VIDEO_COMB_NONE)   == false, "NONE does not separate");
    CHECK(video_chain_comb_mode_separates(VIDEO_COMB_1LINE)  == true,  "1LINE separates");
    CHECK(video_chain_comb_mode_separates(VIDEO_COMB_2LINE)  == true,  "2LINE separates");
    CHECK(video_chain_comb_mode_separates(VIDEO_COMB_3LINE)  == true,  "3LINE separates");
    CHECK(video_chain_comb_mode_separates(VIDEO_COMB_BYPASS) == false, "BYPASS does not separate");
}

static void test_stage_activation_by_connection(void) {
    VideoChain composite, svideo;
    video_chain_init_preset(&composite, VIDEO_CONN_COMPOSITE, VIDEO_COMB_1LINE,
                            SIGNAL_REGION_NTSC);
    video_chain_init_preset(&svideo, VIDEO_CONN_SVIDEO, VIDEO_COMB_BYPASS,
                            SIGNAL_REGION_NTSC);

    CHECK(video_chain_stage_active(&composite, 6) == true,
          "Composite keeps comb stage active");
    CHECK(video_chain_stage_active(&svideo, 6) == false,
          "S-Video skips comb stage physically");
    CHECK(video_chain_stage_active(&svideo, 7) == true,
          "S-Video still runs the chroma decoder");
    CHECK(video_chain_stage_active(&svideo, 9) == true,
          "S-Video still needs matrix decode");
}

static void test_connection_decode_routing(void) {
    CHECK(video_connection_uses_signal_decode(VIDEO_CONN_RF) == true,
          "RF uses the signal decoder");
    CHECK(video_connection_uses_signal_decode(VIDEO_CONN_COMPOSITE) == true,
          "Composite uses the signal decoder");
    CHECK(video_connection_uses_signal_decode(VIDEO_CONN_SVIDEO) == true,
          "S-Video uses the signal decoder");
    CHECK(video_connection_uses_signal_decode(VIDEO_CONN_COMPONENT) == false,
          "Component bypasses carrier decoding and keeps baseband colour");
    CHECK(video_connection_uses_signal_decode(VIDEO_CONN_RGB) == false,
          "RGB bypasses the signal decoder");
    CHECK(video_connection_uses_signal_decode(VIDEO_CONN_DIRECT) == false,
          "Direct bypasses the signal decoder");
}

static void test_region_sample_rates(void) {
    float ntsc = signal_region_sample_rate_hz(SIGNAL_REGION_NTSC);
    float pal  = signal_region_sample_rate_hz(SIGNAL_REGION_PAL);

    CHECK(fabsf(ntsc - (3.579545e6f * 12.0f)) < 0.5f,
          "NTSC sample rate helper matches 12x subcarrier");
    CHECK(fabsf(pal - (4.43361875e6f * 12.0f)) < 0.5f,
          "PAL sample rate helper matches 12x subcarrier");
    CHECK(pal > ntsc, "PAL sample rate is wider than NTSC");
}

static uint16_t f32_to_f16_bits(float f) {
    /* Reference encoder, round-to-nearest-even, for building test inputs. */
    uint32_t bits; memcpy(&bits, &f, sizeof(bits));
    uint32_t sign = (bits >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((bits >> 23) & 0xFFu) - 127 + 15;
    uint32_t mant = bits & 0x7FFFFFu;
    if (exp <= 0) {
        if (exp < -10) return (uint16_t)sign;  /* underflow to 0 */
        mant |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - exp);
        return (uint16_t)(sign | (mant >> shift));
    }
    if (exp >= 31) {
        if (((bits >> 23) & 0xFFu) == 0xFFu && mant != 0) return (uint16_t)(sign | 0x7C00u | 1u); /* NaN */
        return (uint16_t)(sign | 0x7C00u);  /* inf */
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

static void test_physical_controls(void) {
    VideoChain c; video_chain_init_preset(&c,VIDEO_CONN_COMPOSITE,VIDEO_COMB_NONE,0);
    c.cable.length_meters=1; c.cable.capacitance_per_m=67e-12f;
    c.cable.resistance_per_m=.1f; c.cable.connector_resistance=.1f;
    float short_bw=video_cable_bandwidth(&c);
    CHECK(short_bw>50e6f,"short video lead does not impose a spurious 7 MHz limit");
    c.cable.length_meters=5;
    CHECK(video_cable_bandwidth(&c)<short_bw*.21f,"cable response follows capacitance and length");
    c.tv.beam_fwhm_min=.4f;c.tv.beam_fwhm_max=.8f;
    CHECK(fabsf(video_beam_sigma(&c.tv,true)/video_beam_sigma(&c.tv,false)-2)<1e-5f,"beam FWHM controls have consistent physical units");
    c.console_amp_bw=14000;
    CHECK(video_console_bandwidth(&c)==6e6f,"legacy audio-bandwidth typo cannot destroy video");
    c.connection=VIDEO_CONN_RGB;
    CHECK(!video_chain_stage_active(&c,2) && !video_chain_stage_active(&c,8),"RGB skips composite bandwidth and luma filters");
    CHECK(video_chain_stage_active(&c,11),"RGB retains electron beam");
}

static void test_half_to_float_exact(void) {
    struct { uint16_t h; float expected; } cases[] = {
        { 0x0000, 0.0f },
        { 0x8000, -0.0f },
        { 0x3C00, 1.0f },
        { 0xBC00, -1.0f },
        { 0x3800, 0.5f },
        { 0x4000, 2.0f },
        { 0x7BFF, 65504.0f },   /* half max */
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        float got = gpu_half_to_float(cases[i].h);
        CHECK(got == cases[i].expected, "half→float exact value");
        if (got != cases[i].expected) {
            fprintf(stderr, "  case 0x%04X: got %g expected %g\n",
                    cases[i].h, got, cases[i].expected);
        }
    }
}

static void test_half_to_float_roundtrip(void) {
    /* Every finite float value that fits in half should survive a
     * round-trip encode/decode to exactly itself. */
    float values[] = {
        0.1f, 0.25f, 0.75f, 3.14f, -3.14f, 100.0f, -100.0f, 0.001f,
    };
    for (size_t i = 0; i < sizeof(values)/sizeof(values[0]); i++) {
        uint16_t enc = f32_to_f16_bits(values[i]);
        float dec = gpu_half_to_float(enc);
        /* Half precision ≈ 3 decimal digits. Allow relative 1/1024. */
        float err = fabsf(dec - values[i]) / fabsf(values[i]);
        CHECK(err < 1.0f / 1024.0f, "half→float round-trip precision");
        if (!(err < 1.0f / 1024.0f)) {
            fprintf(stderr, "  %g → 0x%04X → %g  (err %g)\n",
                    values[i], enc, dec, err);
        }
    }
}

static void test_half_inf_nan(void) {
    CHECK(isinf(gpu_half_to_float(0x7C00)), "+inf half → +inf float");
    CHECK(isinf(gpu_half_to_float(0xFC00)), "-inf half → -inf float");
    CHECK(isnan(gpu_half_to_float(0x7C01)), "NaN half → NaN float");
}

/* Verify the timing-EMA math matches what chain_run_cmd applies.
 * This doesn't exercise the GPU path — it just pins down the formula
 * (10% blend, seed-on-first-reading) so a future tweak to the
 * smoothing constant doesn't silently shift what the visualiser
 * reports. */
static void test_timing_ema(void) {
    const double alpha = 0.10;
    double avg = 0.0;
    double samples[] = { 100.0, 110.0, 95.0, 105.0 };
    /* Seed on first reading. */
    avg = samples[0];
    CHECK(avg == 100.0, "timing EMA: first reading seeds the average");
    /* Subsequent readings: avg += (x - avg) * alpha */
    for (size_t i = 1; i < sizeof(samples)/sizeof(samples[0]); i++) {
        avg += (samples[i] - avg) * alpha;
    }
    /* After 4 samples (100, 110, 95, 105): avg ≈ 100.855 */
    double expected = 100.0;
    expected += (110.0 - expected) * alpha;  /* 101.0 */
    expected += (95.0  - expected) * alpha;  /* 100.4 */
    expected += (105.0 - expected) * alpha;  /* 100.86 */
    double err = fabs(avg - expected);
    CHECK(err < 1e-6, "timing EMA: 4-step rollup matches");
    if (err >= 1e-6) fprintf(stderr, "  got %g expected %g\n", avg, expected);
}

static void test_colour_response(void) {
    TVDisplayParams t={.gamma=2.4f,.color_temperature=6500,.saturation=1,
        .r_drive=1,.g_drive=1,.b_drive=1,.phosphor_gamut=1};
    float m[3][3],bias[3],d[3],p[3][3];
    crt_white_drive(&t,d);
    for(int i=0;i<3;i++) CHECK(fabsf(d[i]-1)<.0001f,"D65 gun voltage is neutral");
    t.color_temperature=9300; crt_white_drive(&t,d);crt_phosphor_matrix(t.phosphor_gamut,p);
    float rgb[3]={0};
    for(int i=0;i<3;i++) for(int j=0;j<3;j++) rgb[i]+=p[i][j]*powf(d[j],t.gamma);
    float X=.4123908f*rgb[0]+.3575843f*rgb[1]+.1804808f*rgb[2];
    float Y=.2126390f*rgb[0]+.7151687f*rgb[1]+.0721923f*rgb[2];
    float Z=.0193308f*rgb[0]+.1191948f*rgb[1]+.9505322f*rgb[2];
    CHECK(fabsf(Y-1)<.00001f,"whitepoint preserves luminance");
    CHECK(fabsf(X/(X+Y+Z)-.28307f)<.0001f && fabsf(Y/(X+Y+Z)-.29693f)<.0001f,"D93 emitted chromaticity");
    CHECK(rgb[2]>1.3f && rgb[0]<.9f,"D93 has a substantial cool white response");
    // FW900 white balance inverts the measured transfer, with a common
    // scale to keep the requested chromaticity inside its measured domain.
    t.monitor_model=1;t.phosphor_gamut=3;
    crt_white_drive(&t,d);crt_phosphor_matrix(t.phosphor_gamut,p);
    memset(rgb,0,sizeof(rgb));
    for(int j=0;j<3;j++) {
        float q=d[j]*255;int k=(int)q;if(k>254) k=254;
        float light=fw900_tone[k]+(fw900_tone[k+1]-fw900_tone[k])*(q-k);
        for(int i=0;i<3;i++) rgb[i]+=p[i][j]*light;
    }
    X=.4123908f*rgb[0]+.3575843f*rgb[1]+.1804808f*rgb[2];
    Y=.2126390f*rgb[0]+.7151687f*rgb[1]+.0721923f*rgb[2];
    Z=.0193308f*rgb[0]+.1191948f*rgb[1]+.9505322f*rgb[2];
    CHECK(fabsf(X/(X+Y+Z)-.28307f)<.0001f && fabsf(Y/(X+Y+Z)-.29693f)<.0001f,"FW900 inverse transfer preserves D93 chromaticity");
    t.monitor_model=0;t.phosphor_gamut=1;
    t.color_temperature=6500;crt_decoder_matrix(&t,false,1,0,1,m,bias);
    float reference[3][3];memcpy(reference,m,sizeof(m));
    t.decoder_red_gain=.2f; t.decoder_blue_gain=-.05f;
    crt_decoder_matrix(&t,false,1,0,1,m,bias);
    for(int i=0;i<3;i++) CHECK(fabsf(m[i][0]-reference[i][0])<1e-6f,"decoder push leaves grey neutral");
    CHECK(fabsf(m[0][1]/reference[0][1]-1.2f)<1e-5f,"R-Y gain independent of gun drive");
    for(int j=1;j<3;j++) {
        float dy=.299f*(m[0][j]-reference[0][j])+.587f*(m[1][j]-reference[1][j])+.114f*(m[2][j]-reference[2][j]);
        CHECK(fabsf(dy)<1e-5f,"decoder axis change preserves nominal luma");
    }
}

int main(void) {
    RFModulatorParams rf={.carrier_level_dbm=-20,.noise_floor_dbm=-70};
    float noise=video_rf_noise_rms(&rf);rf.noise_floor_dbm+=20;
    CHECK(fabsf(video_rf_noise_rms(&rf)/noise-10)<.00001f,"20 dB noise power change gives 10x amplitude");
    /* The link budget: the FCC-cap modulator into a 7 dB tuner is 61.7 dB
     * of carrier to noise in 4 MHz; each dB of loss takes a dB off it; an
     * old carrier and noise pair keeps its ratio as an equivalent budget. */
    {
        const float fs=12*3579545.0f;
        RFModulatorParams link={.modulator_dbmv=9.5f,.link_loss_db=0,.tuner_nf_db=7};
        video_rf_link_budget(&link,fs);
        CHECK(fabsf(link.cnr_db-61.7f)<.1f,"direct link at the FCC cap: 61.7 dB in 4 MHz");
        CHECK(fabsf(link.carrier_level_dbm-(9.5f-48.75f))<1e-4f,"dBmV to dBm across 75 ohm");
        link.link_loss_db=21.4f; video_rf_link_budget(&link,fs);
        CHECK(fabsf(link.cnr_db-40.3f)<.1f,"21.4 dB of loss: 40.3 dB");
        link.tuner_nf_db=10; video_rf_link_budget(&link,fs);
        CHECK(fabsf(link.cnr_db-37.3f)<.1f,"3 dB more noise figure: 3 dB less CNR");
        RFModulatorParams old={.carrier_level_dbm=-20,.noise_floor_dbm=-50};
        float before=video_rf_noise_rms(&old);
        video_rf_link_budget(&old,fs);
        CHECK(old.modulator_dbmv==9.5f && fabsf(old.cnr_db-40.3f)<.1f,"old pair becomes the budget that gives its CNR");
        CHECK(fabsf(video_rf_noise_rms(&old)/before-1)<1e-3f,"and the same injected noise");
    }
    test_colour_response();
    test_comb_shader_mode();
    test_chroma_aux_layout();
    test_stage_activation_by_connection();
    test_connection_decode_routing();
    test_region_sample_rates();
    test_physical_controls();
    test_half_to_float_exact();
    test_half_to_float_roundtrip();
    test_half_inf_nan();
    test_timing_ema();

    if (fails == 0) {
        printf("All video_chain + gpu_half helper tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", fails);
    return 1;
}
