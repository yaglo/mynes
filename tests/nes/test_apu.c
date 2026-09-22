#include <stdio.h>
#include "nes/apu.h"
static int failures;
#define CHECK(x) do { if(!(x)) {fprintf(stderr,"APU FAIL %d: %s\n",__LINE__,#x);failures++;} } while(0)
int main(void) {
    APU a; apu_init(&a); apu_set_region(&a,1); apu_reset(&a);
    CHECK(a.pal && a.cpu_clock==APU_CPU_CLOCK_PAL);
    apu_write(&a,0x400e,2); CHECK(a.noise.timer==14);
    apu_write(&a,0x4010,15); CHECK(a.dmc.timer_reload==50);
    apu_set_region(&a,0);
    apu_write(&a,0x400e,2); CHECK(a.noise.timer==16);
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
    printf("APU region/triangle tests: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
