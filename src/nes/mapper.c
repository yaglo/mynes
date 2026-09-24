/*
 * Mapper dispatcher and lifecycle helpers.
 */

#include "mapper.h"
#include "mappers/mapper_ops.h"
#include <string.h>

/* The single list of implemented mappers: the ROM loader's gate and
 * mapper_supported() both derive from it, so they cannot drift apart. */
static const MapperOps *mapper_ops_lookup(uint16_t number) {
    switch (number) {
    case 0: return &mapper0_ops;
    case 1: return &mapper1_ops;
    case 2: return &mapper2_ops;
    case 3: return &mapper3_ops;
    case 4: return &mapper4_ops;
    case 5: return &mapper5_ops;
    case 7: return &mapper7_ops;
    case 9: return &mapper9_ops;
    case 10: return &mapper10_ops;
    case 11: return &mapper11_ops;
    case 34: return &mapper34_ops;
    case 66: return &mapper66_ops;
    case 69: return &mapper69_ops;
    case 71: return &mapper71_ops;
    case 206: return &mapper206_ops;
    case 227: return &mapper227_ops;
    default: return NULL;
    }
}

/* A Mapper built for an unknown number (loaders gate on mapper_supported,
 * but tests construct Mappers directly) behaves as NROM rather than
 * dereferencing NULL. */
static const MapperOps *mapper_ops_for(uint16_t number) {
    const MapperOps *ops = mapper_ops_lookup(number);
    return ops ? ops : &mapper0_ops;
}

void mapper_init(Mapper *m, uint16_t number,
                 uint8_t *prg_rom, uint32_t prg_size,
                 uint8_t *chr_rom, uint32_t chr_size,
                 uint8_t mirroring) {
    if (!m) return;

    memset(m, 0, sizeof(*m));
    m->number = number;
    m->prg_rom = prg_rom;
    m->prg_rom_size = prg_size;
    m->prg_banks = prg_size / 0x4000;  /* 16KB banks */
    m->chr_rom = chr_rom;
    m->chr_rom_size = chr_size;
    m->chr_banks = chr_size / 0x2000;  /* 8KB banks */
    m->mirroring = mirroring;
    m->has_chr_ram = (chr_size == 0);

    mapper_reset(m);
}

void mapper_reset(Mapper *m) {
    if (!m) return;
    const MapperOps *ops = mapper_ops_for(m->number);
    if (ops && ops->init) {
        ops->init(m);
    }
}

uint8_t mapper_cpu_read(Mapper *m, uint16_t addr) {
    const MapperOps *ops = mapper_ops_for(m ? m->number : 0);
    if (ops && ops->cpu_read) {
        return ops->cpu_read(m, addr);
    }
    return 0;
}

void mapper_cpu_write(Mapper *m, uint16_t addr, uint8_t val) {
    const MapperOps *ops = mapper_ops_for(m ? m->number : 0);
    if (ops && ops->cpu_write) {
        ops->cpu_write(m, addr, val);
    }
}

uint8_t mapper_ppu_read(Mapper *m, uint16_t addr) {
    const MapperOps *ops = mapper_ops_for(m ? m->number : 0);
    if (ops && ops->ppu_read) {
        return ops->ppu_read(m, addr);
    }
    return 0;
}

void mapper_ppu_write(Mapper *m, uint16_t addr, uint8_t val) {
    const MapperOps *ops = mapper_ops_for(m ? m->number : 0);
    if (ops && ops->ppu_write) {
        ops->ppu_write(m, addr, val);
    }
}

void mapper_ppu_address(Mapper *m, uint16_t addr) {
    if (!m) return;
    const MapperOps *ops = mapper_ops_for(m->number);
    if (ops && ops->ppu_address)
        ops->ppu_address(m, addr);
}

void mapper_notify_scanline(Mapper *m) {
    if (!m) return;
    const MapperOps *ops = mapper_ops_for(m->number);
    if (ops->scanline)
        ops->scanline(m);
}

uint8_t mapper_get_mirroring(Mapper *m) {
    return m ? m->mirroring : 0;
}

bool mapper_supported(uint16_t number) {
    return mapper_ops_lookup(number) != NULL;
}

void mapper_ppu_bus_read(Mapper *m, uint16_t addr) {
    const MapperOps *ops = mapper_ops_for(m->number);
    if (ops->ppu_bus_read) ops->ppu_bus_read(m, addr);
}

/* A mapper without a peek has reads that only look things up, so its plain
 * read stands in; the cast drops a const that read never writes through. */
uint8_t mapper_cpu_peek(const Mapper *m, uint16_t addr) {
    const MapperOps *ops = mapper_ops_for(m->number);
    if (ops->cpu_peek) return ops->cpu_peek(m, addr);
    return mapper_cpu_read((Mapper *)m, addr);
}

uint8_t mapper_ppu_peek(const Mapper *m, uint16_t addr) {
    const MapperOps *ops = mapper_ops_for(m->number);
    if (ops->ppu_peek) return ops->ppu_peek(m, addr);
    return mapper_ppu_read((Mapper *)m, addr);
}

void mapper_cpu_clock(Mapper *m) {
    const MapperOps *ops = mapper_ops_for(m->number);
    if (ops->cpu_clock) ops->cpu_clock(m);
}
