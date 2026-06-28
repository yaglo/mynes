# Timing Is Data, Not Code

*A declarative DSL for cycle-accurate 6502 CPU emulation*

---

Hand-writing a cycle-accurate 6502 emulator in C is a combinatorial nightmare. You have 256 opcodes across 13 addressing modes, with conditional extra cycles for page boundary crossings and taken branches. Each cycle has exactly one bus operation -- a read, a write, or a dummy read -- and the exact address and timing of that operation determines interrupt recognition, DMA interaction, and PPU synchronization. Get a single cycle wrong and the bug won't show up until some obscure test ROM checks the exact behavior of a DMC sample fetch landing on the fourth cycle of an INC instruction.

The bugs that kill you are not algorithmic. They are bookkeeping errors. A forgotten dummy read in a read-modify-write instruction. An off-by-one cycle in a page-crossing penalty. The double-write sequence that RMW instructions perform on real silicon (write old value, then write new value) skipped entirely because it seemed redundant. A CLI instruction that clears the interrupt flag immediately instead of after a one-instruction delay. Each of these is a one-line mistake in a sea of nearly identical switch cases, invisible until a specific game relies on the exact timing.

The standard approaches all share the same fundamental problem: the timing specification is interleaved with the implementation. Whether you write a giant switch with manual cycle counting (FCEUX), per-instruction functions with cycle tables (Nestopia), or instruction-level execution with fixups, the "what happens on cycle 3" is expressed as imperative code. When the specification is code, there is no structural guarantee against miscounted cycles.

## The Insight: Timing Follows the Addressing Mode

6502 instruction timing is not arbitrary. It follows regular patterns dictated by the addressing mode. Every absolute-indexed read instruction does the same thing:

1. Fetch low address byte, increment PC
2. Fetch high address byte, add index to low byte, latch carry
3. Read from (possibly wrong) effective address
4. If carry was set, fix high byte, re-read from correct address

This is true for LDA abs,X and AND abs,X and ADC abs,X. The only thing that changes is what the instruction does with the byte it read. The bus access pattern -- the timing -- is *data*. It can be described once and reused.

If you express timing as structured data rather than imperative code, a compiler can ensure every cycle has exactly one bus operation, eliminate manual cycle counting, and generate the tedious C automatically.

## Four Layers of Composition

The DSL in `src/cpu/nes6502.dsl` describes all 256 opcodes in a layered S-expression format. A Chicken Scheme compiler reads it and generates C. Here is how the layers compose.

### Layer 1: `defcycle` -- Name the Atoms

Named single-cycle patterns for common bus operations:

```scheme
(defcycle fetch-adl       (fetch adl pc))
(defcycle fetch-adh       (fetch adh pc))
(defcycle read-to-dl      (read dl ad))
(defcycle fixup-page-read (read dl ad) (adc8-from-pagecross adh adh))
(defcycle write-nz-dl     (write ad dl) (nz dl))
```

`(fetch-adl)` anywhere in a state body expands to one clock cycle that reads from PC into the address low latch and increments PC. Twenty of these cover the common patterns: fetch operands, read from effective address, write results, dummy reads for timing padding.

### Layer 2: `template` -- Compose Addressing Modes

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

The third cycle performs the operation optimistically. If no page cross occurred, the instruction is done in 4 cycles. If the indexed add carried, the read used a wrong high byte, so a fourth cycle fixes the address and re-reads. This matches the real 6502 exactly: the "penalty cycle" is not a number you add to a total. It is a conditional extra bus access with specific behavior.

### Layer 3: `state` -- Instantiate with Operations

States instantiate templates with specific operations. All eight LDA addressing variants are eight one-liners:

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

Compare this to hand-writing 4-8 switch cases per variant, each with manual bus reads, flag updates, and cycle transitions. The DSL version has no room for the "off by one cycle" or "forgot the dummy read" bugs.

### Layer 4: `opcode` -- Map Bytes

```scheme
(opcode #xA9 lda-imm)
(opcode #xA5 lda-zp)
(opcode #xAD lda-abs)
```

Maps bytes to states. The compiler builds a 256-entry lookup table.

## The Interesting Example: Branches

The branch template is the most instructive because it shows how `when` blocks compile to conditional microcode jumps:

```scheme
(template branch-if (cond)
  (cycle (fetch dl pc) (branch-decide cond dl))
  (when branch-taken
    (cycle (dummy pc) (branch-update-pcl dl)))
  (when page-cross
    (cycle (dummy pc) (branch-correct-pch dl))))
```

All eight branch instructions are single-word definitions:

```scheme
(state bpl (branch-if (not n)))
(state bmi (branch-if n))
(state bcc (branch-if (not c)))
(state bcs (branch-if c))
(state bne (branch-if (not z)))
(state beq (branch-if z))
```

Here is what the compiler generates for BPL (cases 440-442 of `cpu_gen.c`):

```c
case 440: /* bpl */
    cpu->DL = cpu->mem_read(cpu, cpu->PC);
    cpu->PC = (cpu->PC + 1) & 0xFFFF;
    if (!((cpu->P & 0x80))) {
        cpu->branch_taken = 1;
        uint16_t _np = (cpu->PC + (int8_t)cpu->DL) & 0xFFFF;
        cpu->page_cross = ((cpu->PC & 0xFF00) != (_np & 0xFF00));
    } else { cpu->branch_taken = 0; cpu->page_cross = 0; }
    if (cpu->branch_taken) { cpu->uPC = 441; return; }
    cpu->uPC = 0; return;
case 441: /* bpl when branch-taken */
    (void)cpu->mem_read(cpu, cpu->PC);
    cpu->PC = (cpu->PC & 0xFF00) | ((cpu->PC + (int8_t)cpu->DL) & 0xFF);
    if (cpu->page_cross) { cpu->uPC = 442; return; }
    cpu->uPC = 0; return;
case 442: /* bpl when page-cross */
    (void)cpu->mem_read(cpu, cpu->PC);
    if ((int8_t)cpu->DL < 0) cpu->PC = (cpu->PC - 0x100) & 0xFFFF;
    else cpu->PC = (cpu->PC + 0x100) & 0xFFFF;
    cpu->uPC = 0; return;
```

Three possible outcomes, three possible cycle counts:
- **Not taken (2 cycles):** Case 440 reads the offset, decides not to branch, jumps to uPC 0.
- **Taken, same page (3 cycles):** Case 440 sets `branch_taken`, falls to 441. Case 441 updates PCL only, sees no page cross, jumps to 0.
- **Taken, page cross (4 cycles):** Case 441 sees page cross, falls to 442. Case 442 fixes PCH.

No cycle counter. No "add 1 if taken, add 1 more if page cross." The variable timing emerges from the conditional microcode structure.

## The NMI Handler Reads Like a Datasheet

Compare the NMI handler DSL to the corresponding timing diagram in the 6502 datasheet:

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

Each line maps 1:1 to a row in the timing diagram. The bus operation, the register manipulation, and the cycle boundary are all visible in one place. You can hold the DSL in one hand and the datasheet in the other and verify them against each other.

## Unofficial Opcodes Compose Naturally

The unofficial opcode DCP (decrement memory, then compare) is a RMW instruction. In the DSL, its core is literally DEC followed by CMP:

```scheme
(state dcp-zp
  (fetch-zp-addr) (read-to-dl)
  (cycle (write ad dl) (dec dl))
  (cycle (write ad dl) (cmp a dl)))
```

The first write puts back the original value after decrementing internally. The second write puts back the decremented result. This matches the real silicon behavior where RMW instructions write the unmodified value, perform the operation, then write the result. The DSL makes this double-write visible and explicit.

All seven DCP addressing variants follow the same pattern -- take the addressing mode's setup cycles, append the DEC+CMP write pair:

```scheme
(state dcp-zp  (fetch-zp-addr) (read-to-dl) ...)
(state dcp-zpx (fetch-zp-addr) (dummy-add-x) (read-to-dl) ...)
(state dcp-abs (fetch-adl) (fetch-adh) (read-to-dl) ...)
(state dcp-abx (fetch-adl) (fetch-adh-add-x) (fixup-page-read) (read-to-dl) ...)
```

The same holds for ISC (INC + SBC), SLO (ASL + ORA), RLA (ROL + AND), SRE (LSR + EOR), and RRA (ROR + ADC). Each combines two official operations, and the DSL makes the composition transparent.

## The Compiler

The compiler is 904 lines of Chicken Scheme. There are no clever tricks.

**Template expansion** is textual substitution. When the compiler sees `(read-abs load-nz a)`, it looks up the `read-abs` template, substitutes `op` with `load-nz` and `reg` with `a`, then recursively expands any nested templates. The result is a flat sequence of `(cycle ...)` forms.

**uPC assignment** is sequential. The compiler walks states in definition order. Each state gets a contiguous range of case labels. The `fetch` state is always case 0.

**`when` blocks** compile to conditional jumps. The cycle before a `when` block emits: "if condition is true, go to the when's first case; else skip past all consecutive when blocks to uPC 0." This is how variable-timing instructions work without any runtime cycle counter.

The whole thing is simple enough to understand in an afternoon.

## Results

1,401 lines of DSL generate 5,300 lines of C containing 834 microcode steps. The generated `cpu_gen.c` compiles to a tight jump table that modern C compilers optimize well.

The accuracy test suite (AccuracyCoin) passes 119 of 138 tests (86%). The remaining failures are not CPU timing bugs -- they are in system integration: DMC DMA cycle-stealing edge cases and sprite evaluation timing, areas where the complexity is in how the CPU, PPU, and APU interact, not in the CPU's own instruction timing.

When a timing bug is found, the fix is typically a one-line change to the DSL -- adding a missing `(snapshot-i)`, reordering operations within a cycle, or adding a `(when page-cross ...)` block. The C code regenerates automatically. The specification is the single source of truth.

The DSL approach means an entire class of bugs -- miscounted cycles, forgotten dummy reads, wrong RMW write sequences, misplaced flag updates -- simply cannot occur. The template structure makes correctness compositional: if the template is right, every instruction using that template is right. You verify the addressing mode once, not once per opcode.
