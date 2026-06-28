/*
 * Fullscreen Triangle — Vertex Shader
 * =====================================
 *
 * Draws a single full-screen triangle using gl_VertexIndex (no VBO).
 * Three vertices cover the entire [-1,1] NDC quad:
 *
 *     v0 (-1,-1)  v1 (3,-1)  v2 (-1,3)
 *
 * The triangle overshoots the clip rect; the rasterizer clips it to
 * exactly the viewport. UV interpolates to [0,1] across the screen.
 *
 * Dispatch: vkCmdDraw(3, 1, 0, 0) with no vertex buffers bound.
 */

#version 450

layout(location = 0) out vec2 uv;

void main() {
    /*
     * Bit tricks: vertex 0 = (0,0), vertex 1 = (2,0), vertex 2 = (0,2).
     * Map to NDC: x = uv.x * 2 - 1, y = uv.y * 2 - 1.
     * UV.y is flipped so (0,0) is top-left to match Vulkan image layout.
     */
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    /* Flip V so texture row 0 appears at the top of the screen.
     * Without this, the image renders upside down on Vulkan/Metal
     * because NDC Y=-1 is the bottom but texture V=0 is the top. */
    uv = vec2(pos.x, 1.0 - pos.y);
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
