# When the Compiler Knows More Than You Do

*Deriving DMA behavior from CPU microcode*

---

Here is a problem most NES emulators get subtly wrong: the DMC sample fetch.

The NES audio chip (APU) has a delta modulation channel that plays 1-bit audio samples read directly from cartridge ROM. When the DMC needs a sample, it halts the CPU and takes over the bus for 3-4 cycles. This happens mid-instruction. Not between instructions -- literally between cycle 3 and cycle 4 of whatever the CPU was doing.

For the DMA controller to work correctly, it needs to know three things about the CPU's upcoming cycle:

1. **What address would the CPU read next?** When the CPU is halted, the bus doesn't float. The DMA controller re-reads from the address the CPU was about to drive -- and those reads have side effects. Reading $2002 clears the VBL flag. Reading $2007 increments the VRAM pointer. Reading $4015 clears the frame IRQ.

2. **Is the next cycle a read or a write?** Real hardware cannot halt the CPU during a write cycle. The DMA waits.

3. **Is the next cycle a SHA/SHX/SHY dummy-read?** These unofficial store instructions compute their store value using the high byte of the target address. When DMA halts on the dummy-read cycle right before the write, the address corruption behavior changes -- the `(ADH+1)` AND term drops out entirely.

In most emulators, these questions are answered by hand-coded logic. Someone reads the CPU code, figures out which instructions read from where on which cycle, and writes a parallel state machine that encodes those answers. This works until someone changes the CPU timing. The DMA code doesn't update. Subtle desync.

## The DSL Compiler Derives It

The DSL compiler already knows the answer to all three questions. During code emission, it processes every microcode step and classifies it by bus operation -- read, write, or dummy read. It records which steps contain SHA-family fixup operations. From this analysis, it generates three pure switch-statement functions mechanically.

Here is what the generated `cpu_get_next_read_addr()` looks like (excerpt from `cpu_gen.c`):

```c
uint16_t cpu_get_next_read_addr(CPU *cpu) {
    switch (cpu->uPC) {
    case 0: return cpu->PC;
    case 1: return cpu->PC;
    case 5: return 0xFFFA;
    case 6: return 0xFFFB;
    case 22: return cpu->PC;
    case 23: return cpu->PC;
    case 24: return ((uint16_t)cpu->ADH << 8) | cpu->ADL;
    // ... 600+ more cases
    }
}
```

Each case inspects the current microcode PC and returns the exact address the CPU would drive on its next cycle. Case 0 (the fetch state) would read from PC. Case 24 (the second cycle of `lda-zp`) would read from the effective address in ADH:ADL. The compiler knows because it already emitted the code that does the read.

The `cpu_next_is_write()` function is similar -- a switch that returns true for every uPC value where the next cycle writes:

```c
bool cpu_next_is_write(CPU *cpu) {
    switch (cpu->uPC) {
    case 2: return true;    /* nmi-handler: push PCH */
    case 3: return true;    /* nmi-handler: push PCL */
    case 4: return true;    /* nmi-handler: push P */
    case 76: return true;   /* sta-zp: store */
    case 314: return true;  /* inc-zp: write old value */
    case 315: return true;  /* inc-zp: write new value */
    // ...
    default: return false;
    }
}
```

And `cpu_next_is_sha_dummy_read()` identifies exactly 5 microcode steps -- the specific cycles where SHA, SHX, SHY, and TAS perform their page-crossing dummy reads:

```c
bool cpu_next_is_sha_dummy_read(CPU *cpu) {
    switch (cpu->uPC) {
    case 815: return true;
    case 819: return true;
    case 824: return true;
    case 828: return true;
    case 832: return true;
    default: return false;
    }
}
```

None of these are hand-written. They are mechanically derived from the DSL. When you add or change an instruction, the DMA helpers update automatically. The CPU timing specification and the DMA interaction rules cannot drift out of sync because they are the same data.

## How nes_step() Uses It

Here is the actual DMC DMA implementation from `nes.h` (lines 513-550):

```c
if (apu_dmc_needs_sample(&nes->apu) && nes->cpu.rdy
    && !cpu_next_is_write(&nes->cpu)) {
    nes->cpu.rdy = false;

    bool need_alignment = (nes->cpu.cycles & 1) != 0;
    int total_dmc_cycles = need_alignment ? 4 : 3;
    uint16_t halt_addr = cpu_get_next_read_addr(&nes->cpu);

    if (cpu_next_is_sha_dummy_read(&nes->cpu)) {
        nes->cpu.ignore_h = 1;
    }

    for (int dmc_cycle = 0; dmc_cycle < total_dmc_cycles; dmc_cycle++) {
        if (dmc_cycle == total_dmc_cycles - 1) {
            /* Last cycle: actual sample read */
            uint8_t sample = nes_cpu_read(&nes->cpu, nes->apu.dmc_current_addr);
            apu_dmc_load_sample(&nes->apu, sample);
        } else {
            /* Halt/dummy/alignment cycles: re-read the upcoming bus addr */
            (void)nes_cpu_read(&nes->cpu, halt_addr);
        }
        ppu_step(&nes->ppu);
        ppu_step(&nes->ppu);
        ppu_step(&nes->ppu);
        apu_step(&nes->apu);
        nes->master_tick += 12;
    }

    if (!nes->dma.oam_active) {
        nes->cpu.rdy = true;
    }
}
```

Walk through what happens:

1. The APU signals it needs a sample. The CPU is ready (not already halted) and its next cycle is not a write.
2. `cpu_get_next_read_addr()` returns the address the CPU was about to read. This is the halt address.
3. If this is a SHA-family dummy-read cycle, `ignore_h` is set so the upcoming store drops the high-byte AND.
4. The DMA loop runs 3-4 cycles. The alignment cycle (if needed) and dummy cycles all re-read `halt_addr` -- with full side effects. The final cycle reads the actual DMC sample.
5. Each DMA cycle also steps the PPU (3 dots) and APU, maintaining system synchronization.

The CPU does not know about DMA. The DMA code does not encode instruction timing. The compiler bridges them.

## A Specific Scenario

Consider this sequence: the DMC needs a sample, and the CPU is about to execute cycle 314 of `cpu_gen.c` -- the first write cycle of an INC zero-page instruction (write old value back to memory).

1. `cpu_next_is_write()` checks uPC 314. Returns true.
2. DMA defers. The CPU executes cycle 314 (writes old value).
3. Next nes_step(): CPU is now at uPC 315 (second write -- write new value).
4. `cpu_next_is_write()` checks uPC 315. Returns true again.
5. DMA defers again. CPU executes cycle 315 (writes new value, sets flags).
6. Next nes_step(): CPU is at uPC 0 (fetch).
7. `cpu_next_is_write()` checks uPC 0. Returns false -- fetch is a read.
8. DMA proceeds. `cpu_get_next_read_addr()` returns `cpu->PC`. The halt/dummy cycles re-read from PC. The final cycle reads the DMC sample.

This exact behavior -- DMA waiting through two consecutive write cycles of a RMW instruction -- is what the AccuracyCoin DMA tests verify. The timing is correct not because someone hand-analyzed the INC instruction and wrote a special case, but because the compiler classified both writes as writes during code generation.

## The Broader Pattern

This technique -- deriving cross-cutting concerns from a single specification -- applies beyond emulation. The timing spec and the DMA interaction rules are the same data, expressed differently. One view answers "what does the CPU do on each clock." The other answers "what would the bus look like if we froze the CPU right now." Both are projections of the same underlying microcode.

The constraint is that the code generator must be able to statically classify every step. For the 6502, this is straightforward: every cycle form in the DSL has exactly one bus operation, and the compiler can see whether it is a read, write, or dummy. A more complex CPU with dynamic bus behavior (conditional reads that depend on runtime state) would make this harder, but the same principle applies wherever you can statically analyze the specification.

In any system where multiple subsystems need to agree on behavior -- interrupt timing, bus arbitration, cache coherence protocols -- expressing the authoritative specification once and deriving each subsystem's view from it eliminates an entire class of consistency bugs. Change one line in the DSL, rerun the compiler, and both the CPU execution and the DMA interaction update together. No separate state machine to maintain. No "don't forget to update the DMA code when you change instruction timing" comments that inevitably get ignored.

The DSL compiler is 904 lines of Scheme. The DMA helper generation is maybe 100 of those. For that investment, you get a guarantee that the CPU and DMA can never disagree about what happens on the bus.
