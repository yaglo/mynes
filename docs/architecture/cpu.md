# CPU Architecture

## Overview

The CPU emulates a Ricoh 2A03 (NMOS 6502 without decimal mode). It is
cycle-accurate at the sub-instruction level: each call to `cpu_step()`
advances exactly one microcode cycle.

## DSL Pipeline

```
src/cpu/nes6502.dsl  --(dsl2c.scm)-->  generated/cpu_gen.c  --(gcc)-->  cpu_step()
```

The DSL source defines every 6502 instruction as a sequence of micro-operations
per clock cycle. The Scheme compiler (`tools/dsl2c.scm`) reads this and emits a
single C function with a `switch(cpu->uPC)` state machine.

### Build Integration

CMake invokes Chicken Scheme (`csi`) at build time:

```cmake
add_custom_command(
    OUTPUT ${CPU_GENERATED}
    COMMAND ${CSI_EXECUTABLE} -s ${DSL_COMPILER} ${CPU_DSL_SOURCE} ${CPU_GENERATED}
    DEPENDS ${CPU_DSL_SOURCE} ${DSL_COMPILER}
)
```

If `csi` is not installed, CMake falls back to a pre-generated file at
`build/cpu/cpu_gen.c`.

## DSL Language

The DSL file (`src/cpu/nes6502.dsl`) uses three main constructs:

### Cycle Macros

Reusable single-cycle patterns:

```scheme
(defcycle fetch-adl          (fetch adl pc))
(defcycle fetch-adh          (fetch adh pc))
(defcycle fetch-zp-addr      (fetch adl pc) (set-adh-zero))
(defcycle fixup-page-read    (read dl ad) (adc8-from-pagecross adh adh))
```

### Templates

Addressing mode templates parameterized by operation and register:

```scheme
;; Absolute,X: 4+ cycles (page cross adds 1)
(template read-abx (op reg)
  (fetch-adl)
  (fetch-adh-add-x)
  (cycle (read dl ad) (op reg dl))
  (when page-cross
    (cycle (adc8-from-pagecross adh adh) (read dl ad) (op reg dl))))
```

The `(when page-cross ...)` clause emits the extra cycle only when an 8-bit
add on ADL produced a carry, indicating the indexed address crossed a page
boundary.

### States

Named multi-cycle sequences for interrupt handlers and the fetch state:

```scheme
(state fetch
  (cycle (poll-interrupts) (fetch ir pc))
  (dispatch))

(state nmi-handler
  (cycle (dummy pc))
  (cycle (write sp pch) (sp-1))
  (cycle (write sp pcl) (sp-1))
  (cycle (prep-push-p nmi) (write sp dl) (sp-1))
  (cycle (set-flag i) (read adl vec-nmi-lo))
  (cycle (read adh vec-nmi-hi) (mov pcl adl) (mov pch adh))
  (goto fetch))
```

## Page-Crossing Fix

For read instructions with indexed addressing (absolute,X / absolute,Y /
indirect,Y), the 6502 performs an optimistic read using only the low byte of
the effective address. If the index addition carried into the high byte,
an extra cycle re-reads from the corrected address.

The DSL expresses this with `(when page-cross ...)`:

```scheme
(template read-abx (op reg)
  (fetch-adl)
  (fetch-adh-add-x)                              ;; ADL += X, latch carry
  (cycle (read dl ad) (op reg dl))               ;; optimistic read
  (when page-cross                               ;; only if carry set:
    (cycle (adc8-from-pagecross adh adh)         ;;   ADH += 1
           (read dl ad) (op reg dl))))            ;;   correct read
```

Store instructions always take the penalty cycle (no optimization).

## CPU State

Key fields in the `CPU` struct (defined in `cpu_gen.c`):

| Field | Description |
|-------|-------------|
| `PC` | 16-bit program counter |
| `A, X, Y` | 8-bit accumulator and index registers |
| `SP` | 8-bit stack pointer (offset into page $01) |
| `P` | 8-bit processor flags (NV-BDIZC) |
| `IR` | Instruction register (current opcode) |
| `uPC` | Microcode program counter (0 = instruction boundary) |
| `cycles` | Total CPU cycles elapsed |
| `rdy` | RDY line state (false = halted by DMA) |
| `irq_pending` | IRQ line asserted |
| `nmi_pending` | NMI edge detected |
| `reset_pending` | Reset sequence requested |
| `mem_read` / `mem_write` | Function pointers for bus access |
| `user_data` | Opaque pointer back to NES struct |

## Opcodes

All 256 opcodes are implemented, including unofficial instructions. The
opcode table in `src/nes/cpu_opnames.h` lists mnemonics (unofficial prefixed
with `*`):

- Official: LDA, STA, ADC, SBC, AND, ORA, EOR, CMP, BIT, ASL, LSR, ROL, ROR,
  INC, DEC, JMP, JSR, RTS, RTI, BRK, branches, stack ops, transfers, flag ops
- Unofficial: SLO, SRE, RLA, RRA, SAX, LAX, DCP, ISB, ANC, ALR, ARR, XAA,
  ATX, AXS, SHA, SHX, SHY, TAS, LAS, NOP variants

## Integration with NES

The NES system (`nes.h`) drives the CPU through `cpu_step()` and detects
instruction boundaries by watching `uPC`:

```c
uint16_t prev_upc = nes->cpu->uPC;
cpu_step(nes->cpu);
if (prev_upc != 0 && nes->cpu->uPC == 0) {
    HOOK_CPU_STEP(nes->cpu->PC, nes->cpu->IR, nes->cpu->cycles);
}
```

Memory access is wired through function pointers set during `nes_init()`:

```c
cpu->mem_read = nes_cpu_read;
cpu->mem_write = nes_cpu_write;
cpu->user_data = nes;
```

## Related Files

- `src/cpu/nes6502.dsl` -- microcode source
- `tools/dsl2c.scm` -- DSL compiler
- `generated/cpu_gen.c` -- generated CPU implementation
- `src/nes/cpu_opnames.h` -- opcode name/mode/size tables
- `src/nes/cpu_trace.h` -- nestest-format disassembly
