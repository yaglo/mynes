#include "gpu_presentation.h"
#include "presentation_schedule.h"
#include <stdio.h>

int main(void) {
    int failures=0;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); failures++; } } while(0)
    CHECK(gpu_presentation_slots(120,60.0988f)==2);
    CHECK(gpu_presentation_slots(240,60.0988f)==4);
    CHECK(gpu_presentation_slots(100,50.007f)==2);
    CHECK(gpu_presentation_slots(144,60.0988f)==1);
    CHECK(gpu_presentation_slots(60,60.0988f)==1);
    CHECK(gpu_presentation_slots(0,60)==1);
    CHECK(gpu_presentation_slots(NAN,60)==1);
    CHECK(gpu_presentation_slots(120,0)==1);
    CHECK(gpu_presentation_slots(120,NAN)==1);
    CHECK(gpu_presentation_vsync_paced(GPU_PRESENT_HOLD,60,60.0988f));
    CHECK(gpu_presentation_vsync_paced(GPU_PRESENT_60HZ,59.94f,60.0988f));
    CHECK(gpu_presentation_vsync_paced(GPU_PRESENT_HOLD,50,50.007f));
    CHECK(!gpu_presentation_vsync_paced(GPU_PRESENT_60HZ,60,50.007f));
    CHECK(!gpu_presentation_vsync_paced(GPU_PRESENT_60HZ,120,60.0988f));
    CHECK(!gpu_presentation_vsync_paced(GPU_PRESENT_BFI,60,60.0988f));
    CHECK(!gpu_presentation_vsync_paced(GPU_PRESENT_HOLD,NAN,60));
    CHECK(gpu_presentation_playback_period(GPU_PRESENT_HOLD,16639268,60,1)==16666667);
    CHECK(gpu_presentation_playback_period(GPU_PRESENT_HOLD,16639268,120,0)==16639268);
    const uint64_t last = 1000000000, period = 16666667, ntsc = 16639268;
    CHECK(gpu_presentation_period_ns(GPU_PRESENT_60HZ,ntsc)==period);
    CHECK(gpu_presentation_period_ns(GPU_PRESENT_60HZ,19997200)==19997200);
    CHECK(gpu_presentation_period_ns(GPU_PRESENT_HOLD,ntsc)==ntsc);
    CHECK(gpu_presentation_period_ns(GPU_PRESENT_BFI,ntsc)==ntsc);
    CHECK(gpu_presentation_next_ns(0,last)==last+period);
    CHECK(gpu_presentation_next_ns(last,last+50000)==last+period);
    CHECK(gpu_presentation_next_ns(last,last+period*5)==last+period*6);
    uint64_t deadline=last;
    for (int i=0;i<600;i++) deadline=gpu_presentation_next_ns(deadline,deadline+50000);
    CHECK(deadline==last+600*period); /* 50 us timer overshoot does not accumulate. */
    const double dt=1.0/60;
    double now=100, target=mynes_presentation_target(0,now,dt,1,0);
    CHECK(fabs(target-(now+2*dt))<1e-9);
    for(int i=0;i<600;i++) {
        now+=dt;
        target=mynes_presentation_target(target,now+.00005,dt,1,0);
    }
    CHECK(fabs(target-(100+602*dt))<1e-9);
    // A 30 ms encoding stall used to leave a past deadline until manual reset.
    now+=dt+.030;
    target=mynes_presentation_target(target,now,dt,1,0);
    CHECK(fabs(target-(now+2*dt))<1e-9);
    double next=mynes_presentation_target(target,now+2*dt,dt,2,0);
    CHECK(fabs(next-target-2*dt)<1e-9); // preserve skipped source intervals
    CHECK(fabs(mynes_presentation_target(next,now,dt,1,1)-now-2*dt)<1e-9);
    CHECK(fabs(mynes_presentation_target(next,now,dt,-100,0)-now-2*dt)<1e-9);
    CHECK(fabs(mynes_presentation_target(next,now+10,dt,600,0)-now-10-2*dt)<1e-9);
    CHECK(fabs(mynes_presentation_target(now+10,now,dt,1,0)-now-2*dt)<1e-9);
    for (int n=2;n<=8;n++) for (int d=0;d<=20;d++) {
        float floor=d/20.0f,sum=0;
        for (int i=0;i<n;i++) {
            float gain=gpu_presentation_gain(n,i,floor);
            CHECK(gain>=0 && gain<=n);
            sum+=gain/n;
            if (i>0 && d==0) CHECK(gain==0);
            if (d==20) CHECK(gain==1);
        }
        CHECK(fabsf(sum-1)<1e-6f);
    }
    return failures ? 1 : 0;
}
