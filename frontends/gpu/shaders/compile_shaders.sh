#!/bin/bash
# Compile all GLSL shaders to SPIR-V, cross-compile to MSL, and fix
# Metal buffer indices to match SDL_GPU's binding formula:
#
#   uniform buffers:          buffer(0)   .. buffer(U-1)
#   readonly storage buffers: buffer(U)   .. buffer(U+R-1)
#   readwrite storage buffers: buffer(U+R) .. buffer(U+R+W-1)
#
# Requires: glslc (shaderc), spirv-cross
# Usage: ./compile_shaders.sh

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Build output directory (relative to repo root).
BUILD_SHADER_DIR="$SCRIPT_DIR/../../../build/shaders"

ERRORS=0
COMPILED=0
FIXED=0

# ── Resource counts per compute kernel: U R W ──
get_resources() {
    case "$1" in
        pointwise)      echo "1 0 2" ;;
        rc_filter)      echo "1 0 2" ;;
        fir)            echo "1 2 1" ;;
        delay)          echo "1 2 1" ;;
        modulator)      echo "1 1 2" ;;
        dac_2c02)       echo "1 3 1" ;;
        matrix_decode)  echo "1 3 1" ;;
        pal_chroma)     echo "1 2 2" ;;
        deflection)     echo "1 0 2" ;;
        beam_profile)   echo "1 3 1" ;;
        comb_filter)    echo "1 1 2" ;;
        rf_mod_demod)   echo "1 0 1" ;;
        video_amp)      echo "1 1 1" ;;
        h_blur_rgb)     echo "1 0 2" ;;
        temporal_blit)  echo "1 2 0" ;;
        *)              echo "" ;;
    esac
}

# ── Compile one shader ──
compile_shader() {
    local glsl="$1"
    local stage="$2"
    local spv="${glsl%.glsl}.spv"
    local msl="${glsl%.glsl}.msl"
    local base
    base=$(basename "$glsl")

    printf "  %s\n" "$base"
    if ! glslc -fshader-stage="$stage" "$glsl" -o "$spv" 2>&1; then
        echo "    ERROR: glslc failed"
        ERRORS=$((ERRORS + 1))
        return 1
    fi
    echo "    -> $(basename "$spv")"

    if ! spirv-cross --msl "$spv" --output "$msl" 2>/dev/null; then
        echo "    ERROR: spirv-cross failed"
        ERRORS=$((ERRORS + 1))
        return 1
    fi
    echo "    -> $(basename "$msl")"
    COMPILED=$((COMPILED + 1))
    return 0
}

# ── Fix MSL buffer indices for a compute shader ──
#
# Strategy:
# 1. Parse the GLSL source to classify each buffer struct name as
#    "uniform", "readonly", or "readwrite" (from layout qualifiers).
# 2. Parse the MSL kernel signature to find each struct name and its
#    current [[buffer(N)]] index.
# 3. Assign correct indices per SDL_GPU formula:
#    - uniform structs -> 0..U-1 (in GLSL binding order)
#    - readonly structs -> U..U+R-1 (in GLSL binding order)
#    - readwrite structs -> U+R..U+R+W-1 (in GLSL binding order)
# 4. Rewrite the MSL if any index changed.
fix_msl_buffers() {
    local msl="$1"
    local glsl="$2"
    local kernel_name="$3"
    local res
    res=$(get_resources "$kernel_name")
    if [ -z "$res" ]; then
        echo "    buffers: no resource map (skipped)"
        return 0
    fi

    local U R W
    U=$(echo "$res" | cut -d' ' -f1)
    R=$(echo "$res" | cut -d' ' -f2)
    W=$(echo "$res" | cut -d' ' -f3)

    # Step 1: Parse GLSL to get ordered lists of struct names by category.
    # uniform buffer names (from "uniform" keyword before "buffer")
    local uniform_names=""
    local readonly_names=""
    local readwrite_names=""

    while IFS= read -r line; do
        # Match lines like: layout(set = N, binding = M) [readonly|writeonly|] buffer StructName {
        case "$line" in
            *uniform*) ;;
            *readonly*buffer*)
                local name
                name=$(echo "$line" | sed 's/.*buffer[[:space:]]*\([A-Za-z_][A-Za-z_0-9]*\).*/\1/')
                readonly_names="${readonly_names:+$readonly_names }$name"
                continue ;;
            *buffer*)
                # readwrite (writeonly or plain buffer without readonly/uniform)
                local name
                name=$(echo "$line" | sed 's/.*buffer[[:space:]]*\([A-Za-z_][A-Za-z_0-9]*\).*/\1/')
                readwrite_names="${readwrite_names:+$readwrite_names }$name"
                continue ;;
        esac
        case "$line" in
            *uniform*)
                local name
                name=$(echo "$line" | sed 's/.*uniform[[:space:]]*\([A-Za-z_][A-Za-z_0-9]*\).*/\1/')
                uniform_names="${uniform_names:+$uniform_names }$name"
                ;;
        esac
    done < <(grep -E 'layout\(set.*binding' "$glsl")

    # Step 2: Parse MSL kernel signature to find struct_name -> current buffer index.
    local sig
    sig=$(grep 'kernel void main0(' "$msl") || return 0

    # Build a mapping: for each struct name, find its current buffer(N).
    # We'll store as "StructName:N" pairs.
    local msl_buffers=""
    local save_ifs="$IFS"
    IFS=','
    for param in $(echo "$sig" | sed 's/kernel void main0(//;s/)$//'); do
        case "$param" in
            *"[[buffer("*")"*) ;;
            *) continue ;;
        esac
        local idx
        idx=$(echo "$param" | sed 's/.*\[\[buffer(\([0-9]*\))\]\].*/\1/')
        # Extract struct type name (e.g., "Params" from "constant Params& _20")
        local sname
        sname=$(echo "$param" | sed 's/.*[[:space:]]\([A-Za-z_][A-Za-z_0-9]*\)&.*/\1/')
        msl_buffers="${msl_buffers:+$msl_buffers }${sname}:${idx}"
    done
    IFS="$save_ifs"

    # Step 3: Assign correct indices.
    local need_fix=0
    local expected=0
    # Collect all remaps as "old:new" pairs.
    local remaps=""

    # Helper: look up current buffer index for a struct name.
    lookup_idx() {
        local target="$1"
        local entry
        for entry in $msl_buffers; do
            if [ "${entry%%:*}" = "$target" ]; then
                echo "${entry##*:}"
                return
            fi
        done
        echo ""
    }

    # Uniforms first.
    for name in $uniform_names; do
        local old
        old=$(lookup_idx "$name")
        if [ -z "$old" ]; then continue; fi
        if [ "$old" -ne "$expected" ]; then
            need_fix=1
        fi
        remaps="${remaps:+$remaps }${old}:${expected}:${name}"
        expected=$((expected + 1))
    done
    # Readonly storage.
    for name in $readonly_names; do
        local old
        old=$(lookup_idx "$name")
        if [ -z "$old" ]; then continue; fi
        if [ "$old" -ne "$expected" ]; then
            need_fix=1
        fi
        remaps="${remaps:+$remaps }${old}:${expected}:${name}"
        expected=$((expected + 1))
    done
    # Readwrite storage.
    for name in $readwrite_names; do
        local old
        old=$(lookup_idx "$name")
        if [ -z "$old" ]; then continue; fi
        if [ "$old" -ne "$expected" ]; then
            need_fix=1
        fi
        remaps="${remaps:+$remaps }${old}:${expected}:${name}"
        expected=$((expected + 1))
    done

    if [ "$need_fix" -eq 0 ]; then
        echo "    buffers: OK (U=$U R=$R W=$W)"
        return 0
    fi

    # Step 4: Rewrite MSL.  Two-pass sed to avoid collisions.
    local sed_cmd=""
    # Pass 1: rename changed indices to temporary placeholders.
    for entry in $remaps; do
        local old="${entry%%:*}"
        local rest="${entry#*:}"
        local new="${rest%%:*}"
        if [ "$old" -ne "$new" ]; then
            sed_cmd="${sed_cmd}s/\[\[buffer($old)\]\]/[[buffer(__TMP_${new}__)]]/g;"
        fi
    done
    # Pass 2: resolve all placeholders to final indices.
    for entry in $remaps; do
        local rest="${entry#*:}"
        local new="${rest%%:*}"
        sed_cmd="${sed_cmd}s/\[\[buffer(__TMP_${new}__)\]\]/[[buffer($new)]]/g;"
    done

    if [ -n "$sed_cmd" ]; then
        sed -i '' "$sed_cmd" "$msl"
        FIXED=$((FIXED + 1))
        echo "    buffers: FIXED (U=$U R=$R W=$W)"
        for entry in $remaps; do
            local old="${entry%%:*}"
            local rest="${entry#*:}"
            local new="${rest%%:*}"
            local name="${rest#*:}"
            if [ "$old" -ne "$new" ]; then
                echo "      $name: buffer($old) -> buffer($new)"
            fi
        done
    fi
}

# ════════════════════════════════════════════════════════════════════
echo "=== Compute Shaders ==="
for glsl in "$SCRIPT_DIR"/compute/*.comp.glsl; do
    [ -f "$glsl" ] || continue
    compile_shader "$glsl" compute || continue

    kernel_name=$(basename "$glsl" .comp.glsl)
    msl="${glsl%.glsl}.msl"
    fix_msl_buffers "$msl" "$glsl" "$kernel_name"
done

echo ""
echo "=== Render Shaders (vertex) ==="
for glsl in "$SCRIPT_DIR"/render/*.vert.glsl; do
    [ -f "$glsl" ] || continue
    compile_shader "$glsl" vertex
done

echo ""
echo "=== Render Shaders (fragment) ==="
for glsl in "$SCRIPT_DIR"/render/*.frag.glsl; do
    [ -f "$glsl" ] || continue
    compile_shader "$glsl" fragment
done

# ── Copy to build directory ──
echo ""
if [ -d "$BUILD_SHADER_DIR" ]; then
    echo "=== Copying to build directory ==="
    mkdir -p "$BUILD_SHADER_DIR/compute" "$BUILD_SHADER_DIR/render"

    copied=0
    for f in "$SCRIPT_DIR"/compute/*.spv "$SCRIPT_DIR"/compute/*.msl; do
        [ -f "$f" ] || continue
        cp "$f" "$BUILD_SHADER_DIR/compute/"
        copied=$((copied + 1))
    done
    for f in "$SCRIPT_DIR"/render/*.spv "$SCRIPT_DIR"/render/*.msl; do
        [ -f "$f" ] || continue
        cp "$f" "$BUILD_SHADER_DIR/render/"
        copied=$((copied + 1))
    done
    echo "  Copied $copied files to $BUILD_SHADER_DIR"
else
    echo "(Build directory $BUILD_SHADER_DIR not found -- skipping copy)"
fi

# ── Summary ──
echo ""
echo "=== Summary ==="
echo "  Compiled: $COMPILED shaders"
echo "  Buffer fixes applied: $FIXED"
echo "  Errors: $ERRORS"
[ "$ERRORS" -eq 0 ] || exit 1
