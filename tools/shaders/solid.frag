#version 450
/* Solid primitives (rect, roundrect, circle, ring, line): the colour comes
 * entirely from the vertex and the fixed pipeline blends it (src-alpha). */
layout(location = 1) in vec4 v_col;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = v_col;
}
