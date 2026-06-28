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
        printf("TEST unsupported_mapper: PASS (correctly rejected mapper 99)\n");
        return 1;
    } else {
        printf("TEST unsupported_mapper: FAIL (expected ROM_ERR_MAPPER, got %d)\n", result);
        nes_rom_free(&rom);
        return 0;
    }
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

int main(void) {
    printf("=== NES ROM Loading Tests ===\n\n");

    int passed = 0;
    int total = 0;

    total++; passed += test_valid_rom();
    total++; passed += test_horizontal_mirroring();
    total++; passed += test_chr_ram();
    total++; passed += test_invalid_magic();
    total++; passed += test_file_not_found();
    total++; passed += test_unsupported_mapper();
    total++; passed += test_battery_flag();
    total++; passed += test_rom_free();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
