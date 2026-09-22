/*
 * Internal mapper operation table definitions.
 */

#ifndef NES_MAPPER_OPS_H
#define NES_MAPPER_OPS_H

#include "../mapper.h"

typedef struct MapperOps {
    void (*init)(Mapper *m);
    uint8_t (*cpu_read)(Mapper *m, uint16_t addr);
    void (*cpu_write)(Mapper *m, uint16_t addr, uint8_t val);
    uint8_t (*ppu_read)(Mapper *m, uint16_t addr);
    void (*ppu_write)(Mapper *m, uint16_t addr, uint8_t val);
    void (*ppu_address)(Mapper *m, uint16_t addr);
    void (*ppu_bus_read)(Mapper *m, uint16_t addr);
    void (*cpu_clock)(Mapper *m);
    void (*scanline)(Mapper *m);   /* Called at end of each visible scanline */
} MapperOps;

extern const MapperOps mapper0_ops;
extern const MapperOps mapper1_ops;
extern const MapperOps mapper2_ops;
extern const MapperOps mapper3_ops;
extern const MapperOps mapper4_ops;
extern const MapperOps mapper7_ops;
extern const MapperOps mapper5_ops;
extern const MapperOps mapper9_ops;
extern const MapperOps mapper10_ops;
extern const MapperOps mapper11_ops;
extern const MapperOps mapper34_ops;
extern const MapperOps mapper66_ops;
extern const MapperOps mapper69_ops;
extern const MapperOps mapper71_ops;
extern const MapperOps mapper206_ops;
extern const MapperOps mapper227_ops;

#endif /* NES_MAPPER_OPS_H */
