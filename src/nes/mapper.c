/*
 * Mapper dispatcher and lifecycle helpers.
 */

#include "mapper.h"
#include "mappers/mapper_ops.h"
#include <string.h>

static const MapperOps *mapper_ops_for(uint8_t number) {
    switch (number) {
    case 0: return &mapper0_ops;
    case 1: return &mapper1_ops;
    case 2: return &mapper2_ops;
    case 3: return &mapper3_ops;
    case 4: return &mapper4_ops;
    case 5: return &mapper5_ops;
    case 7: return &mapper7_ops;
    case 10: return &mapper10_ops;
    case 69: return &mapper69_ops;
    case 227: return &mapper227_ops;
    default: return &mapper0_ops;
    }
}

void mapper_init(Mapper *m, uint8_t number,
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

bool mapper_supported(uint8_t number) {
    return number == 0 || number == 1 || number == 2 ||
           number == 3 || number == 4 || number == 5 ||
           number == 7 || number == 10 || number == 69 || number == 227;
}
