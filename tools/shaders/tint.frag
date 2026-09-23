#version 450
/* Tinted texture (tex_tint): a silhouette from the texture alpha filled with the
 * vertex colour. The texel RGB is ignored, as in the pixel-exact software
 * renderer, where a shadow was the tint colour with the texture alpha. */
layout(binding = 0) uniform sampler2D u_tex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(location = 0) out vec4 out_color;
void main() {
    float a = texture(u_tex, v_uv).a * v_col.a;
    out_color = vec4(v_col.rgb, a);
}
