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
    total++; passed += test_battery_flag();
    total++; passed += test_rom_free();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
