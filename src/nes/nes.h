/*
 * NES System Integration
 *
 * Ties together CPU, PPU, and memory into a complete NES system.
 * Handles memory mapping, timing, and inter-component communication.
 */

#ifndef NES_H
#define NES_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ============================================================================
 * Include component headers BEFORE the NES struct so we can embed them.
 * ============================================================================ */

#include "nes/hooks.h"
#include "nes/cpu_opnames.h"
#include "cpu_gen.h"
#include "ppu/ppu.h"
#include "nes/apu.h"
#include "nes/mapper.h"

/* ============================================================================
 * NES System State
 * ============================================================================ */

typedef struct NES NES;

struct NES {
    /* Components — embedded by value */
    CPU cpu;
    PPU ppu;
    APU apu;
    Mapper mapper;
    bool mapper_loaded;  /* replaces NULL checks on old mapper pointer */

    /* System RAM (2KB, mirrored 4x to fill $0000-$1FFF) */
    uint8_t ram[0x800];

    /* Legacy cartridge fields - kept for compatibility, mapper now handles these */
    uint8_t *prg_rom;       /* Program ROM (CPU $8000-$FFFF) */
    uint32_t prg_rom_size;
    uint8_t *chr_rom;       /* Character ROM (PPU $0000-$1FFF) */
    uint32_t chr_rom_size;
    uint8_t prg_ram[0x2000]; /* Cartridge RAM ($6000-$7FFF) */

    /* Controller state */
    uint8_t controller[2];      /* Current button state */
    uint8_t controller_shift[2]; /* Shift register for serial read */
    uint8_t controller_strobe;  /* Strobe state (bit 0) */

    /* DMA Controller State */
    struct {
        /* OAM DMA */
        bool oam_pending;        /* OAM DMA requested */
        uint8_t oam_page;        /* Source page for OAM DMA */
        bool oam_active;         /* OAM DMA in progress */
        uint16_t oam_cycle;      /* Current cycle within OAM DMA (0-513) */

        /* DMC DMA */
        uint8_t dmc_cycle;       /* Current cycle within DMC DMA (0-3) */
        bool dmc_active;         /* DMC DMA in progress */

        /* RDY line - when low, CPU is halted */
        uint16_t halt_addr;
        uint8_t oam_data;
        bool oam_data_ready;
        bool cpu_halted;         /* CPU cannot execute (DMA owns bus) */
    } dma;

    /* Legacy DMA fields for compatibility */
    bool oam_dma_pending;
    uint8_t oam_dma_page;

    /* Open bus - last value on data bus */
    uint8_t open_bus;
    uint8_t internal_bus;

    /* Master Clock Scheduler.
     *
     * The NES has a single master clock at 21.477272 MHz (NTSC).
     * Component dividers:
     *   - CPU: master / 12 → 1.789 MHz (1 CPU cycle = 12 master ticks)
     *   - PPU: master / 4  → 5.369 MHz (1 PPU dot = 4 master ticks, 3 dots/CPU cycle)
     *   - PAL uses a 26.601712 MHz master: CPU /16 and PPU /5.
     *   - APU: same divider as CPU; runs in lockstep
     *
     * The master_tick counter is the canonical NES time. Within a single
     * CPU cycle (12 master ticks), components fire at specific tick offsets:
     *   - PPU dot work: ticks 0, 4, 8 (modulo ppu_phase_offset)
     *   - CPU bus access: tick cpu_phase_offset (default 4 → "align=1" semantics)
     *   - APU: same as CPU
     *
     * cpu_phase_offset values 0..11 select sub-cycle CPU bus position:
     *   0      : CPU runs at the very start of a CPU cycle (before any PPU dot)
     *   1..3   : CPU runs in the first slot (between dot 0 and dot 1)
     *   4..7   : CPU runs in the second slot (between dot 1 and dot 2)
     *   8..11  : CPU runs in the third slot (between dot 2 and end of cycle)
     *
     * Default cpu_phase_offset = 4 reproduces the legacy "align=1" interleave
     * (PPU dot 0, CPU, PPU dot 1, PPU dot 2). cpu_align is kept as a coarse
     * alias (0/1/2) and is mapped to cpu_phase_offset 0/4/8 at config time. */
    uint64_t master_tick;        /* Total master clock ticks elapsed */
    uint64_t master_clock;       /* Legacy alias - kept for HOOK_FRAME compatibility */
    uint8_t cpu_divider;         /* Counts 0-2, CPU ticks when wraps */

    /* CPU/PPU clock alignment (0, 1, or 2): coarse alias kept for backward
     * compatibility with the older API and existing tests. Real master-tick
     * granularity is expressed via cpu_phase_offset.
     *   0: CPU step happens BEFORE the 3 PPU steps
     *   1: CPU step happens AFTER 1 PPU step (legacy default)
     *   2: CPU step happens AFTER 2 PPU steps */
    uint8_t cpu_align;
    uint8_t cpu_phase_offset;   /* Master tick offset within CPU cycle (0..11) */

    /* Mirroring mode (0=horizontal, 1=vertical) */
    uint8_t mirroring;

    /* NMI edge detection */
    bool prev_nmi;
    bool nmi_edge_detected;

    /* IRQ inhibit — after CLI/PLP clears I flag, IRQ is delayed 1 instruction */
    uint8_t irq_inhibit_cycles;

    /* Mapper scanline tracking (for MMC3 IRQ) */
    uint16_t mapper_last_scanline;

    /* Instruction trace: PC at the start of the current instruction */
    uint16_t last_instr_pc;
};

/* ============================================================================
 * Memory Map Constants
 * ============================================================================ */

/* CPU Memory Map:
 * $0000-$07FF  2KB internal RAM
 * $0800-$1FFF  Mirrors of $0000-$07FF
 * $2000-$2007  PPU registers
 * $2008-$3FFF  Mirrors of $2000-$2007
 * $4000-$4017  APU and I/O registers
 * $4018-$401F  APU and I/O (normally disabled)
 * $4020-$5FFF  Cartridge expansion
 * $6000-$7FFF  Cartridge RAM
 * $8000-$FFFF  Cartridge PRG ROM
 */

/* ============================================================================
 * Memory Access Functions
 * ============================================================================ */

static inline uint8_t nes_cpu_read(CPU *cpu, uint16_t addr) {
    NES *nes = (NES *)cpu->user_data;
    uint8_t val;

    if (addr < 0x2000) {
        val = nes->ram[addr & 0x07FF];
        nes->open_bus = val;
    }
    else if (addr < 0x4000) {
        /* PPUSTATUS latches VBlank at M2 rise, whereas OAMDATA and the
         * sprite status bits remain driven until M2 falls (~two dots). */
        if ((addr & 7) == 4 || (addr & 7) == 7)
            ppu_advance_to_master_tick(&nes->ppu, nes->master_tick + nes->cpu_phase_offset +
                (nes->ppu.region == PPU_REGION_PAL ? 9 : 7));
        val = ppu_reg_read(&nes->ppu, addr);
        if ((addr & 7) == 2) {
            ppu_advance_to_master_tick(&nes->ppu, nes->master_tick + nes->cpu_phase_offset +
                (nes->ppu.region == PPU_REGION_PAL ? 9 : 7));
            val = (val & 0x9F) | (nes->ppu.status & 0x60);
            ppu_refresh_decay(&nes->ppu, val);
        }
        nes->open_bus = val;
    }
    else if (addr < 0x4018) {
        /* APU/I/O reads: open bus is NOT updated (bits mix in from existing bus) */
        if (addr == 0x4016) {
            if (nes->controller_strobe) {
                val = nes->controller[0] & 0x01;
            } else {
                val = nes->controller_shift[0] & 0x01;
                nes->controller_shift[0] >>= 1;
                nes->controller_shift[0] |= 0x80;
            }
            val = (val & 0x1F) | (nes->open_bus & 0xE0);
        }
        else if (addr == 0x4017) {
            if (nes->controller_strobe) {
                val = nes->controller[1] & 0x01;
            } else {
                val = nes->controller_shift[1] & 0x01;
                nes->controller_shift[1] >>= 1;
                nes->controller_shift[1] |= 0x80;
            }
            val = (val & 0x1F) | (nes->open_bus & 0xE0);
        }
        else if (addr == 0x4015) {
            val = apu_read_status(&nes->apu);
            val = (val & 0xDF) | (nes->internal_bus & 0x20);
        }
        else {
            val = nes->open_bus;
        }
    }
    else if (addr < 0x6000 && (!nes->mapper_loaded || nes->mapper.number != 5)) {
        val = nes->open_bus;
    }
    else {
        if (nes->mapper_loaded) {
            val = mapper_cpu_read(&nes->mapper, addr);
        } else if (addr < 0x8000) {
            val = nes->prg_ram[addr & 0x1FFF];
        } else if (nes->prg_rom) {
            val = nes->prg_rom[(addr - 0x8000) % nes->prg_rom_size];
        } else {
            val = nes->open_bus;
        }
        nes->open_bus = val;
    }

    if (addr == 0x4016 || addr == 0x4017) nes->open_bus = val;
    if (cpu->rdy) nes->internal_bus = val;
    HOOK_MEM_ACCESS(addr, val, false);
    return val;
}

static inline void nes_cpu_write(CPU *cpu, uint16_t addr, uint8_t val) {
    NES *nes = (NES *)cpu->user_data;

    /* All writes update the data bus */
    nes->open_bus = val;
    nes->internal_bus = val;
    HOOK_MEM_ACCESS(addr, val, true);

    if (addr < 0x2000) {
        /* Internal RAM (mirrored) */
        nes->ram[addr & 0x07FF] = val;
    }
    else if (addr < 0x4000) {
        /* PPU registers (mirrored every 8 bytes) */
        ppu_reg_write(&nes->ppu, addr, val);
    }
    else if (addr < 0x4018) {
        /* APU and I/O */
        if (addr == 0x4014) {
            /* DMA waits for a CPU read cycle. Consecutive RMW writes
             * therefore update the page before the CPU can be halted. */
            nes->oam_dma_pending = true;
            nes->oam_dma_page = val;
        }
        else if (addr == 0x4016) {
            /* Controller strobe. The actual latching of the button state
             * into the shift register happens on APU "put" cycles while
             * strobe is high (see nes_step's per-cycle strobe handler).
             * This matches real hardware: a 1-cycle strobe pulse that
             * happens to fall between two put cycles never strobes at
             * all (AccuracyCoin Controller Strobing test 4). */
            nes->controller_strobe = val & 1;
        }
        else {
            /* APU registers ($4000-$4013, $4015, $4017) */
            apu_write(&nes->apu, addr, val);
        }
    }
    else if (addr >= 0x5000) {
        /* Cartridge space ($5000-$FFFF) - handled by mapper */
        if (nes->mapper_loaded) {
            mapper_cpu_write(&nes->mapper, addr, val);
            /* Propagate any mapper-controlled mirroring changes */
            nes->ppu.mirroring = mapper_get_mirroring(&nes->mapper);
            nes->mirroring = nes->ppu.mirroring;
        } else if (addr < 0x8000) {
            /* Legacy fallback for PRG RAM */
            nes->prg_ram[addr & 0x1FFF] = val;
        }
        /* ROM writes without mapper are ignored */
    }
}

/* ============================================================================
 * PPU Memory Access (for CHR ROM/RAM)
 * ============================================================================ */

static inline uint8_t nes_ppu_read(PPU *ppu, uint16_t addr) {
    NES *nes = (NES *)ppu->user_data;

    if (addr < 0x2000 || (ppu->cart_nametables && addr < 0x3F00)) {
        /* Pattern tables, plus cartridge-owned nametables. */
        if (nes->mapper_loaded) {
            nes->mapper.ppu_dot = ppu->dot;
            return mapper_ppu_read(&nes->mapper, addr);
        }
        /* Legacy fallback */
        if (nes->chr_rom && nes->chr_rom_size > 0) {
            return nes->chr_rom[addr % nes->chr_rom_size];
        }
        /* CHR RAM fallback */
        return ppu->vram[addr];
    }
    return 0;
}

static inline void nes_ppu_write(PPU *ppu, uint16_t addr, uint8_t val) {
    NES *nes = (NES *)ppu->user_data;

    if (addr < 0x2000 || (ppu->cart_nametables && addr < 0x3F00)) {
        /* Pattern tables, plus cartridge-owned nametables. */
        if (nes->mapper_loaded) {
            mapper_ppu_write(&nes->mapper, addr, val);
        } else if (!nes->chr_rom || nes->chr_rom_size == 0) {
            /* Legacy CHR RAM fallback */
            ppu->vram[addr] = val;
        }
    }
}

static inline void nes_ppu_bus_read(PPU *ppu, uint16_t addr) {
    NES *nes = (NES *)ppu->user_data;
    mapper_ppu_bus_read(&nes->mapper, addr);
}

static inline void nes_ppu_address(PPU *ppu, uint16_t addr) {
    NES *nes = (NES *)ppu->user_data;
    if (nes->mapper_loaded)
        mapper_ppu_address(&nes->mapper, addr);
}

/* ============================================================================
 * OAM DMA - Cycle-Stealing Implementation
 * ============================================================================ */

/* DMA uses the same bus phase and master clock as normal CPU accesses.
 * The CPU's address remains fixed while DMA substitutes external addresses. */
static inline uint8_t nes_dma_read(NES *nes, uint16_t addr) {
    bool apu_selected = nes->dma.halt_addr >= 0x4000 && nes->dma.halt_addr < 0x4020;
    uint8_t value = nes->open_bus;
    if (addr < 0x4000 || addr >= 0x4020)
        value = nes_cpu_read(&nes->cpu, addr);
    if (apu_selected) {
        uint16_t reg = 0x4000 | (addr & 0x1F);
        if (reg == 0x4015) {
            value = (apu_read_status(&nes->apu) & 0xDF) | (value & 0x20);
        } else if (reg == 0x4016 || reg == 0x4017) {
            uint8_t controller = nes_cpu_read(&nes->cpu, reg);
            if (addr >= 0x2000) value = controller;
            nes->open_bus = value;
        }
    }
    return value;
}

static inline bool nes_dma_step(NES *nes) {
    bool dmc_request = apu_dmc_needs_sample(&nes->apu);
    bool active = nes->dma.oam_active || nes->dma.dmc_active;
    bool abort = nes->apu.dmc_abort_pending;
    nes->apu.dmc_abort_pending = false;
    if (abort && !active && !dmc_request && !nes->oam_dma_pending) {
        /* Unlike a normal request, an aborted one is lost on a CPU write. */
        if (cpu_next_is_write(&nes->cpu)) return false;
        nes->cpu.rdy = false;
        (void)nes_cpu_read(&nes->cpu, cpu_get_next_read_addr(&nes->cpu));
        return true;
    }
    if (!active && (nes->oam_dma_pending || dmc_request)) {
        if (cpu_next_is_write(&nes->cpu)) return false;
        nes->dma.halt_addr = cpu_get_next_read_addr(&nes->cpu);
        nes->dma.cpu_halted = true;
    }
    if (!active && !nes->dma.cpu_halted) return false;

    nes->cpu.rdy = false;
    bool halted_now = !active;
    if (nes->oam_dma_pending && !nes->dma.oam_active) {
        nes->dma.oam_active = true;
        nes->oam_dma_pending = false;
        nes->dma.oam_page = nes->oam_dma_page;
        nes->dma.oam_cycle = 0;
        nes->dma.oam_data_ready = false;
    }
    if (dmc_request && !nes->dma.dmc_active) {
        nes->dma.dmc_active = true;
        nes->dma.dmc_cycle = 0;
        if (cpu_next_is_sha_dummy_read(&nes->cpu)) nes->cpu.ignore_h = 1;
    }

    bool bus_used = false;
    /* Halt and dummy cycles precede the DMC get; OAM can continue on both. */
    if (nes->dma.dmc_active) {
        if (nes->dma.dmc_cycle >= 2 && !nes->apu.put_cycle) {
            uint8_t sample = nes_dma_read(nes, nes->apu.dmc.current_address);
            apu_dmc_load_sample(&nes->apu, sample);
            nes->dma.dmc_active = false;
            bus_used = true;
        } else {
            nes->dma.dmc_cycle++;
        }
    }
    if (nes->dma.oam_active && !halted_now && !bus_used) {
        if (!nes->apu.put_cycle) {
            uint16_t addr = ((uint16_t)nes->dma.oam_page << 8) | nes->dma.oam_cycle;
            nes->dma.oam_data = nes_dma_read(nes, addr);
            nes->dma.oam_data_ready = true;
            bus_used = true;
        } else if (nes->dma.oam_data_ready) {
            nes->open_bus = nes->dma.oam_data;
            ppu_reg_write(&nes->ppu, 0x2004, nes->dma.oam_data);
            nes->dma.oam_data_ready = false;
            bus_used = true;
            if (++nes->dma.oam_cycle == 256) nes->dma.oam_active = false;
        }
    }
    if (!bus_used) (void)nes_cpu_read(&nes->cpu, nes->dma.halt_addr);
    nes->dma.cpu_halted = nes->dma.oam_active || nes->dma.dmc_active;
    return true;
}

/* ============================================================================
 * System Step - Run one CPU cycle (3 PPU cycles)
 * ============================================================================ */

/* Helper: trace bookkeeping wrapper around cpu_step. Captures the
 * instruction-boundary PC for trace hooks and tracks NMI/IRQ consumption.
 * Inlined into nes_step at the cpu_phase_offset master tick. */
static inline void nes_cpu_step_traced(NES *nes) {
    uint16_t prev_upc = nes->cpu.uPC;
    bool prev_nmi_pending = nes->cpu.nmi_pending;
    bool prev_irq_pending = nes->cpu.irq_pending;

    cpu_step(&nes->cpu);

    /* CPU instruction completed (uPC returned to 0). Avoid the extra opcode
     * lookup, including mapper dispatch, when no trace consumer is installed. */
    if (debug_hooks.on_cpu_step && prev_upc != 0 && nes->cpu.uPC == 0) {
        uint16_t tpc = nes->last_instr_pc;
        uint8_t actual_op;
        if (tpc < 0x2000)
            actual_op = nes->ram[tpc & 0x7FF];
        else if (tpc >= 0x8000 && nes->mapper_loaded)
            actual_op = mapper_cpu_peek(&nes->mapper, tpc);
        else if (tpc >= 0x8000 && nes->prg_rom)
            actual_op = nes->prg_rom[(tpc - 0x8000) % nes->prg_rom_size];
        else
            actual_op = nes->cpu.IR;
        HOOK_CPU_STEP(nes->last_instr_pc, actual_op, nes->cpu.cycles);
    }
    if (nes->cpu.uPC == 0) {
        nes->last_instr_pc = nes->cpu.PC;
    }
    if (prev_nmi_pending && !nes->cpu.nmi_pending) {
        HOOK_CPU_IRQ(true, 0xFFFA, nes->cpu.PC);
    }
    if (prev_irq_pending && !nes->cpu.irq_pending) {
        HOOK_CPU_IRQ(false, 0xFFFE, nes->cpu.PC);
    }
}

static inline void nes_step(NES *nes) {
    /* Step APU FIRST so IRQ is set before CPU checks for interrupts */
    apu_step(&nes->apu);

    /* Controller strobe handling. Real hardware: while $4016 bit 0 is high,
     * the shift registers are reloaded from the controller buttons on every
     * APU "put" cycle. A 1-cycle strobe pulse that falls between two put
     * cycles never strobes at all — AccuracyCoin Controller Strobing test 4
     * relies on this. */
    if (nes->apu.put_cycle && nes->controller_strobe) {
        nes->controller_shift[0] = nes->controller[0];
        nes->controller_shift[1] = nes->controller[1];
    }

    /* IRQ line — level-sensitive, reflects current state of all sources.
     * On real hardware, the CPU /IRQ pin is active-low and directly driven
     * by all IRQ sources ORed together. The CPU samples this at each
     * instruction boundary. */
    nes->cpu.irq_pending = nes->apu.frame_irq_pending ||
                            nes->apu.dmc_irq_pending ||
                            (nes->mapper_loaded && nes->mapper.irq_pending);

    /* ============================================================
     * Master-clock-driven CPU/PPU interleave
     * ============================================================
     *
     * Each nes_step advances one CPU cycle: 12 NTSC or 16 PAL master ticks.
     * PAL PPU dots use five ticks, preserving the hardware 16:5 ratio.
     * The following offsets describe the NTSC schedule:
     * Within these 12 ticks, components run at well-defined offsets:
     *   - PPU dot work at master tick offset 0, 4, 8 (3 dots per cycle)
     *   - CPU bus access at master tick offset cpu_phase_offset (0..11)
     *
     * The legacy cpu_align (0/1/2) maps to cpu_phase_offset (0/4/8)
     * and reproduces the original interleave exactly:
     *   align=0 → offset 0:  cpu, ppu(0), ppu(4), ppu(8)
     *   align=1 → offset 4:  ppu(0), cpu, ppu(4), ppu(8)
     *   align=2 → offset 8:  ppu(0), ppu(4), cpu, ppu(8)
     *
     * Finer offsets in 1..3, 5..7, 9..11 are accepted but currently
     * behave the same as the slot they fall in (PPU work is atomic
     * per dot in our model). They reserve sub-dot resolution for
     * future master-clock-aware PPU events.
     *
     * For now we just unroll the schedule. The cpu_phase_offset is
     * recomputed from cpu_align at config time so callers using the
     * legacy API see no behavior change. */
    {
        uint64_t cpu_cycle_start = nes->master_tick;
        uint64_t cpu_bus_tick = cpu_cycle_start + nes->cpu_phase_offset;

        /* Advance PPU forward to (but not past) cpu_bus_tick.
         * After this, the PPU's state reflects all dots whose start tick
         * is < cpu_bus_tick — the state the cpu sees at bus access. */
        ppu_advance_to_master_tick(&nes->ppu, cpu_bus_tick);

        /* PPU activity before the CPU bus phase can assert a mapper IRQ
         * (notably an MMC3 A12 edge). Refresh the level-sensitive IRQ line
         * here so the CPU can sample it in this cycle. */
        nes->cpu.irq_pending = nes->apu.frame_irq_pending ||
                               nes->apu.dmc_irq_pending ||
                               (nes->mapper_loaded && nes->mapper.irq_pending);

        if (nes->mapper_loaded && (nes->mapper.number == 5 || nes->mapper.number == 69))
            mapper_cpu_clock(&nes->mapper);

        bool dma_cycle = nes_dma_step(nes);
        nes->cpu.rdy = !dma_cycle;
        nes_cpu_step_traced(nes);
        nes->cpu.rdy = !nes->dma.cpu_halted;

        /* DMA and CPU work share this single CPU cycle. */
        nes->master_tick = cpu_cycle_start +
            (nes->ppu.region == PPU_REGION_PAL ? 16 : 12);
        ppu_advance_to_master_tick(&nes->ppu, nes->master_tick);
    }

    /* Legacy once-per-scanline notification for mappers whose current
     * implementation uses it. MMC3 is driven separately by PPU A12. */
    if (nes->mapper_loaded && nes->mapper.number != 4 && nes->mapper.number != 5 &&
        nes->ppu.scanline < 240 &&
        nes->ppu.dot >= 260 &&
        nes->ppu.scanline != nes->mapper_last_scanline &&
        (nes->ppu.mask & (MASK_BG_ENABLE | MASK_SPRITE_ENABLE))) {
        nes->mapper_last_scanline = nes->ppu.scanline;
        mapper_notify_scanline(&nes->mapper);
        /* IRQ propagation happens via the level-sensitive check above
         * on the next nes_step cycle. No need for immediate propagation. */
    }
    /* Reset tracker at start of new frame (pre-render scanline) */
    if (nes->ppu.scanline >= 241) {
        nes->mapper_last_scanline = 0xFFFF;
    }

    /* Sync PPU mirroring from mapper (mappers like 1, 4, 7 can change it dynamically) */
    if (nes->mapper_loaded) {
        nes->ppu.mirroring = mapper_get_mirroring(&nes->mapper);
    }

    /* Check for NMI suppression from $2002 read at VBL-set cycle.
     * When $2002 is read in the suppression window, we must:
     * 1. Clear any pending edge detection
     * 2. Clear any pending NMI on the CPU (not yet executed)
     * 3. Skip edge detection this cycle (even if nmi_output is high)
     * 4. Update prev_nmi to prevent false edge on next cycle
     */
    if (nes->ppu.suppress_nmi_edge) {
        nes->nmi_edge_detected = false;
        nes->cpu.nmi_pending = false;
        nes->cpu.nmi_armed = false;
        nes->cpu.nmi_sampled = false;
        nes->prev_nmi = nes->ppu.nmi_output;  /* Prevent edge detection */
        nes->ppu.suppress_nmi_edge = false;
        nes->ppu.nmi_edge_pending = false;  /* Suppression beats sticky edge */
    } else {
        /* Sticky rising-edge latch from PPU. The PPU sets this when its
         * internal nmi_output transitions low→high — either inside ppu_step
         * (e.g., dot 1 of sl 241 sets VBL when NMI is already enabled) or
         * inside ppu_reg_write to PPUCTRL (STA $2000 sets NMI enable while
         * VBL is still pending). Captures brief edges that may be cleared
         * again by a later ppu_step in the same nes_step iteration. */
        if (nes->ppu.nmi_edge_pending) {
            nes->nmi_edge_detected = true;
            nes->ppu.nmi_edge_pending = false;
        }

        /* Cancel an edge that disappears before reaching the CPU latch. */
        if (nes->ppu.nmi_output && !nes->prev_nmi) {
            nes->nmi_edge_detected = true;
        } else if (!nes->ppu.nmi_output && nes->nmi_edge_detected) {
            /* NMI output went low - cancel pending edge */
            nes->nmi_edge_detected = false;
        }
        nes->prev_nmi = nes->ppu.nmi_output;
    }

    /* Latch the edge. The CPU polls it at the instruction's interrupt
     * sampling cycle; fetching an opcode does not itself poll NMI. */
    if (nes->nmi_edge_detected) {
        nes->cpu.nmi_pending = true;
        nes->nmi_edge_detected = false;
    }

    nes->master_clock++;

    /* Frame completion hook */
    if (nes->ppu.frame_complete) {
        HOOK_FRAME(nes->ppu.frame, nes->master_clock);
    }
}

/* Run until frame is complete */
static inline void nes_run_frame(NES *nes) {
    nes->ppu.frame_complete = false;

    while (!nes->ppu.frame_complete) {
        nes_step(nes);
    }
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

static inline void nes_init(NES *nes) {
    memset(nes, 0, sizeof(NES));

    nes->mapper_loaded = false;  /* No mapper loaded yet */

    /* Default CPU/PPU alignment. cpu_phase_offset = 5 in the master-tick
     * driven model means the CPU bus access master tick is 5, after PPU
     * dots at master ticks 0 and 4 have been processed, before dot at
     * tick 8. This is the fixed power-on CPU/PPU phase used by frontends.
     * Override via nes_set_cpu_align() or nes_set_cpu_phase_offset(). */
    nes->cpu_align = 2;
    nes->cpu_phase_offset = 5;

    /* Initialize CPU */
    cpu_init(&nes->cpu);
    nes->cpu.mem_read = nes_cpu_read;
    nes->cpu.mem_write = nes_cpu_write;
    nes->cpu.user_data = nes;

    /* Trigger reset to load reset vector. S powers on at $00; the reset
     * sequence's three suppressed pushes leave it at $FD. */
    nes->cpu.SP = 0x00;
    nes->cpu.reset_pending = true;

    /* Initialize PPU */
    ppu_init(&nes->ppu);
    nes->ppu.cart_read = nes_ppu_read;
    nes->ppu.cart_write = nes_ppu_write;
    nes->ppu.cart_address = nes_ppu_address;
    nes->ppu.user_data = nes;

    /* Initialize APU */
    apu_init(&nes->apu);
}

static inline void nes_reset(NES *nes) {
    /* Reset CPU - trigger reset sequence in microcode. reset_pending is only
     * polled at uPC 0, so restart the microcode there; otherwise a CPU stuck
     * in KIL/JAM (or mid-DMA) never services the reset. */
    nes->cpu.uPC = 0;
    nes->cpu.reset_pending = true;
    nes->cpu.rdy = true;
    nes->cpu.irq_pending = nes->cpu.nmi_pending = false;
    nes->cpu.irq_sampled = nes->cpu.nmi_sampled = 0;
    nes->cpu.irq_armed = nes->cpu.nmi_armed = 0;
    memset(&nes->dma, 0, sizeof(nes->dma));
    nes->oam_dma_pending = false;
    nes->prev_nmi = nes->nmi_edge_detected = false;
    nes->irq_inhibit_cycles = 0;

    /* Reset PPU */
    ppu_reset(&nes->ppu);

    /* Reset APU */
    apu_reset(&nes->apu);

    /* Clear RAM (optional, real NES has random values) */
    memset(nes->ram, 0, sizeof(nes->ram));
}

/* Set CPU/PPU clock alignment (0, 1, or 2). Real hardware has a fixed but
 * power-on-random alignment. Maps to cpu_phase_offset values that, in the
 * master-tick driven scheduler, produce N PPU dots done before the CPU
 * bus access:
 *   align=0 → offset 0   (0 dots done — CPU before any PPU dot)
 *   align=1 → offset 1   (1 dot done — CPU after dot 0)
 *   align=2 → offset 5   (2 dots done — CPU after dots 0 and 1)
 * The master-tick interpretation is: dot D's start tick = D*4. CPU at
 * master tick T sees state with all dots having start_tick < T processed.
 * align=2 maps to offset 5..8 (any value in the slot); we pick 5 as the
 * canonical representative. */
static inline void nes_set_cpu_align(NES *nes, uint8_t align) {
    if (align > 2) align = 2;
    nes->cpu_align = align;
    static const uint8_t align_to_offset[3] = { 0, 1, 5 };
    nes->cpu_phase_offset = align_to_offset[align];
}

/* Set CPU phase offset directly with master-tick resolution (0..11).
 * Allows finer alignment than the legacy 0/1/2 align values. */
static inline void nes_set_cpu_phase_offset(NES *nes, uint8_t offset) {
    if (offset > 11) offset = 11;
    nes->cpu_phase_offset = offset;
    /* Map back to coarse cpu_align for backward compatibility. */
    if (offset == 0)       nes->cpu_align = 0;
    else if (offset <= 4)  nes->cpu_align = 1;
    else                   nes->cpu_align = 2;
}

/* Load PRG ROM */
static inline void nes_load_prg(NES *nes, uint8_t *data, uint32_t size) {
    nes->prg_rom = data;
    nes->prg_rom_size = size;
}

/* Load CHR ROM */
static inline void nes_load_chr(NES *nes, uint8_t *data, uint32_t size) {
    nes->chr_rom = data;
    nes->chr_rom_size = size;
}

/* Load mapper - initializes the embedded Mapper and marks it loaded */
static inline void nes_load_mapper(NES *nes, uint16_t number,
                                    uint8_t *prg_rom, uint32_t prg_size,
                                    uint8_t *chr_rom, uint32_t chr_size,
                                    uint8_t mirroring) {
    mapper_init(&nes->mapper, number, prg_rom, prg_size, chr_rom, chr_size, mirroring);
    nes->mapper.nes = nes;
    nes->mapper_loaded = true;
    nes->ppu.cart_bus_read = number == 5 ? nes_ppu_bus_read : NULL;
    nes->ppu.cart_nametables = number == 5;

    /* Also set legacy fields for compatibility */
    nes->prg_rom = prg_rom;
    nes->prg_rom_size = prg_size;
    nes->chr_rom = chr_rom;
    nes->chr_rom_size = chr_size;
    nes->mirroring = mirroring;

    /* Set initial PPU mirroring */
    nes->ppu.mirroring = mapper_get_mirroring(&nes->mapper);

}

/* ============================================================================
 * Controller Input
 * ============================================================================ */

/* Controller button bits (active high, bit 7 read first) */
#define BTN_A      0x01
#define BTN_B      0x02
#define BTN_SELECT 0x04
#define BTN_START  0x08
#define BTN_UP     0x10
#define BTN_DOWN   0x20
#define BTN_LEFT   0x40
#define BTN_RIGHT  0x80

static inline void nes_set_controller(NES *nes, int player, uint8_t buttons) {
    if (player >= 0 && player < 2) {
        nes->controller[player] = buttons;
    }
}

/* Set region (NTSC or PAL) — affects PPU scanline count and APU clock rate.
 * Must be called before nes_reset(). */
#define NES_REGION_NTSC 0
#define NES_REGION_PAL  1

static inline void nes_set_region(NES *nes, int region) {
    ppu_set_region(&nes->ppu, region);
    apu_set_region(&nes->apu, region);
}

#endif /* NES_H */
