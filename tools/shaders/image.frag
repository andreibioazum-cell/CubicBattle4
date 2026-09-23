#version 450
/* Textured primitives (tex, tex_tint, text): the vertex colour is multiplied by
 * the texel. The sampler is nearest, matching the pixel-exact software renderer.
 * The font atlas is white with the coverage in the alpha, so text goes through
 * this shader too: texel RGB is 1 and alpha is the glyph coverage. */
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(location = 0) out vec4 out_color;
void main() {
    out_color = v_col * texture(u_tex, v_uv);
}
