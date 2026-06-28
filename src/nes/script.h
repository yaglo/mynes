/*
 * NES Test Script System
 *
 * Simple line-based scripting for automated test execution.
 * Commands: WAIT_FRAMES, PRESS_BUTTON, WAIT_EVENT, DUMP_SCREEN, SCREENSHOT, STOP
 */

#ifndef NES_SCRIPT_H
#define NES_SCRIPT_H

#include <stdint.h>
#include <stdbool.h>

/* Maximum commands per script */
#define SCRIPT_MAX_COMMANDS 256
#define SCRIPT_MAX_ARG_LEN  128

/* ============================================================================
 * Script Commands
 * ============================================================================ */

typedef enum {
    SCRIPT_CMD_WAIT_FRAMES,    /* WAIT_FRAMES <n> */
    SCRIPT_CMD_PRESS_BUTTON,   /* PRESS_BUTTON <button> <frames> */
    SCRIPT_CMD_WAIT_EVENT,     /* WAIT_EVENT <name> */
    SCRIPT_CMD_DUMP_SCREEN,    /* DUMP_SCREEN [filename] */
    SCRIPT_CMD_SCREENSHOT,     /* SCREENSHOT [filename] */
    SCRIPT_CMD_STOP,           /* STOP */
} ScriptCommandType;

typedef struct {
    ScriptCommandType type;
    int int_arg;               /* Frame count, button mask, etc. */
    int int_arg2;              /* Duration for PRESS_BUTTON */
    char str_arg[SCRIPT_MAX_ARG_LEN]; /* Event name, filename */
} ScriptCommand;

typedef struct {
    ScriptCommand commands[SCRIPT_MAX_COMMANDS];
    int count;
} Script;

/* ============================================================================
 * API
 * ============================================================================ */

/* Parse a script file. Returns true on success. */
bool script_load(Script *script, const char *filename);

/* Parse a button name to a button mask. Returns 0 on unknown. */
uint8_t script_parse_button(const char *name);

#endif /* NES_SCRIPT_H */
