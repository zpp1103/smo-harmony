// M4s P2 变体 D：把**当前绑定的 FS 纹理槽 0** 原样采样到屏幕上（不读常量、不乘 tint）。
// 离线编译：glslangValidator -V dbg_tex.frag -o dbg_tex.frag.spv
//
// 用途：M4s boot#1 实测「真实 FS 把整屏写成 (0,0,0,0)」。两个候选原因必须分开：
//   ①采样源（纹理图）本身就是空的；②纹理有内容，但被全零的 Tint（binding 19）乘成 0。
// 本变体绕过 Tint ⇒ 输出非黑即证明「纹理有内容、黑是 tint 造成的」；输出全黑即证明「源纹理是空的」。
// uv 用 gl_FragCoord / (1600,900) —— 与 boot#1 实测的目标尺寸一致（调用方可按尺寸把关）。
#version 450

layout(set = 1, binding = 32) uniform sampler2D t0;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = texture(t0, gl_FragCoord.xy / vec2(1600.0, 900.0));
}
