#!/bin/bash
# Compatibility entry point; keep one shader compiler and Metal binding map.
exec "$(dirname "$0")/compile_shaders.sh" "$@"
