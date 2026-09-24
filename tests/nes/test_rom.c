#include <stdio.h>
#include <string.h>
#include "nes/rom.h"

/* ============================================================================
 * Test Helpers
 * ============================================================================ */

/* Create a minimal valid iNES file for testing */
static void create_test_rom(const char *path, uint8_t prg_banks, uint8_t chr_banks,
                            uint8_t flags6, uint8_t flags7) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return;

    /* Header */
    uint8_t header[16] = {
        'N', 'E', 'S', 0x1A,  /* Magic */
        prg_banks,            /* PRG banks */
        chr_banks,            /* CHR banks */
        flags6,               /* Flags 6 */
        flags7,               /* Flags 7 */
        0, 0, 0, 0, 0, 0, 0, 0 /* Padding */
    };
    fwrite(header, 1, 16, fp);

    /* PRG ROM - fill with pattern */
    uint32_t prg_size = prg_banks * 16384;
    for (uint32_t i = 0; i < prg_size; i++) {
        uint8_t val = (uint8_t)(i & 0xFF);
        fwrite(&val, 1, 1, fp);
    }

    /* CHR ROM - fill with pattern */
    uint32_t chr_size = chr_banks * 8192;
    for (uint32_t i = 0; i < chr_size; i++) {
        uint8_t val = (uint8_t)((i + 0x80) & 0xFF);
        fwrite(&val, 1, 1, fp);
    }

    fclose(fp);
}

/* ============================================================================
 * Tests
 * ============================================================================ */

int test_valid_rom(void) {
    const char *path = "/tmp/test_valid.nes";
    create_test_rom(path, 2, 1, 0x01, 0x00);  /* 32KB PRG, 8KB CHR, vertical mirroring */

    ROM rom;
    int result = nes_rom_load(&rom, path);

    if (result != ROM_OK) {
        printf("TEST valid_rom: FAIL (load returned %d: %s)\n", result, nes_rom_error_str(result));
        return 0;
    }

    int pass = 1;
    if (rom.prg_size != 32768) {
        printf("TEST valid_rom: FAIL (prg_size=%u expected 32768)\n", rom.prg_size);
        pass = 0;
    }
    if (rom.chr_size != 8192) {
        printf("TEST valid_rom: FAIL (chr_size=%u expected 8192)\n", rom.chr_size);
        pass = 0;
    }
    if (rom.mirroring != 1) {
        printf("TEST valid_rom: FAIL (mirroring=%u expected 1)\n", rom.mirroring);
        pass = 0;
    }
    if (rom.mapper != 0) {
        printf("TEST valid_rom: FAIL (mapper=%u expected 0)\n", rom.mapper);
        pass = 0;
    }

    /* Verify PRG data */
    if (rom.prg_rom[0] != 0x00 || rom.prg_rom[255] != 0xFF) {
        printf("TEST valid_rom: FAIL (PRG data incorrect)\n");
        pass = 0;
    }

    /* Verify CHR data */
    if (rom.chr_rom[0] != 0x80 || rom.chr_rom[127] != 0xFF) {
        printf("TEST valid_rom: FAIL (CHR data incorrect)\n");
        pass = 0;
    }

    nes_rom_free(&rom);

    if (pass) {
        printf("TEST valid_rom: PASS (PRG=%uKB CHR=%uKB mapper=%u mirror=%s)\n",
               32, 8, 0, "vertical");
    }
    return pass;
}

int test_horizontal_mirroring(void) {
    const char *path = "/tmp/test_hmirror.nes";
    create_test_rom(path, 1, 1, 0x00, 0x00);  /* horizontal mirroring (bit 0 = 0) */

    ROM rom;
    int result = nes_rom_load(&rom, path);

    if (result != ROM_OK) {
        printf("TEST horizontal_mirroring: FAIL (load error)\n");
        return 0;
    }

    if (rom.mirroring == 0) {
        printf("TEST horizontal_mirroring: PASS (mirroring=horizontal)\n");
        nes_rom_free(&rom);
        return 1;
    } else {
        printf("TEST horizontal_mirroring: FAIL (mirroring=%u expected 0)\n", rom.mirroring);
        nes_rom_free(&rom);
        return 0;
    }
}

int test_chr_ram(void) {
    const char *path = "/tmp/test_chrram.nes";
    create_test_rom(path, 1, 0, 0x00, 0x00);  /* 0 CHR banks = CHR RAM */

    ROM rom;
    int result = nes_rom_load(&rom, path);

    if (result != ROM_OK) {
        printf("TEST chr_ram: FAIL (load error)\n");
        return 0;
    }

    if (rom.chr_size == 0 && rom.chr_rom == NULL) {
        printf("TEST chr_ram: PASS (CHR RAM mode, chr_rom=NULL)\n");
        nes_rom_free(&rom);
        return 1;
    } else {
        printf("TEST chr_ram: FAIL (chr_size=%u chr_rom=%p)\n", rom.chr_size, (void*)rom.chr_rom);
        nes_rom_free(&rom);
        return 0;
    }
}

int test_invalid_magic(void) {
    const char *path = "/tmp/test_badmagic.nes";

    /* Create file with invalid magic */
    FILE *fp = fopen(path, "wb");
    if (!fp) {
        printf("TEST invalid_magic: FAIL (couldn't create test file)\n");
        return 0;
    }
    uint8_t bad_header[16] = { 'B', 'A', 'D', '!', 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    fwrite(bad_header, 1, 16, fp);
    fclose(fp);

    ROM rom;
    int result = nes_rom_load(&rom, path);

    if (result == ROM_ERR_HEADER) {
        printf("TEST invalid_magic: PASS (correctly rejected bad header)\n");
        return 1;
    } else {
        printf("TEST invalid_magic: FAIL (expected ROM_ERR_HEADER, got %d)\n", result);
        nes_rom_free(&rom);
        return 0;
    }
}

int test_file_not_found(void) {
    ROM rom;
    int result = nes_rom_load(&rom, "/tmp/nonexistent_file_12345.nes");

    if (result == ROM_ERR_FILE) {
        printf("TEST file_not_found: PASS (correctly returned ROM_ERR_FILE)\n");
        return 1;
    } else {
        printf("TEST file_not_found: FAIL (expected ROM_ERR_FILE, got %d)\n", result);
        return 0;
    }
}

int test_unsupported_mapper(void) {
    const char *path = "/tmp/test_mapper99.nes";
    /* Mapper 99: flags6 bits 4-7 = 0011, flags7 bits 4-7 = 0110 → mapper 0x63 = 99 */
    create_test_rom(path, 2, 1, 0x30, 0x60);

    ROM rom;
    int result = nes_rom_load(&rom, path);

    if (result == ROM_ERR_MAPPER) {
        /* The message must name the mapper so users can report it. */
        const char *msg = nes_rom_error_str(result);
        if (strcmp(msg, "Unsupported mapper 99") != 0) {
            printf("TEST unsupported_mapper: FAIL (message \"%s\" does not name mapper 99)\n", msg);
            return 0;
        }
        printf("TEST unsupported_mapper: PASS (correctly rejected: %s)\n", msg);
        return 1;
    } else {
        printf("TEST unsupported_mapper: FAIL (expected ROM_ERR_MAPPER, got %d)\n", result);
        nes_rom_free(&rom);
        return 0;
    }
}

/* The loader's gate and the dispatcher must agree for every number, so a
 * ROM that loads always has an implementation behind it and vice versa. */
static int test_mapper_gate(void) {
    static uint8_t data[INES_HEADER_SIZE + INES_PRG_BANK_SIZE + INES_CHR_BANK_SIZE];
    memcpy(data, "NES\x1A\x01\x01", 6);
    int pass = 1;
    for (int mapper = 0; mapper < 256; ++mapper) {
        data[6] = (uint8_t)((mapper & 0x0F) << 4);
        data[7] = (uint8_t)(mapper & 0xF0);
        ROM rom;
        int result = nes_rom_load_data(&rom, data, sizeof(data));
        if ((result == ROM_OK) != mapper_supported((uint8_t)mapper)) {
            printf("TEST mapper_gate: FAIL (mapper %d: loader %d, dispatcher %d)\n",
                   mapper, result, mapper_supported((uint8_t)mapper));
            pass = 0;
        }
        if (result == ROM_OK) nes_rom_free(&rom);
    }
    const uint8_t expected[] = {0, 1, 2, 3, 4, 5, 7, 9, 10, 11, 34, 66, 69, 71, 206, 227};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        if (!mapper_supported(expected[i])) {
            printf("TEST mapper_gate: FAIL (mapper %u not supported)\n", expected[i]);
            pass = 0;
        }
    }
    if (pass)
        printf("TEST mapper_gate: PASS (loader and dispatcher agree for all 256 numbers)\n");
    return pass;
}

/* NES 2.0 byte 8 holds mapper bits 8-11 and the submapper. A mapper above
 * 255 must be rejected under its own number rather than load as the mapper
 * its low eight bits name (260 would otherwise run as MMC3). */
static int test_nes2_mapper(void) {
    static uint8_t data[INES_HEADER_SIZE + INES_PRG_BANK_SIZE + INES_CHR_BANK_SIZE];
    memcpy(data, "NES\x1A\x01\x01", 6);
    int pass = 1;
    ROM rom;

    data[6] = 0x40;   /* mapper bits 0-3 = 4 */
    data[7] = 0x08;   /* NES 2.0 */
    data[8] = 0x21;   /* submapper 2, mapper bits 8-11 = 1: mapper 260 */
    int result = nes_rom_load_data(&rom, data, sizeof(data));
    const char *msg = nes_rom_error_str(result);
    if (result != ROM_ERR_MAPPER || rom.mapper != 260 || rom.submapper != 2 ||
        strcmp(msg, "Unsupported mapper 260") != 0) {
        printf("TEST nes2_mapper: FAIL (mapper 260: result %d, mapper %u.%u, \"%s\")\n",
               result, rom.mapper, rom.submapper, msg);
        if (result == ROM_OK) nes_rom_free(&rom);
        pass = 0;
    }

    data[8] = 0x10;   /* submapper 1 of mapper 4 loads */
    result = nes_rom_load_data(&rom, data, sizeof(data));
    if (result != ROM_OK || rom.mapper != 4 || rom.submapper != 1 || !rom.is_nes2) {
        printf("TEST nes2_mapper: FAIL (mapper 4.1: result %d, mapper %u.%u)\n",
               result, rom.mapper, rom.submapper);
        pass = 0;
    }
    if (result == ROM_OK) nes_rom_free(&rom);

    /* iNES 1.0 uses byte 8 for PRG RAM size and old dumps fill it with
     * junk, so it must not reach the mapper number there. */
    data[7] = 0x00;
    data[8] = 0x21;
    result = nes_rom_load_data(&rom, data, sizeof(data));
    if (result != ROM_OK || rom.mapper != 4 || rom.submapper != 0 || rom.is_nes2) {
        printf("TEST nes2_mapper: FAIL (iNES 1.0 byte 8: result %d, mapper %u.%u)\n",
               result, rom.mapper, rom.submapper);
        pass = 0;
    }
    if (result == ROM_OK) nes_rom_free(&rom);

    /* Every 12-bit number parses back unchanged, and the loader and the
     * dispatcher agree on it. */
    for (int mapper = 0; mapper < 4096; ++mapper) {
        data[6] = (uint8_t)((mapper & 0x0F) << 4);
        data[7] = (uint8_t)(0x08 | (mapper & 0xF0));
        data[8] = (uint8_t)(mapper >> 8);
        result = nes_rom_load_data(&rom, data, sizeof(data));
        if (rom.mapper != mapper ||
            (result == ROM_OK) != mapper_supported((uint16_t)mapper)) {
            printf("TEST nes2_mapper: FAIL (mapper %d parsed as %u, loader %d)\n",
                   mapper, rom.mapper, result);
            pass = 0;
        }
        if (result == ROM_OK) nes_rom_free(&rom);
    }

    if (pass)
        printf("TEST nes2_mapper: PASS (12-bit mapper and submapper; 260 rejected by number)\n");
    return pass;
}

/* Flags 6 bit 3 puts 2 KB of VRAM on the cartridge (Rad Racer II, Gauntlet).
 * The loader reports it as mirroring mode 4 whatever bit 0 says, for iNES
 * 1.0 and NES 2.0 headers alike. */
static int test_four_screen(void) {
    static uint8_t data[INES_HEADER_SIZE + INES_PRG_BANK_SIZE + INES_CHR_BANK_SIZE];
    static const struct { uint8_t flags6, flags7, mirroring; } cases[] = {
        {0x48, 0x00, 4}, {0x49, 0x00, 4}, {0x48, 0x08, 4}, {0x49, 0x08, 4},
        {0x40, 0x00, 0}, {0x41, 0x00, 1}, {0x41, 0x08, 1},
    };
    memcpy(data, "NES\x1A\x01\x01", 6);
    int pass = 1;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        data[6] = cases[i].flags6;
        data[7] = cases[i].flags7;
        ROM rom;
        int result = nes_rom_load_data(&rom, data, sizeof(data));
        if (result != ROM_OK || rom.mapper != 4 || rom.mirroring != cases[i].mirroring) {
            printf("TEST four_screen: FAIL (flags6 %02X flags7 %02X: result %d, mapper %u, "
                   "mirroring %u, expected %u)\n", cases[i].flags6, cases[i].flags7,
                   result, rom.mapper, rom.mirroring, cases[i].mirroring);
            pass = 0;
        }
        if (result == ROM_OK) nes_rom_free(&rom);
    }
    if (pass)
        printf("TEST four_screen: PASS (flags6 bit 3 gives mirroring 4 in iNES and NES 2.0)\n");
    return pass;
}

/* NES 2.0 byte 9 extends both sizes: the low nibble is PRG size bits 8-11
 * and the high nibble CHR size bits 8-11, with $F selecting the
 * exponent-multiplier form 2^E * (2M+1). iNES 1.0 ignores the byte. */
static int test_nes2_sizes(void) {
    static uint8_t data[INES_HEADER_SIZE + 0x100 * INES_PRG_BANK_SIZE + 3 * 0x2000];
    int pass = 1;
    ROM rom;
    memset(data, 0, INES_HEADER_SIZE);
    memcpy(data, "NES\x1A", 4);

    /* PRG $100 banks (4 MB) through the MSB nibble; CHR 3 x 8 KB as
     * 2^13 * 3 in exponent form. */
    data[4] = 0x00;
    data[5] = (13 << 2) | 1;
    data[7] = 0x08;
    data[9] = 0xF1;
    int result = nes_rom_load_data(&rom, data, sizeof(data));
    if (result != ROM_OK || rom.prg_size != 0x100u * INES_PRG_BANK_SIZE ||
        rom.chr_size != 3 * 0x2000) {
        printf("TEST nes2_sizes: FAIL (MSB/exponent: result %d, PRG %u, CHR %u)\n",
               result, rom.prg_size, rom.chr_size);
        pass = 0;
    } else {
        Mapper m;
        mapper_init(&m, 0, rom.prg_rom, rom.prg_size, rom.chr_rom, rom.chr_size, 0);
        if (m.prg_banks != 0x100) {
            printf("TEST nes2_sizes: FAIL (Mapper.prg_banks %u)\n", m.prg_banks);
            pass = 0;
        }
    }
    if (result == ROM_OK) nes_rom_free(&rom);

    /* 8 KB PRG in exponent form (2^13 * 1). */
    data[4] = 13 << 2;
    data[5] = 0;
    data[9] = 0x0F;
    result = nes_rom_load_data(&rom, data, INES_HEADER_SIZE + 0x2000);
    if (result != ROM_OK || rom.prg_size != 0x2000 || rom.chr_size != 0) {
        printf("TEST nes2_sizes: FAIL (8 KB PRG: result %d, PRG %u)\n", result, rom.prg_size);
        pass = 0;
    }
    if (result == ROM_OK) nes_rom_free(&rom);

    /* Sizes past what the mapper can count are refused, not wrapped. */
    data[4] = 63 << 2;
    result = nes_rom_load_data(&rom, data, sizeof(data));
    if (result != ROM_ERR_HEADER) {
        printf("TEST nes2_sizes: FAIL (2^63 PRG: result %d)\n", result);
        if (result == ROM_OK) nes_rom_free(&rom);
        pass = 0;
    }

    /* iNES 1.0: byte 9 is the TV system bit, not a size. */
    data[4] = 1;
    data[7] = 0x00;
    data[9] = 0x11;
    result = nes_rom_load_data(&rom, data, INES_HEADER_SIZE + INES_PRG_BANK_SIZE);
    if (result != ROM_OK || rom.prg_size != INES_PRG_BANK_SIZE || rom.chr_size != 0) {
        printf("TEST nes2_sizes: FAIL (iNES 1.0 byte 9: result %d, PRG %u)\n",
               result, rom.prg_size);
        pass = 0;
    }
    if (result == ROM_OK) nes_rom_free(&rom);

    if (pass)
        printf("TEST nes2_sizes: PASS (byte 9 MSB and exponent-multiplier sizes)\n");
    return pass;
}

/* Every mapper reduces PRG addresses modulo the PRG size, so a header
 * declaring no PRG ROM must be refused rather than divide by zero. */
static int test_prg_size_zero(void) {
    static uint8_t data[INES_HEADER_SIZE + INES_CHR_BANK_SIZE];
    memcpy(data, "NES\x1A\x00\x01", 6);
    int pass = 1;
    for (int nes2 = 0; nes2 < 2; ++nes2) {
        data[7] = nes2 ? 0x08 : 0x00;
        ROM rom;
        int result = nes_rom_load_data(&rom, data, sizeof(data));
        if (result != ROM_ERR_HEADER) {
            printf("TEST prg_size_zero: FAIL (%s: result %d)\n", nes2 ? "NES 2.0" : "iNES", result);
            if (result == ROM_OK) nes_rom_free(&rom);
            pass = 0;
        }
    }
    if (pass)
        printf("TEST prg_size_zero: PASS (header without PRG ROM rejected)\n");
    return pass;
}

/* Flags 6 bit 2 puts a 512-byte trainer between the header and PRG ROM.
 * Both loaders keep it, and it lands at $7000 once the mapper is up. */
static int test_trainer(void) {
    static uint8_t data[INES_HEADER_SIZE + INES_TRAINER_SIZE + INES_PRG_BANK_SIZE];
    memcpy(data, "NES\x1A\x01\x00", 6);
    data[6] = 0x04;
    for (int i = 0; i < INES_TRAINER_SIZE; ++i)
        data[INES_HEADER_SIZE + i] = (uint8_t)(i ^ 0x5A);
    data[INES_HEADER_SIZE + INES_TRAINER_SIZE] = 0xC3;
    const char *path = "/tmp/test_trainer.nes";
    FILE *fp = fopen(path, "wb");
    if (!fp) return 0;
    fwrite(data, 1, sizeof(data), fp);
    fclose(fp);

    int pass = 1;
    for (int from_file = 0; from_file < 2; ++from_file) {
        ROM rom;
        int result = from_file ? nes_rom_load(&rom, path)
                               : nes_rom_load_data(&rom, data, sizeof(data));
        if (result != ROM_OK || !rom.has_trainer || rom.prg_rom[0] != 0xC3) {
            printf("TEST trainer: FAIL (%s: result %d)\n", from_file ? "file" : "buffer", result);
            if (result == ROM_OK) nes_rom_free(&rom);
            pass = 0;
            continue;
        }
        Mapper m;
        mapper_init(&m, rom.mapper, rom.prg_rom, rom.prg_size, rom.chr_rom, rom.chr_size,
                    rom.mirroring);
        nes_rom_apply_trainer(&rom, &m);
        if (mapper_cpu_read(&m, 0x7000) != 0x5A || mapper_cpu_read(&m, 0x71FF) != (uint8_t)(0x1FF ^ 0x5A) ||
            mapper_cpu_read(&m, 0x6FFF) != 0 || mapper_cpu_read(&m, 0x7200) != 0) {
            printf("TEST trainer: FAIL (%s: trainer not at $7000)\n", from_file ? "file" : "buffer");
            pass = 0;
        }
        nes_rom_free(&rom);
    }
    remove(path);
    if (pass)
        printf("TEST trainer: PASS (trainer kept and loaded at $7000)\n");
    return pass;
}

int test_battery_flag(void) {
    const char *path = "/tmp/test_battery.nes";
    create_test_rom(path, 1, 1, 0x02, 0x00);  /* Battery flag set (bit 1) */

    ROM rom;
    int result = nes_rom_load(&rom, path);

    if (result != ROM_OK) {
        printf("TEST battery_flag: FAIL (load error)\n");
        return 0;
    }

    if (rom.has_battery) {
        printf("TEST battery_flag: PASS (battery=true)\n");
        nes_rom_free(&rom);
        return 1;
    } else {
        printf("TEST battery_flag: FAIL (battery flag not detected)\n");
        nes_rom_free(&rom);
        return 0;
    }
}

int test_rom_free(void) {
    const char *path = "/tmp/test_free.nes";
    create_test_rom(path, 1, 1, 0x00, 0x00);

    ROM rom;
    nes_rom_load(&rom, path);

    /* Verify data is loaded */
    if (!rom.prg_rom || !rom.chr_rom) {
        printf("TEST rom_free: FAIL (data not loaded)\n");
        return 0;
    }

    /* Free and verify cleanup */
    nes_rom_free(&rom);

    if (rom.prg_rom == NULL && rom.chr_rom == NULL &&
        rom.prg_size == 0 && rom.chr_size == 0) {
        printf("TEST rom_free: PASS (memory freed, pointers nulled)\n");
        return 1;
    } else {
        printf("TEST rom_free: FAIL (cleanup incomplete)\n");
        return 0;
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */

static int test_region_fallback(void) {
    const char *paths[] = {"/tmp/mynes_test (E).nes", "/tmp/mynes_test (Europe).nes",
        "/tmp/mynes_test (PAL).nes", "/tmp/mynes_test (Australia).nes",
        "/tmp/mynes_test (U).nes", "/tmp/mynes_test Europe.nes"};
    int pass = !nes_rom_pal_filename("/Europe/(E)/Game (USA).nes");
    for (int i = 0; i < 6; ++i) {
        create_test_rom(paths[i], 1, 0, 0, 0);
        ROM rom;
        if (nes_rom_load(&rom, paths[i]) != ROM_OK) return 0;
        pass &= rom.tv_system == (i < 4 ? NES_TV_PAL : NES_TV_NTSC);
        pass &= rom.region_from_filename == (i < 4);
        nes_rom_free(&rom);
        remove(paths[i]);
    }
    /* NES 2.0 explicitly declares NTSC: the filename must not override it. */
    create_test_rom(paths[0], 1, 0, 0, 8);
    ROM rom;
    if (nes_rom_load(&rom, paths[0]) != ROM_OK) return 0;
    pass &= rom.is_nes2 && rom.tv_system == NES_TV_NTSC && !rom.region_from_filename;
    nes_rom_free(&rom);
    remove(paths[0]);
    /* The memory loader has no filename and continues to honour the header. */
    uint8_t data[16 + 16384] = {'N', 'E', 'S', 0x1a, 1};
    data[9] = 1;
    if (nes_rom_load_data(&rom, data, sizeof(data)) != ROM_OK) return 0;
    pass &= rom.tv_system == NES_TV_PAL && !rom.region_from_filename;
    nes_rom_free(&rom);
    printf("TEST region_fallback: %s\n", pass ? "PASS" : "FAIL");
    return pass;
}

int main(void) {
    printf("=== NES ROM Loading Tests ===\n\n");

    int passed = 0;
    int total = 0;

    total++; passed += test_valid_rom();
    total++; passed += test_region_fallback();
    total++; passed += test_horizontal_mirroring();
    total++; passed += test_chr_ram();
    total++; passed += test_invalid_magic();
    total++; passed += test_file_not_found();
    total++; passed += test_unsupported_mapper();
    total++; passed += test_mapper_gate();
    total++; passed += test_nes2_mapper();
    total++; passed += test_four_screen();
    total++; passed += test_nes2_sizes();
    total++; passed += test_prg_size_zero();
    total++; passed += test_trainer();
    total++; passed += test_battery_flag();
    total++; passed += test_rom_free();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
