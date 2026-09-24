/* The receiver's flyback blanking over the decode window (decode_window.h).
 * trace.xy is the unblanked line in window samples and trace.zw the
 * unblanked rows, [z, w). A sample stands for the interval half a sample
 * either side of it; its gate is the part of that interval the trace
 * covers on an unblanked row, and zero on a blanked row. */
float trace_gate(float x, uint row, vec4 trace) {
    float r = float(row);
    if (r < trace.z || r >= trace.w) return 0.0;
    return clamp(min(x + 0.5, trace.y) - max(x - 0.5, trace.x), 0.0, 1.0);
}
