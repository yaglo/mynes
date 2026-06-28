#!/bin/bash
# Compile all GLSL shaders to SPIR-V and cross-compile to MSL (Metal).
# Requires: glslc (shaderc), spirv-cross
#
# Usage: ./compile.sh
# Output: .spv + .msl files alongside each .glsl source.

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

HAS_SPIRV_CROSS=0
which spirv-cross >/dev/null 2>&1 && HAS_SPIRV_CROSS=1

# --- Compute shaders ---
for glsl in "$SCRIPT_DIR"/compute/*.comp.glsl; do
    [ -f "$glsl" ] || continue
    spv="${glsl%.glsl}.spv"
    echo "Compiling $(basename "$glsl") → $(basename "$spv")"
    glslc -fshader-stage=compute "$glsl" -o "$spv"
    if [ "$HAS_SPIRV_CROSS" = "1" ]; then
        msl="${glsl%.glsl}.msl"
        spirv-cross --msl "$spv" --output "$msl" 2>/dev/null && echo "  → $(basename "$msl")" || true
    fi
done

# --- Render shaders: vertex ---
for glsl in "$SCRIPT_DIR"/render/*.vert.glsl; do
    [ -f "$glsl" ] || continue
    spv="${glsl%.glsl}.spv"
    echo "Compiling $(basename "$glsl") → $(basename "$spv")"
    glslc -fshader-stage=vertex "$glsl" -o "$spv"
    if [ "$HAS_SPIRV_CROSS" = "1" ]; then
        msl="${glsl%.glsl}.msl"
        spirv-cross --msl "$spv" --output "$msl" 2>/dev/null && echo "  → $(basename "$msl")" || true
    fi
done

# --- Render shaders: fragment ---
for glsl in "$SCRIPT_DIR"/render/*.frag.glsl; do
    [ -f "$glsl" ] || continue
    spv="${glsl%.glsl}.spv"
    echo "Compiling $(basename "$glsl") → $(basename "$spv")"
    glslc -fshader-stage=fragment "$glsl" -o "$spv"
    if [ "$HAS_SPIRV_CROSS" = "1" ]; then
        msl="${glsl%.glsl}.msl"
        spirv-cross --msl "$spv" --output "$msl" 2>/dev/null && echo "  → $(basename "$msl")" || true
    fi
done

echo "Done. SPIR-V compiled.$([ "$HAS_SPIRV_CROSS" = "1" ] && echo " MSL cross-compiled." || echo " (spirv-cross not found, no MSL.)")"
