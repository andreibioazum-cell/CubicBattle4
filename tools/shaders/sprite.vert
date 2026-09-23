#version 450
/* Common vertex shader of the Cubic Battle renderer: takes a vertex in virtual
 * screen pixels (origin at the top left, y down) and maps it to Vulkan NDC
 * through the push constant (2/w, 2/h, -1, -1). UV and colour reach the fragment
 * shader unchanged; solid primitives leave uv unused. */
layout(location = 0) in vec2 in_pos;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_col; /* R8G8B8A8_UNORM, normalized */
layout(push_constant) uniform Push {
    vec4 u_screen; /* (2/w, 2/h, -1, -1) */
} pc;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_col;
void main() {
    gl_Position = vec4(in_pos.x * pc.u_screen.x + pc.u_screen.z,
                       in_pos.y * pc.u_screen.y + pc.u_screen.w, 0.0, 1.0);
    v_uv = in_uv;
    v_col = in_col;
}
