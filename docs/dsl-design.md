# Timing Is Data, Not Code

A declarative approach to cycle-accurate 6502 CPU emulation.

## The Problem

Writing a cycle-accurate CPU emulator means specifying what happens on every clock cycle of every instruction. For the 6502, that's 256 opcodes across ~13 addressing modes, with conditional extra cycles for page boundary crossings and taken branches. The timing isn't just bookkeeping -- it determines when bus reads and writes happen, which affects interrupt recognition, DMA interaction, and PPU synchronization.

The standard approaches each have problems:

**Giant switch with manual cycle counting.** You write a case per opcode, track a cycle counter, and manually sequence bus operations. With 256 opcodes and up to 8 cycles each, this means thousands of lines of repetitive C where copy-paste errors in cycle 4 of STA (zp),Y are invisible until a specific game breaks. FCEUX takes this approach.

**Per-instruction functions with cycle tables.** Cleaner than a raw switch, but the timing is still encoded as imperative code inside each function. The cycle table tells you "this takes 5 cycles" but not what happens on cycle 3 specifically. Nestopia uses this pattern.

**Instruction-level execution.** Run the entire instruction, charge N cycles, move on. Fast and simple. But wrong for anything that depends on mid-instruction bus activity: sprite DMA, DMC sample fetch, mappers that watch bus addresses, or interrupts arriving between the penultimate and final cycle of an instruction. You can add fixups, but they multiply until you're fighting the abstraction.

The fundamental issue: in all these approaches, the timing specification is interleaved with the implementation. The "what happens when" is expressed as control flow -- if/else chains, state variables, cycle counters -- rather than as data. When the specification is code, there's no structural guarantee that you haven't miscounted a cycle, forgotten a dummy read, or skipped the double-write that RMW instructions perform on real silicon.

## The Insight

6502 instruction timing is not arbitrary. It follows regular patterns dictated by the addressing mode. Every absolute-indexed read instruction does the same thing:

1. Fetch low address byte, increment PC
2. Fetch high address byte, add index to low byte, latch carry
3. Read from (possibly wrong) effective address
4. If carry was set, fix high byte, re-read from correct address

This is true for LDA abs,X and for AND abs,X and for ADC abs,X. The only thing that changes is what the instruction does with the byte it read. The bus access pattern -- the timing -- is data. It can be described once and reused.

If you express timing as structured data rather than imperative code, a compiler can:

- Ensure every cycle has exactly one bus operation
- Eliminate manual cycle counting
- Generate the tedious C code automatically
- Make the specification readable against the 6502 datasheet

## The DSL

The microcode DSL in `src/cpu/nes6502.dsl` (1401 lines) describes all 256 opcodes in a layered S-expression format. A Chicken Scheme compiler (`tools/dsl2c.scm`, 904 lines) reads it and generates C.

### Layer 1: Cycle Macros

Named single-cycle patterns for common bus operations:

```scheme
(defcycle fetch-adl       (fetch adl pc))
(defcycle fetch-adh       (fetch adh pc))
(defcycle read-to-dl      (read dl ad))
(defcycle fixup-page-read (read dl ad) (adc8-from-pagecross adh adh))
(defcycle write-nz-dl     (write ad dl) (nz dl))
```

These name the atoms. `(fetch-adl)` anywhere in a state body expands to `(cycle (fetch adl pc))` -- one clock cycle that reads from PC into the address low latch and bumps PC.

### Layer 2: Addressing Mode Templates

Templates compose cycle macros into multi-cycle patterns with parameters:

```scheme
(template read-abs (op reg)
  (fetch-adl)
  (fetch-adh)
  (cycle (read dl ad) (op reg dl)))

(template rmw-zp (op)
  (fetch-zp-addr)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl))
```

`read-abs` takes an operation and a register as parameters. When instantiated, `op` and `reg` are substituted textually. The template defines the bus access pattern; the parameters define what to do with the data.

The `when` construct handles variable-length instructions:

```scheme
(template read-abx (op reg)
  (fetch-adl)
  (fetch-adh-add-x)
  (cycle (read dl ad) (op reg dl))
  (when page-cross
    (cycle (adc8-from-pagecross adh adh) (read dl ad) (op reg dl))))
```

The third cycle performs the operation optimistically. If no page cross occurred, we're done -- 4 cycles total. If the indexed add carried, the read used a wrong high byte, so a fourth cycle fixes the address and re-reads. This matches the real 6502 exactly: the "penalty cycle" isn't a number you add; it's a conditional extra bus access with specific behavior.

### Layer 3: Instruction States

States instantiate templates with specific operations:

```scheme
(state lda-abs (read-abs load-nz a))
(state adc-abs (read-abs do-adc a))
(state cmp-abs (read-abs do-cmp a))
(state and-abs (read-abs do-and a))
```

Each line is a complete instruction definition. All eight LDA addressing variants are eight one-liners:

```scheme
(state lda-imm (read-imm load-nz a))
(state lda-zp  (read-zp  load-nz a))
(state lda-zpx (read-zpx load-nz a))
(state lda-abs (read-abs load-nz a))
(state lda-abx (read-abx load-nz a))
(state lda-aby (read-aby load-nz a))
(state lda-izx (read-izx load-nz a))
(state lda-izy (read-izy load-nz a))
```

Compare this to hand-writing 4-8 switch cases per variant, each with manual bus reads, flag updates, and cycle transitions. The DSL version has no room for the "off by one cycle" or "forgot the dummy read" bugs that plague hand-coded implementations.

### Layer 4: Opcode Table

```scheme
(opcode #xA9 lda-imm)
(opcode #xA5 lda-zp)
(opcode #xAD lda-abs)
```

Maps bytes to states. The compiler builds a 256-entry lookup table.

## The Compiler

The compiler is 904 lines of Chicken Scheme. Its job is mechanical: expand templates, assign microcode PC values, emit C switch cases.

**Template expansion** is textual substitution. When the compiler sees `(read-abs load-nz a)`, it looks up the `read-abs` template, substitutes `op` with `load-nz` and `reg` with `a`, then recursively expands `load-nz` the same way. The result is a flat sequence of `(cycle ...)` forms.

**uPC assignment** is sequential. The compiler walks states in definition order. Each state gets a contiguous range of case labels. The `fetch` state is always case 0.

**`when` blocks** compile to conditional jumps. The cycle before a `when` block emits: "if condition, go to the when's first case; else skip past all consecutive when blocks." This is how variable-timing instructions work without any runtime cycle counter.

**DMA helper generation** is where the compiler does something most emulators don't attempt: it derives DMA interaction rules from the same microcode that defines CPU execution. During code emission, the compiler classifies every microcode step by its bus operation -- read, write, or dummy read. It also records which steps contain SHA/SHX/SHY/TAS page-fixup operations. From this analysis, it generates three helper functions as pure switch statements over `uPC`:

- `cpu_get_next_read_addr()` -- inspects the current `uPC` and returns the address the CPU *would* drive on the bus during its next cycle. When DMC DMA halts the CPU, the bus address doesn't disappear; the DMA controller re-reads from this address (with full side effects -- clearing VBL on $2002, incrementing VRAM on $2007, clearing frame IRQ on $4015).
- `cpu_next_is_write()` -- returns true if the next cycle is a write. Real hardware cannot halt the CPU during a write cycle; DMA waits for the next read.
- `cpu_next_is_sha_dummy_read()` -- identifies the 5 specific microcode steps where SHA/SHX/SHY/TAS perform their page-crossing dummy read. When DMA halts on one of these cycles, the address corruption behavior changes: the `(ADH+1)` term drops out entirely, and the stored value becomes just `A & X` (or `X`, or `Y`) without the high-byte AND.

These aren't hand-written -- they're mechanically derived from the DSL. When you add or change an instruction in the DSL, the DMA helpers update automatically. The CPU timing specification and the DMA interaction rules can never drift out of sync.

## What This Enables

### The DSL reads like a datasheet

Compare the NMI handler in the DSL to the corresponding timing diagram in the 6502 datasheet:

```scheme
(state nmi-handler
  (cycle (dummy pc))                                          ;; T1: internal
  (cycle (write sp pch) (sp-1))                               ;; T2: push PCH
  (cycle (write sp pcl) (sp-1))                               ;; T3: push PCL
  (cycle (prep-push-p nmi) (write sp dl) (sp-1))              ;; T4: push P
  (cycle (set-flag i) (snapshot-i) (read adl vec-nmi-lo))     ;; T5: read vector lo
  (cycle (read adh vec-nmi-hi) (mov pcl adl) (mov pch adh))   ;; T6: read vector hi
  (goto fetch))
```

Each line maps 1:1 to a row in the timing diagram. The bus operation, the register manipulation, and the cycle boundary are all visible in one place.

### Unofficial opcodes compose naturally

The unofficial opcode DCP (decrement memory, then compare) is a RMW instruction. In the DSL, its core is literally DEC followed by CMP:

```scheme
(state dcp-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (dec dl))
  (cycle (write ad dl) (cmp a dl)))
```

The first write puts back the old value (after decrementing), the second write puts back the new value. This matches real silicon behavior where the RMW instructions write the unmodified value, perform the operation internally, then write the result. The DSL makes this double-write visible and explicit.

All seven DCP addressing variants follow the same pattern -- take the addressing mode's setup cycles, append the DEC+CMP write pair. The same holds for ISC (INC + SBC), SLO (ASL + ORA), RLA (ROL + AND), SRE (LSR + EOR), and RRA (ROR + ADC).

### Hardware quirks are explicit

**IRQ flag latency.** When CLI clears the interrupt disable flag, the change doesn't take effect until after the next instruction. The DSL makes this visible:

```scheme
(state cli
  (cycle (snapshot-i) (clear-flag i) (dummy pc)))
```

`(snapshot-i)` captures the current I flag value *before* clearing it. The interrupt poll at the next fetch uses the snapshot, giving the 1-instruction delay. Without the DSL, this timing detail is buried in flag-manipulation code where it's easy to get wrong.

**NMI hijacking.** If an NMI arrives during an IRQ or BRK sequence, the 6502 uses the NMI vector instead. The DSL makes the hijack checkpoints visible:

```scheme
(state irq-handler
  (cycle (dummy pc) (when nmi-pending (goto nmi-handler)))
  (cycle (write sp pch) (sp-1) (when nmi-pending (goto nmi-handler)))
  ...)
```

The `when nmi-pending` checks appear at exactly the hardware-correct cycle boundaries.

**Branch page-cross dummy reads.** When a branch crosses a page boundary, the CPU reads from an intermediate address with the wrong high byte before correcting. The DSL separates this into three distinct operations:

```scheme
(template branch-if (cond)
  (cycle (fetch dl pc) (branch-decide cond dl))      ;; decide, don't update PC
  (when branch-taken
    (cycle (dummy pc) (branch-update-pcl dl)))        ;; update low byte only
  (when page-cross
    (cycle (dummy pc) (branch-correct-pch dl))))      ;; fix high byte
```

Each cycle's bus address is precisely defined by the current state of the PC registers.

### Structural correctness

The RMW double-write is enforced by the template structure. Every RMW template has the pattern:

```scheme
(cycle (write ad dl) (op dl))    ;; write old value, perform operation
(write-nz-dl)                    ;; write new value, set flags
```

You cannot accidentally produce an RMW instruction that writes only once. The template makes it structural.

### DMA behavior derived from microcode

In most emulators, DMA cycle-stealing is implemented separately from the CPU -- a hand-written state machine that makes assumptions about when the CPU reads vs writes. When someone changes instruction timing, the DMA code doesn't automatically update, and subtle desync bugs creep in.

Here, the DMA system calls `cpu_get_next_read_addr()`, `cpu_next_is_write()`, and `cpu_next_is_sha_dummy_read()` -- all generated by the DSL compiler from the same microcode that defines instruction execution. The NES system code (`nes.h`) uses these to implement DMC sample fetch:

```c
if (apu_dmc_needs_sample(&nes->apu) && nes->cpu.rdy
    && !cpu_next_is_write(&nes->cpu)) {
    uint16_t halt_addr = cpu_get_next_read_addr(&nes->cpu);
    if (cpu_next_is_sha_dummy_read(&nes->cpu))
        nes->cpu.ignore_h = 1;
    // halt CPU, re-read halt_addr during dummy cycles,
    // read sample on final cycle
}
```

The CPU doesn't know about DMA. The DMA code doesn't encode instruction timing. The compiler bridges the two by analyzing the microcode and exposing the bus behavior as queryable functions. This is a single source of truth for both execution and bus interaction -- change a cycle in the DSL, and the DMA helpers reflect it automatically.

## Comparison to Other Emulators

**Mesen (C#):** Uses per-cycle callbacks with manual state tracking. Achieves high accuracy but the cycle logic is spread across instruction methods. Adding an instruction means writing cycle-by-cycle C# code.

**Nestopia (C++):** Instruction-level execution with cycle tables. Reasonably accurate for most games but cannot model mid-instruction bus activity. Timing is a number, not a sequence of bus operations.

**FCEUX (C):** Large switch + per-instruction functions. Timing corrections applied as fixups. Accurate enough for compatibility but the corrections accumulate into hard-to-audit code.

**This project:** The DSL *is* the specification. When a timing bug is found, the fix is typically a one-line change to the DSL -- adding a missing `(snapshot-i)`, reordering operations within a cycle, or adding a `(when page-cross ...)` block. The C code regenerates automatically.

## Results

The generated `cpu_gen.c` is ~5300 lines containing 834 switch cases. It compiles to a tight jump table that modern C compilers optimize well.

The bundled AccuracyCoin suite now passes all 144 tests (144/144, no failures or unrun tests), rechecked on 22 September 2026 at the default CPU/PPU alignment. This includes the suite's DMC DMA and sprite-evaluation cases. See the [test fixture and revision](../tests/accuracy_coin/UPSTREAM.md). The DSL approach keeps CPU timing fixes in the specification; system integration still needs independent CPU/PPU/APU and mapper coverage.

## Applicability Beyond 6502

The approach generalizes to any CPU where instructions follow regular multi-cycle patterns. The Z80 has a similar structure: addressing modes define bus access patterns, and the operation is orthogonal. The 65816 (SNES CPU) is a direct superset. The 68000 has more complex bus sequences but the same regularity. Even modern microcontrollers with pipeline stalls could benefit -- anywhere "what the bus does on each clock" is the authoritative specification.

The compiler itself is CPU-agnostic in structure. Only two things are 6502-specific:

1. The vocabulary of operations (`adc`, `sbc`, `asl`, etc.) and the C code each emits
2. The register/flag model in the generated struct

Retargeting to a Z80 would mean writing new `emit-internal-op` handlers for Z80 ALU operations and a new register model. The template expansion, uPC assignment, `when` block compilation, and DMA helper generation would carry over unchanged.

## Trade-offs

**Build-time dependency.** The compiler requires Chicken Scheme. This is mitigated by committing the generated `cpu_gen.c` and `cpu_gen.h` to the repository. Most developers never run the compiler; they edit the DSL and regenerate only when changing CPU behavior.

**New language to learn.** The DSL is not C and not standard Scheme. However, it has roughly 10 constructs total (`defcycle`, `template`, `state`, `opcode`, `cycle`, `when`, `fetch`, `read`, `write`, `dummy`). Someone familiar with the 6502 can learn it in 30 minutes by reading the existing definitions. The DSL file has 1401 lines; the generated C has 5300. The compression ratio is the point.

**Large generated switch.** 834 cases in one function looks alarming. In practice, the compiler generates a jump table (measured with `-O2` on Clang and GCC). The per-call overhead is an indexed jump, a handful of assignments, and a return. There is no performance problem.

**Debugging indirection.** When stepping through the generated C in a debugger, you see case 314 of a 5300-line switch. The `/* inc-zp */` comment tells you which instruction, and the uPC value maps back to a specific cycle in the DSL. This requires one level of indirection compared to hand-written code. The comments in the generated code make this manageable.

**No runtime flexibility.** The microcode is compiled statically. You cannot change instruction timing at runtime (e.g., to simulate a 65C02 variant). This is acceptable for a NES emulator targeting one specific CPU, but a multi-target emulator might want an interpreted approach instead.
