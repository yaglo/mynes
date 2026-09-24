#include <stdio.h>
#include <string.h>
#include "nes/nes.h"
#include "nes/debug.h"

static NES nes;
static uint8_t prg[0x8000], chr[0x10000];
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); ++failures; } } while(0)

static void scanline(Mapper *m) {
    mapper_ppu_bus_read(m, 0x2000);
    mapper_ppu_bus_read(m, 0x2000);
    mapper_ppu_bus_read(m, 0x2000);
    mapper_ppu_bus_read(m, 0x23c0);
}

int main(void) {
    for (int i=0; i<64; ++i) memset(chr+i*0x400,i,0x400);
    nes_init(&nes);
    nes_load_mapper(&nes,5,prg,sizeof(prg),chr,sizeof(chr),0);
    Mapper *m=&nes.mapper;
    nes.ppu.ctrl=CTRL_SPRITE_SIZE;
    nes.ppu.mask=MASK_BG_ENABLE|MASK_SPRITE_ENABLE;
    nes.ppu.scanline=12;
    /* Banks are indexed in units of the selected size. Check every mode,
     * independent BG set B, sprite set A, and B's repeated 4KB window. */
    const int a[]={7,3,1,0}, b[]={11,11,9,8};
    for (int mode=0; mode<4; ++mode) {
        mapper_cpu_write(m,0x5101,mode);
        mapper_cpu_write(m,0x5120+a[mode],1);
        mapper_cpu_write(m,0x5120+b[mode],3);
        nes.ppu.dot=5;
        CHECK(mapper_ppu_read(m,0)==(3<<(3-mode)));
        nes.ppu.dot=261;
        CHECK(mapper_ppu_read(m,0)==(1<<(3-mode)));
        nes.ppu.ctrl=0;
        nes.ppu.dot=5;
        CHECK(mapper_ppu_read(m,0)==(1<<(3-mode)));
        nes.ppu.ctrl=CTRL_SPRITE_SIZE;
    }
    nes.ppu.mask=0;
    mapper_cpu_write(m,0x5128,21);
    CHECK(mapper_ppu_read(m,0)==21);
    CHECK(mapper_ppu_read(m,0x1000)==21);
    mapper_cpu_write(m,0x5120,9);
    CHECK(mapper_ppu_read(m,0)==9);

    /* Mixed mapping used by CV3 ($E4): both CIRAM pages, ExRAM and fill.
     * Exercise the public PPU path, including $3000 mirrors and attributes. */
    mapper_cpu_write(m,0x5105,0xe4);
    mapper_cpu_write(m,0x5106,0x37);
    mapper_cpu_write(m,0x5107,2);
    ppu_write(&nes.ppu,0x2009,0x12);
    ppu_write(&nes.ppu,0x2409,0x34);
    ppu_write(&nes.ppu,0x2809,0x56);
    ppu_write(&nes.ppu,0x2c09,0xff); /* Fill cannot be written. */
    CHECK(ppu_read(&nes.ppu,0x2009)==0x12);
    CHECK(ppu_read(&nes.ppu,0x2409)==0x34);
    CHECK(ppu_read(&nes.ppu,0x2809)==0x56);
    CHECK(ppu_read(&nes.ppu,0x2c09)==0x37);
    CHECK(ppu_read(&nes.ppu,0x2fc0)==0xaa);
    CHECK(ppu_read(&nes.ppu,0x3409)==0x34);
    mapper_cpu_write(m,0x5104,2);
    CHECK(ppu_read(&nes.ppu,0x2809)==0);
    mapper_cpu_write(m,0x5104,0);
    mapper_cpu_write(m,0x5105,0x44);
    CHECK(ppu_read(&nes.ppu,0x2809)==0x12);
    CHECK(ppu_read(&nes.ppu,0x2c09)==0x34);

    mapper_reset(m);
    mapper_cpu_write(m,0x5203,1);
    scanline(m); /* Counter zero cannot trigger. */
    CHECK(mapper_cpu_read(m,0x5204)==0x40);
    scanline(m); /* Pending status is set even with IRQ disabled. */
    CHECK(!m->irq_pending);
    mapper_cpu_write(m,0x5204,0x80);
    CHECK(m->irq_pending);
    mapper_cpu_write(m,0x5204,0);
    CHECK(!m->irq_pending);
    mapper_cpu_write(m,0x5204,0x80);
    CHECK(m->irq_pending);
    CHECK(mapper_cpu_read(m,0x5204)==0xc0);
    CHECK(!m->irq_pending);
    mapper_cpu_clock(m); /* The interval that contained the last /RD. */
    mapper_cpu_clock(m);
    mapper_cpu_clock(m);
    CHECK(mapper_cpu_read(m,0x5204)==0x40);
    mapper_cpu_clock(m); /* Three idle CPU cycles clear in-frame. */
    CHECK(mapper_cpu_read(m,0x5204)==0);
    scanline(m);
    scanline(m);
    CHECK(m->irq_pending);
    mapper_cpu_read(m,0xfffa); /* NMI vector acknowledges/reset detection. */
    CHECK(!m->irq_pending && mapper_cpu_read(m,0x5204)==0);
    mapper_cpu_write(m,0x5203,0);
    scanline(m);
    CHECK(mapper_cpu_read(m,0x5204)==0x40);

    /* Debugger reads leave the frame and IRQ state alone: a real read of
     * $FFFA ends the frame, a real read of $5204 acknowledges. */
    mapper_cpu_write(m,0x5203,(uint8_t)(m->ext.mmc5.scanline_counter+1));
    mapper_cpu_write(m,0x5204,0x80);
    scanline(m);
    CHECK(m->irq_pending);
    CHECK(debug_read_cpu(&nes,0xFFFA)==mapper_cpu_peek(m,0xFFFA));
    CHECK(debug_read_cpu_word(&nes,0xFFFA)==(prg[0x7ffa]|prg[0x7ffb]<<8));
    CHECK(mapper_cpu_peek(m,0x5204)==0xC0);
    CHECK(m->irq_pending && m->ext.mmc5.in_frame);
    CHECK(mapper_cpu_read(m,0xFFFA)==prg[0x7ffa]);
    CHECK(!m->irq_pending && !m->ext.mmc5.in_frame);
    mapper_cpu_write(m,0x5204,0);

    /* $5114-$5116 bit 7 clear maps PRG RAM into $8000-$DFFF; $5117 and
     * mode 0 are always ROM. */
    mapper_reset(m);
    for (int i=0; i<4; ++i) memset(prg+i*0x2000,0x40+i,0x2000);
    mapper_cpu_write(m,0x6123,0x5a);
    CHECK(mapper_cpu_read(m,0x8123)==0x40);
    mapper_cpu_write(m,0x8123,0x11);    /* ROM ignores writes */
    CHECK(mapper_cpu_read(m,0x8123)==0x40);
    mapper_cpu_write(m,0x5114,0x00);
    mapper_cpu_write(m,0x5116,0x81);
    CHECK(mapper_cpu_read(m,0x8123)==0x5a);
    CHECK(mapper_cpu_read(m,0xC123)==0x41);
    mapper_cpu_write(m,0x9234,0x77);
    CHECK(mapper_cpu_read(m,0x7234)==0x77);
    mapper_cpu_write(m,0x5100,1);       /* 16 KB: $5115 governs $8000 */
    mapper_cpu_write(m,0x5115,0x00);
    mapper_cpu_write(m,0x5117,0x00);
    CHECK(mapper_cpu_read(m,0xB234)==0x77);
    CHECK(mapper_cpu_read(m,0xC000)==0x40);
    mapper_cpu_write(m,0x5100,0);       /* 32 KB: always ROM */
    CHECK(mapper_cpu_read(m,0x8000)==0x40);
    memset(prg,0,0x8000);

    /* Exercise the real PPU bus hook over successive frames: counter must
     * restart, assert near line 1's attribute fetch, and clear in vblank. */
    nes_init(&nes);
    memset(prg,0xea,sizeof(prg));
    prg[0x6000]=0x4c; prg[0x6001]=0; prg[0x6002]=0xe0;
    prg[0x7ffc]=0; prg[0x7ffd]=0xe0;
    nes_load_mapper(&nes,5,prg,sizeof(prg),chr,sizeof(chr),0);
    nes_reset(&nes);
    nes.ppu.mask=MASK_BG_ENABLE|MASK_SPRITE_ENABLE;
    mapper_cpu_write(m,0x5203,1);
    mapper_cpu_write(m,0x5204,0x80);
    int irqs=0;
    while(nes.ppu.frame<4) {
        nes_step(&nes);
        if(m->irq_pending) {

            CHECK(nes.ppu.scanline==(nes.ppu.frame==0 ? 2 : 1) && nes.ppu.dot>=4 && nes.ppu.dot<=10);
            mapper_cpu_read(m,0x5204);
            ++irqs;
        }
        if(nes.ppu.scanline==241 && nes.ppu.dot<4)
            CHECK(!(mapper_cpu_read(m,0x5204)&0x40));
    }
    CHECK(irqs==4);
    printf("MMC5 tests: %s (%d failures)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
