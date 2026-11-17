// M4z P1 变体 B/G/H：用**真实 VS 的 varying v0** 采样 FS 纹理槽 0，输出 rgb + alpha 强制 255。
// 离线编译：glslangValidator -V dbg_tex_v0.frag -o dbg_tex_v0.frag.spv  → xxd → ../dbg_spv.h
//
// 和 dbg_tex.frag（变体 D，uv=gl_FragCoord/尺寸）的区别：D 完全不经过 v0，
//   · D 单份、G 两份 ⇒ 重复来自 v0（UV 变换）；
//   · D 两份、G 两份 ⇒ 源纹理自己就带两份（上游写进去的）；
//   · D 单份、G 单份 ⇒ 采样侧干净，A 的色带来自真实 FS 的算术（tint/其他常量）。
// alpha 固定 1.0：写回链路若把 alpha 改掉，B/G 的 alpha 就不是 255；若仍是 255，则 A 的 27 是 FS 算出来的。
#version 450

layout(set = 1, binding = 32) uniform sampler2D t0;
layout(location = 0) in vec4 v0;
layout(location = 0) out vec4 o_color;

void main()
{
    vec4 t = texture(t0, v0.xy);
    o_color = vec4(t.rgb, 1.0);
}
