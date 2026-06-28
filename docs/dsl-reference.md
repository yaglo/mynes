# 6502 Microcode DSL Reference

## Overview

The file `src/cpu/nes6502.dsl` (1401 lines) defines the cycle-accurate microcode for a NES 6502 (2A03) CPU. The compiler `tools/dsl2c.scm` (Chicken Scheme, 904 lines) reads this DSL and generates two files:

- `generated/cpu_gen.h` — CPU struct definition, `cpu_init()`, function declarations
- `generated/cpu_gen.c` — `cpu_step()` function (~5300 lines) containing a `switch(cpu->uPC)` with 834 microcode steps, plus DMA helper functions

Each call to `cpu_step()` advances the CPU by exactly one clock cycle. The microcode program counter (`uPC`) determines which case executes. After the last cycle of an instruction, `uPC` returns to 0 (the `fetch` state) to begin the next instruction.

## Top-Level Constructs

The DSL has four top-level forms.

### `defcycle` -- Named Single-Cycle Patterns

Defines a reusable single-cycle macro. When referenced by name inside a `state` or `template`, it expands to `(cycle ops...)`.

```scheme
(defcycle name op1 op2 ...)
```

Examples from the DSL:

```scheme
(defcycle read-to-dl         (read dl ad))
(defcycle fetch-adl          (fetch adl pc))
(defcycle fetch-zp-addr      (fetch adl pc) (set-adh-zero))
(defcycle dummy-add-x        (dummy ad) (add8-latch-carry adl adl x))
(defcycle fixup-page-read    (read dl ad) (adc8-from-pagecross adh adh))
(defcycle write-nz-dl        (write ad dl) (nz dl))
```

A `defcycle` is invoked by wrapping its name in parentheses as a standalone form:

```scheme
(fetch-adl)          ;; expands to: (cycle (fetch adl pc))
(fixup-page-read)    ;; expands to: (cycle (read dl ad) (adc8-from-pagecross adh adh))
```

### `template` -- Parameterized Multi-Cycle Patterns

Defines a reusable multi-cycle addressing mode pattern with named parameters. Parameters are substituted textually at expansion time.

```scheme
(template name (param1 param2 ...)
  cycle-or-defcycle-ref ...
  cycle-or-defcycle-ref ...)
```

Templates can contain `(cycle ...)` forms, `defcycle` references, `(when ...)` blocks, and calls to other templates. Parameters appear as bare symbols inside operations and are replaced with the arguments at the call site.

Examples:

```scheme
;; Immediate: 2 cycles total (1 cycle here + 1 for fetch)
(template read-imm (op reg)
  (cycle (fetch dl pc) (op reg dl)))

;; Absolute,X: 4+ cycles (page cross adds 1)
(template read-abx (op reg)
  (fetch-adl)
  (fetch-adh-add-x)
  (cycle (read dl ad) (op reg dl))
  (when page-cross
    (cycle (adc8-from-pagecross adh adh) (read dl ad) (op reg dl))))

;; Zero Page RMW: 5 cycles
(template rmw-zp (op)
  (fetch-zp-addr)
  (read-to-dl)
  (cycle (write ad dl) (op dl))
  (write-nz-dl))
```

Templates can also wrap ALU operations for use as the `op` parameter in addressing mode templates:

```scheme
(template load-nz (dst src)
  (mov dst src)
  (nz dst))

(template do-adc (reg mem)
  (adc reg mem))
```

When a template like `load-nz` is passed as the `op` parameter to `read-imm`, the operations `(mov dst src) (nz dst)` are spliced into the cycle alongside the bus operation.

### `state` -- Instruction/Handler Definition

Defines a named microcode sequence. A state can use templates, defcycle references, inline cycles, control flow, and conditional blocks.

```scheme
(state name body ...)
```

**Simple states using templates** -- one line per instruction variant:

```scheme
(state lda-imm (read-imm load-nz a))
(state lda-abs (read-abs load-nz a))
(state inc-zp  (rmw-zp inc))
(state asl-abs (rmw-abs asl))
```

**States with explicit cycles:**

```scheme
(state tax (cycle (dummy pc) (mov x a) (nz x)))

(state pha
  (dummy-read-pc)
  (cycle (write sp a) (sp-1)))
```

**States with control flow:**

```scheme
(state fetch
  (cycle (poll-interrupts) (fetch ir pc))
  (dispatch))

(state nmi-handler
  (cycle (dummy pc))
  (cycle (write sp pch) (sp-1))
  (cycle (write sp pcl) (sp-1))
  (cycle (prep-push-p nmi) (write sp dl) (sp-1))
  (cycle (set-flag i) (snapshot-i) (read adl vec-nmi-lo))
  (cycle (read adh vec-nmi-hi) (mov pcl adl) (mov pch adh))
  (goto fetch))
```

Every state implicitly ends with `(goto fetch)` unless it contains an explicit `(goto ...)` or `(dispatch)`.

### `opcode` -- Byte-to-State Mapping

Maps a hex byte value to a state name. All 256 byte values must be mapped.

```scheme
(opcode #xA9 lda-imm)
(opcode #xAD lda-abs)
(opcode #x00 brk)
(opcode #xEA nop)
```

The compiler builds a 256-entry lookup table (`cpu_entry[256]`) that maps opcode bytes to the starting `uPC` of each state.

## Cycle-Level Operations

Every `(cycle ...)` form contains one or more operations that execute within a single clock cycle. One operation performs a bus access; the rest are internal register/flag manipulations.

### Bus Operations

Each cycle has exactly one bus operation (read, write, or dummy read).

| Operation | Description |
|-----------|-------------|
| `(fetch dest pc)` | Read byte at PC into `dest`, increment PC |
| `(read dest addr)` | Read byte at `addr` into `dest` |
| `(write addr src)` | Write `src` to `addr` |
| `(dummy addr)` | Read from `addr`, discard result (timing only) |

`dest` and `src` are register names. `addr` is an address expression.

### Address Expressions

| Expression | C expansion | Description |
|------------|-------------|-------------|
| `pc` | `cpu->PC` | Program counter (16-bit) |
| `ad` | `(ADH << 8) \| ADL` | Effective address from ADH:ADL |
| `sp` | `0x0100 \| SP` | Stack address (page 1) |
| `vec-reset-lo` | `0xFFFC` | Reset vector low byte |
| `vec-reset-hi` | `0xFFFD` | Reset vector high byte |
| `vec-nmi-lo` | `0xFFFA` | NMI vector low byte |
| `vec-nmi-hi` | `0xFFFB` | NMI vector high byte |
| `vec-irq-lo` | `0xFFFE` | IRQ vector low byte |
| `vec-irq-hi` | `0xFFFF` | IRQ vector high byte |
| `vec-irq-hijack-lo` | NMI or IRQ vector | Uses NMI vector if `nmi_pending` (hijacking) |
| `vec-irq-hijack-hi` | NMI or IRQ vector | Same, also clears `nmi_pending` |

### Register Names

| Name | Field | Description |
|------|-------|-------------|
| `a` | `cpu->A` | Accumulator |
| `x` | `cpu->X` | X index register |
| `y` | `cpu->Y` | Y index register |
| `sp` | `cpu->SP` | Stack pointer (8-bit, page 1 implied) |
| `p` | `cpu->P` | Processor status flags |
| `ir` | `cpu->IR` | Instruction register |
| `dl` | `cpu->DL` | Data latch (temporary) |
| `adl` | `cpu->ADL` | Address low byte |
| `adh` | `cpu->ADH` | Address high byte |
| `pcl` | `cpu->PC & 0xFF` | PC low byte (read); sets low byte (write) |
| `pch` | `cpu->PC >> 8` | PC high byte (read); sets high byte (write) |

### Register Operations

| Operation | Description |
|-----------|-------------|
| `(mov dst src)` | Copy register: `dst = src` |
| `(nz reg)` | Set N and Z flags from register value |
| `(inc reg)` | Increment with wrapping, set N/Z |
| `(dec reg)` | Decrement with wrapping, set N/Z |
| `(inc-nf reg)` | Increment without flag changes (pointer math) |
| `(adc reg mem)` | Add with carry, full flag update (C, Z, N, V) |
| `(sbc reg mem)` | Subtract with borrow, full flag update (C, Z, N, V) |
| `(and reg mem)` | Logical AND, set N/Z |
| `(ora reg mem)` | Logical OR, set N/Z |
| `(eor reg mem)` | Logical XOR, set N/Z |
| `(cmp reg mem)` | Compare (subtract without storing), set C/Z/N |
| `(bit reg mem)` | Bit test: Z from AND, N/V copied from memory bits 7/6 |
| `(asl reg)` | Arithmetic shift left, set C/N/Z |
| `(lsr reg)` | Logical shift right, set C/N/Z |
| `(rol reg)` | Rotate left through carry, set C/N/Z |
| `(ror reg)` | Rotate right through carry, set C/N/Z |

### Address Math

| Operation | Description |
|-----------|-------------|
| `(add8-latch-carry dst a b)` | 8-bit add `a + b` into `dst`, set `page_cross` on carry |
| `(adc8-from-pagecross dst src)` | Add 1 to `dst` if `page_cross` is set, clear flag |
| `(set-adh-zero)` | Set ADH = 0 (zero-page addressing) |

### Flag Operations

| Operation | Description |
|-----------|-------------|
| `(set-flag f)` | Set flag `f` (one of: `c`, `z`, `i`, `d`, `b`, `v`, `n`) |
| `(clear-flag f)` | Clear flag `f` |
| `(snapshot-i)` | Copy current I flag to `effective_i` for delayed IRQ recognition |
| `(set-c-from-n)` | Copy N flag to C (used by ANC unofficial opcode) |

### Stack Operations

| Operation | Description |
|-----------|-------------|
| `(sp+1)` | Increment stack pointer |
| `(sp-1)` | Decrement stack pointer |
| `(pc+1)` | Increment program counter |
| `(prep-push-p mode)` | Prepare P register for push. Mode: `php` (B set), `brk` (B set, NMI hijack check), `irq`/`nmi` (B clear) |

### Branch Operations

| Operation | Description |
|-----------|-------------|
| `(branch-decide cond offset)` | Evaluate condition, set `branch_taken` and `page_cross` flags. Does NOT update PC. |
| `(branch-update-pcl offset)` | Add signed offset to PCL only (PCH unchanged) |
| `(branch-correct-pch offset)` | Fix PCH after page-crossing branch |

### Control Flow

| Operation | Description |
|-----------|-------------|
| `(dispatch)` | Jump to instruction handler via `cpu_entry[IR]` |
| `(goto state-name)` | Jump to named state's starting uPC |
| `(poll-interrupts)` | Check reset/NMI/IRQ pending (priority order). Only valid in the `fetch` state's first cycle. |

### Unofficial Opcode Operations

| Operation | Description |
|-----------|-------------|
| `(store-ax addr)` | Write `A & X` to address |
| `(arr reg mem)` | AND + ROR with special V/C flag handling |
| `(axs reg mem)` | `(A & X) - mem` into X with carry |
| `(las-op)` | `A = X = SP = mem & SP`, set N/Z |
| `(sha-page-fixup)` | Compute `DL = A & X & (ADH+1)`, corrupt ADH on page cross |
| `(shx-page-fixup)` | Compute `DL = X & (ADH+1)`, corrupt ADH on page cross |
| `(shy-page-fixup)` | Compute `DL = Y & (ADH+1)`, corrupt ADH on page cross |
| `(tas-page-fixup)` | `SP = A & X`, compute `DL = SP & (ADH+1)`, corrupt ADH on page cross |

The page-fixup operations also support the `ignore_h` flag, which is set by the DMA subsystem when a DMA halt lands on the fixup cycle. When set, the `(ADH+1)` term is replaced with `0xFF`, removing the address corruption.

## Conditional Execution

`(when condition ...)` wraps cycles that execute only when a runtime condition holds.

| Condition | Meaning |
|-----------|---------|
| `page-cross` | Indexed address crossed a 256-byte page boundary |
| `branch-taken` | Branch condition was true |
| `nmi-pending` | NMI signal is asserted |

When the condition is false, the cycles inside the `when` block are skipped entirely (the uPC jumps past them).

**Branch template** -- demonstrates all three conditions interacting:

```scheme
(template branch-if (cond)
  (cycle (fetch dl pc) (branch-decide cond dl))
  (when branch-taken
    (cycle (dummy pc) (branch-update-pcl dl)))
  (when page-cross
    (cycle (dummy pc) (branch-correct-pch dl))))
```

Cycle counts by outcome:
- Not taken: 2 cycles (fetch + decide)
- Taken, same page: 3 cycles (+ PCL update)
- Taken, page cross: 4 cycles (+ PCH correction)

**IRQ handler** -- uses `(when nmi-pending ...)` inline within cycles for NMI hijacking:

```scheme
(state irq-handler
  (cycle (dummy pc) (when nmi-pending (goto nmi-handler)))
  (cycle (write sp pch) (sp-1) (when nmi-pending (goto nmi-handler)))
  ...)
```

## DSL-to-C Translation

### Example 1: LDA immediate

DSL:
```scheme
(state lda-imm (read-imm load-nz a))
```

After template expansion, this becomes a single cycle:
```
cycle: (fetch dl pc) (mov a dl) (nz a)
```

Generated C:
```c
case 22: /* lda-imm */
    cpu->last_read_addr = cpu->PC;
    cpu->DL = cpu->mem_read(cpu, cpu->PC);
    cpu->PC = (cpu->PC + 1) & 0xFFFF;
    cpu->A = cpu->DL;
    { uint8_t v = cpu->A; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
    cpu->uPC = 0; return;
```

One cycle, one case. `uPC = 0` returns to fetch.

### Example 2: INC zero page (read-modify-write, 4 cycles)

DSL:
```scheme
(state inc-zp (rmw-zp inc))
```

After expansion through `rmw-zp`:
```
cycle 1: (fetch adl pc) (set-adh-zero)     -- fetch ZP address
cycle 2: (read dl ad)                       -- read current value
cycle 3: (write ad dl) (inc dl)             -- write old value, do increment
cycle 4: (write ad dl) (nz dl)              -- write new value, set flags
```

Generated C (cases 312-315):
```c
case 312: /* inc-zp */
    cpu->ADL = cpu->mem_read(cpu, cpu->PC);
    cpu->PC = (cpu->PC + 1) & 0xFFFF;
    cpu->ADH = 0;
    cpu->uPC = 313; return;
case 313: /* inc-zp */
    cpu->DL = cpu->mem_read(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL);
    cpu->uPC = 314; return;
case 314: /* inc-zp */
    cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
    { uint8_t n = (cpu->DL + 1) & 0xFF; cpu->DL = n; ... }
    cpu->uPC = 315; return;
case 315: /* inc-zp */
    cpu->mem_write(cpu, ((uint16_t)cpu->ADH << 8) | cpu->ADL, cpu->DL);
    { uint8_t v = cpu->DL; cpu->P = (cpu->P & 0x7D) | (v == 0 ? 0x02 : 0) | (v & 0x80); }
    cpu->uPC = 0; return;
```

Note the RMW double-write: case 314 writes the old value (before incrementing), case 315 writes the new value. This matches real hardware behavior and is structurally enforced by the `rmw-zp` template.

### Example 3: BPL (conditional branch, variable timing)

DSL:
```scheme
(state bpl (branch-if (not n)))
```

Generated C (cases 440-442):
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

The `when` blocks compile to conditional `uPC` jumps. When the branch is not taken, case 440 jumps directly to `uPC = 0`. When taken without page cross, case 441 jumps to 0. Only on page cross does case 442 execute.

## Generated Code Structure

### CPU struct

```c
typedef struct CPU {
    uint8_t A, X, Y, SP, P, IR, DL, ADL, ADH;
    uint16_t PC, uPC;
    bool page_cross, branch_taken;
    bool irq_pending, nmi_pending, reset_pending;
    bool rdy;
    uint16_t last_read_addr;
    uint8_t effective_i;
    uint8_t ignore_h;
    uint8_t irq_armed;
    uint64_t cycles;
    uint8_t (*mem_read)(struct CPU *cpu, uint16_t addr);
    void (*mem_write)(struct CPU *cpu, uint16_t addr, uint8_t val);
    void *user_data;
} CPU;
```

Key fields beyond standard 6502 registers:

- `uPC` -- microcode program counter, indexes into the switch statement
- `DL`, `ADL`, `ADH` -- internal latches matching real 6502 silicon
- `page_cross`, `branch_taken` -- runtime flags for conditional cycle execution
- `effective_i` -- delayed I flag for correct IRQ timing (1-instruction lag for CLI/SEI/PLP)
- `irq_armed` -- two-stage IRQ recognition pipeline matching hardware poll timing
- `ignore_h` -- DMA interaction flag for SHA/SHX/SHY/TAS address corruption
- `rdy` -- RDY line for DMA halt support
- `last_read_addr` -- last bus read address, used by DMA to re-read the correct address

### Helper Functions

The compiler also generates three DMA support functions by analyzing which cycles perform reads vs writes:

- `cpu_get_next_read_addr(CPU *cpu)` -- returns the address the CPU would read on its next cycle, based on `uPC`. Used by DMA to re-read from the same address.
- `cpu_next_is_write(CPU *cpu)` -- returns true if the next cycle is a write. DMA cannot halt on write cycles.
- `cpu_next_is_sha_dummy_read(CPU *cpu)` -- identifies SHA/SHX/SHY/TAS dummy-read cycles where DMA halting triggers `ignore_h` behavior.

### Entry Table

```c
const uint16_t cpu_entry[256] = { ... };
```

Maps each opcode byte to the starting `uPC` of its state. The `fetch` state reads `IR` and uses `cpu_entry[IR]` via `(dispatch)` to jump to the instruction handler.

## File Organization

| Lines | Section | Content |
|-------|---------|---------|
| 1-58 | Cycle macros | 20 `defcycle` definitions for common single-cycle patterns |
| 60-118 | Core states | `fetch`, `nmi-handler`, `irq-handler`, `nmi-handler-next`, `reset-handler`, `illegal` |
| 120-367 | Templates | Addressing modes (read: 8 modes, store: 8 modes, RMW: 7 modes) + operation wrappers |
| 368-711 | Official instructions | All 56 official 6502 opcodes across all addressing modes |
| 713-1110 | Unofficial opcodes | LAX, SAX, DCP, ISC, SLO, RLA, SRE, RRA, ANC, ALR, ARR, XAA, AXS, STP, NOPs, LAS, TAS, SHA, SHX, SHY |
| 1112-1401 | Opcode mappings | All 256 byte values mapped to state names |

## Addressing Mode Summary

### Read Templates (used by LDA, LDX, ADC, SBC, AND, ORA, EOR, CMP, BIT, etc.)

| Template | Cycles | Parameters |
|----------|--------|------------|
| `read-imm` | 1 | `(op reg)` |
| `read-zp` | 2 | `(op reg)` |
| `read-zpx` | 3 | `(op reg)` |
| `read-zpy` | 3 | `(op reg)` |
| `read-abs` | 3 | `(op reg)` |
| `read-abx` | 3-4 | `(op reg)` -- +1 on page cross |
| `read-aby` | 3-4 | `(op reg)` -- +1 on page cross |
| `read-izx` | 5 | `(op reg)` |
| `read-izy` | 4-5 | `(op reg)` -- +1 on page cross |

Cycle counts exclude the fetch cycle (case 0). Total instruction cycles = template cycles + 1.

### Store Templates (used by STA, STX, STY)

| Template | Cycles | Parameters |
|----------|--------|------------|
| `store-zp` | 2 | `(reg)` |
| `store-zpx` | 3 | `(reg)` |
| `store-zpy` | 3 | `(reg)` |
| `store-abs` | 3 | `(reg)` |
| `store-abx` | 4 | `(reg)` -- always takes penalty cycle |
| `store-aby` | 4 | `(reg)` -- always takes penalty cycle |
| `store-izx` | 5 | `(reg)` |
| `store-izy` | 5 | `(reg)` -- always takes penalty cycle |

### RMW Templates (used by INC, DEC, ASL, LSR, ROL, ROR)

| Template | Cycles | Parameters |
|----------|--------|------------|
| `rmw-zp` | 4 | `(op)` |
| `rmw-zpx` | 5 | `(op)` |
| `rmw-abs` | 5 | `(op)` |
| `rmw-abx` | 6 | `(op)` |
| `rmw-aby` | 6 | `(op)` |
| `rmw-izx` | 7 | `(op)` |
| `rmw-izy` | 7 | `(op)` |

### Branch Template

| Template | Cycles | Parameters |
|----------|--------|------------|
| `branch-if` | 1-3 | `(cond)` -- 1 (not taken), 2 (taken same page), 3 (taken page cross) |

Branch conditions: `c`, `z`, `n`, `v`, `(not c)`, `(not z)`, `(not n)`, `(not v)`.

## Compiler Internals

The compiler (`tools/dsl2c.scm`) operates in these phases:

1. **Parse** -- Reads S-expressions from the DSL file using Scheme's built-in reader.
2. **Collect** -- Separates forms into `*states*`, `*templates*`, `*cycle-macros*`, and `*opcodes*`.
3. **Expand** -- Recursively expands template calls and defcycle references into flat `(cycle ...)` sequences.
4. **Assign uPC** -- Iterates states in definition order, assigning each a contiguous range of uPC values based on cycle count.
5. **Emit** -- Generates C switch cases. Each cycle becomes one case. `when` blocks become conditional uPC jumps.
6. **Emit helpers** -- Generates `cpu_get_next_read_addr()`, `cpu_next_is_write()`, `cpu_next_is_sha_dummy_read()` by analyzing recorded bus operation categories per uPC.
7. **Emit entry table** -- Generates `cpu_entry[256]` mapping opcode bytes to state base uPC values.

Template expansion is pure textual substitution with no type checking. The compiler trusts that operations are well-formed. An undefined operation name will pass through expansion unchanged and trigger a code generation error or produce incorrect C.
