/*
 * gpu_log.c -- Single definition of the GPU frontend verbosity flag.
 * Set by main.c's --verbose / -v flag. Read by everything that uses LOGV().
 */
#include "gpu_log.h"

bool gpu_verbose = false;
