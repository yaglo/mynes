/*
 * NES Test Runner Exit Codes
 *
 * Standard exit codes for automated testing and CI integration.
 */

#ifndef NES_EXIT_CODES_H
#define NES_EXIT_CODES_H

#define NES_EXIT_PASS    0  /* Test passed */
#define NES_EXIT_FAIL    1  /* Test failed */
#define NES_EXIT_TIMEOUT 2  /* Test timed out (still running at frame limit) */
#define NES_EXIT_ROM_ERR 3  /* ROM load error or unsupported mapper */

#endif /* NES_EXIT_CODES_H */
