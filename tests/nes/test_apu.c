#include <stdio.h>
#include "nes/apu.h"
static int failures;
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"APU FAIL %d: %s\n",__LINE__,#x);failures++;} } while(0)
int main(void) {
    APU a; apu_init(&a); apu_set_region(&a,1); apu_reset(&a);
    CHECK(a.pal && a.cpu_clock==APU_CPU_CLOCK_PAL);
    /* A period write leaves the running divider alone; the new period
     * loads at the next reload. */
    a.noise.enabled=true; a.noise.length_counter=1; a.noise.timer=1;
    apu_write(&a,0x400e,2); CHECK(a.noise.timer==1);
    apu_clock_noise_timer(&a.noise,a.pal); CHECK(a.noise.timer==14);
    apu_write(&a,0x4010,15); CHECK(a.dmc.timer_reload==50);
    apu_set_region(&a,0);
    a.noise.timer=1;
    apu_write(&a,0x400e,2); apu_clock_noise_timer(&a.noise,a.pal);
    CHECK(a.noise.timer==16);
    apu_write(&a,0x4010,15); CHECK(a.dmc.timer_reload==54);
    apu_set_region(&a,1); CHECK(a.dmc.timer_reload==50);
    /* A PAL half-frame clocks lengths every 16626/16626 CPU cycles, rather
     * than NTSC's 14914/14914. Use an isolated sequencer, no register delay. */
    a.frame.pending_write=false; a.frame.five_step=false; a.frame.cycle=0;
    a.pulse[0].reg[0]=0; a.pulse[0].length_counter=10;
    for(int i=0;i<16626;i++) apu_clock_frame_counter(&a);
    CHECK(a.pulse[0].length_counter==10);
    apu_clock_frame_counter(&a); CHECK(a.pulse[0].length_counter==9);
    for(int i=16627;i<=33252;i++) apu_clock_frame_counter(&a);
    CHECK(a.pulse[0].length_counter==8);
    a.triangle.enabled=true; a.triangle.sequence_step=7; a.triangle.timer=123;
    apu_write(&a,0x400b,0x28);
    CHECK(a.triangle.sequence_step==7 && a.triangle.timer==123);
    int level=apu_triangle_output(&a.triangle);
    a.triangle.length_counter=0; a.triangle.linear_counter=0;
    for(int i=0;i<1000;i++) apu_clock_triangle_timer(&a.triangle);
    CHECK(a.triangle.sequence_step==7 && apu_triangle_output(&a.triangle)==level);
    apu_write(&a,0x4015,0); CHECK(apu_triangle_output(&a.triangle)==level);
    /* Volume/halt writes do not retrigger envelopes; length writes do. */
    a.pulse[0].envelope_start=false;
    a.pulse[1].envelope_start=false;
    a.noise.envelope_start=false;
    apu_write(&a,0x4000,0x2f); apu_write(&a,0x4004,0x2f);
    apu_write(&a,0x400c,0x2f);
    CHECK(!a.pulse[0].envelope_start && !a.pulse[1].envelope_start &&
          !a.noise.envelope_start);
    apu_write(&a,0x4003,0); apu_write(&a,0x4007,0); apu_write(&a,0x400f,0);
    CHECK(a.pulse[0].envelope_start && a.pulse[1].envelope_start &&
          a.noise.envelope_start);
    /* Pulse period writes keep the running divider as well. */
    a.pulse[0].timer=5;
    apu_write(&a,0x4002,0x40); apu_write(&a,0x4003,0x01);
    CHECK(a.pulse[0].timer==5 && a.pulse[0].timer_reload==0x140);
    /* The sweep target mutes the channel when it would pass $7FF, even with
     * the sweep unit disabled or a zero shift; negate never overflows. */
    apu_write(&a,0x4001,0x00);           /* disabled, shift 0: target 2*p */
    apu_write(&a,0x4002,0xFF); apu_write(&a,0x4003,0x03);   /* $3FF */
    CHECK(!a.pulse[0].sweep_mute);
    apu_write(&a,0x4002,0x00); apu_write(&a,0x4003,0x04);   /* $400 */
    CHECK(a.pulse[0].sweep_mute);
    apu_write(&a,0x4001,0x08); CHECK(!a.pulse[0].sweep_mute);      /* negate */
    apu_write(&a,0x4001,0x01); CHECK(!a.pulse[0].sweep_mute);      /* $600 */
    apu_write(&a,0x4002,0x56); apu_write(&a,0x4003,0x05);   /* $556 */
    CHECK(a.pulse[0].sweep_mute);                           /* +$2AB */
    apu_write(&a,0x4002,0x07); apu_write(&a,0x4003,0x00);   /* period < 8 */
    CHECK(a.pulse[0].sweep_mute);
    apu_write(&a,0x4005,0x87); apu_write(&a,0x4006,0xF0); apu_write(&a,0x4007,0x07);
    CHECK(!a.pulse[1].sweep_mute);                          /* $7F0 + $F */
    apu_write(&a,0x4005,0x86); CHECK(a.pulse[1].sweep_mute);       /* + $1F */
    a.pulse[1].enabled=true; a.pulse[1].length_counter=1;
    a.pulse[1].reg[0]=0xBF; a.pulse[1].sequence_step=1;
    CHECK(apu_pulse_output(&a.pulse[1])==0);
    apu_write(&a,0x4005,0x87); CHECK(apu_pulse_output(&a.pulse[1])==15);
    /* Filter and analog settings are user configuration and survive reset. */
    a.filter_config.lp_alpha=0.5; a.analog.saturation=0.25f; a.analog.output_gain=2.0f;
    apu_reset(&a);
    CHECK(a.filter_config.lp_alpha==0.5 && a.analog.saturation==0.25f &&
          a.analog.output_gain==2.0f);
    /* The pulse divider runs while the channel is silent: a period set
     * during the silence is loaded by the time the next note starts, so
     * its first step comes within one new period, not the old count. */
    {
        APU b; apu_init(&b); apu_reset(&b);
        apu_write(&b,0x4015,0x01); apu_write(&b,0x4000,0x9F);
        apu_write(&b,0x4002,0xFF); apu_write(&b,0x4003,0x1F);   /* $7FF, length 2 */
        int cycles=0;
        do { apu_step(&b); cycles++; }
        while (b.pulse[0].length_counter && cycles<100000);
        CHECK(b.pulse[0].length_counter==0 && b.pulse[0].timer>100);
        apu_write(&b,0x4015,0x00);
        apu_write(&b,0x4002,0x10); apu_write(&b,0x4003,0x00);   /* $010, silent */
        for (int i=0;i<5000;i++) apu_step(&b);
        apu_write(&b,0x4015,0x01); apu_write(&b,0x4003,0x08);
        int step=b.pulse[0].sequence_step, wait=0;
        while (b.pulse[0].sequence_step==step && wait<5000) { apu_step(&b); wait++; }
        CHECK(wait<=(0x10+1)*2);
        /* The noise LFSR keeps shifting while the channel is off. */
        uint16_t lfsr=b.noise.lfsr;
        CHECK(!b.noise.enabled);
        for (int i=0;i<100;i++) apu_step(&b);
        CHECK(b.noise.lfsr!=lfsr && apu_noise_output(&b.noise)==0);
    }
    printf("APU region/triangle tests: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
