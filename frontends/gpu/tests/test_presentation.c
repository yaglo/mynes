#include "gpu_presentation.h"
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
