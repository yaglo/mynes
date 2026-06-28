#include "nes/script.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Button names matching BTN_* defines from nes.h */
static const struct { const char *name; uint8_t mask; } button_map[] = {
    {"a",      0x01}, {"b",      0x02}, {"select", 0x04}, {"start",  0x08},
    {"up",     0x10}, {"down",   0x20}, {"left",   0x40}, {"right",  0x80},
    {NULL, 0}
};

uint8_t script_parse_button(const char *name) {
    char lower[32];
    int i;
    for (i = 0; name[i] && i < 31; i++)
        lower[i] = tolower((unsigned char)name[i]);
    lower[i] = '\0';

    for (i = 0; button_map[i].name; i++) {
        if (strcmp(lower, button_map[i].name) == 0)
            return button_map[i].mask;
    }
    return 0;
}

bool script_load(Script *script, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return false;

    script->count = 0;
    char line[512];

    while (fgets(line, sizeof(line), fp) && script->count < SCRIPT_MAX_COMMANDS) {
        /* Strip trailing whitespace */
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' '))
            line[--len] = '\0';

        /* Skip empty lines and comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '#') continue;

        ScriptCommand *cmd = &script->commands[script->count];
        memset(cmd, 0, sizeof(*cmd));

        char word[64], arg1[128], arg2[64];
        int n = sscanf(p, "%63s %127s %63s", word, arg1, arg2);

        if (n < 1) continue;

        if (strcmp(word, "WAIT_FRAMES") == 0 && n >= 2) {
            cmd->type = SCRIPT_CMD_WAIT_FRAMES;
            cmd->int_arg = atoi(arg1);
        }
        else if (strcmp(word, "PRESS_BUTTON") == 0 && n >= 3) {
            cmd->type = SCRIPT_CMD_PRESS_BUTTON;
            cmd->int_arg = script_parse_button(arg1);
            cmd->int_arg2 = atoi(arg2);
        }
        else if (strcmp(word, "WAIT_EVENT") == 0 && n >= 2) {
            cmd->type = SCRIPT_CMD_WAIT_EVENT;
            strncpy(cmd->str_arg, arg1, SCRIPT_MAX_ARG_LEN - 1);
        }
        else if (strcmp(word, "DUMP_SCREEN") == 0) {
            cmd->type = SCRIPT_CMD_DUMP_SCREEN;
            if (n >= 2) strncpy(cmd->str_arg, arg1, SCRIPT_MAX_ARG_LEN - 1);
        }
        else if (strcmp(word, "SCREENSHOT") == 0) {
            cmd->type = SCRIPT_CMD_SCREENSHOT;
            if (n >= 2) strncpy(cmd->str_arg, arg1, SCRIPT_MAX_ARG_LEN - 1);
        }
        else if (strcmp(word, "STOP") == 0) {
            cmd->type = SCRIPT_CMD_STOP;
        }
        else {
            fprintf(stderr, "Script: unknown command '%s'\n", word);
            continue;
        }

        script->count++;
    }

    fclose(fp);
    return true;
}
