// M4aa P2：把真实 VS 输出的 **varying v4.xy** 画成颜色（M4z 的 F 变体只涂了 v0，本轮换成 v4）。
// 离线编译：glslangValidator -V dbg_uv.frag -o dbg_uv.frag.spv  → xxd → ../dbg_spv.h
//
// 为什么是 v4：SPIR-V 8832429cd37c79fe（色带那笔的 FS）直读，采样坐标来自 v4[0]/v4[1]
//   （经 gl_FragCoord.w 与 1/r6 做透视校正后进 OpImageSampleExplicitLod），不是 v0、不是 gl_FragCoord。
// 判据（R/G 两通道）：
//   · 沿屏幕一次单调 0→255 ⇒ v4 跨度 ≈ [0,1]（重复不是 UV 越界造成的）；
//   · 先 0→255 再跳回 0 再来一遍（或 255→0→255）⇒ 跨度 ≈ 2 个周期，REPEAT 会把源图贴两遍；
//   · 通道恒 0 ⇒ 该 location 在 VS 里不存在/未写（下一步换 location 再测）。
// B 通道恒 128 = 对照：证明确实走过「真实 VS + 本 FS」这条管线，而不是空管线黑图。
#version 450

layout(location = 4) in vec4 v4;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(v4.x, v4.y, 0.5, 1.0);
}
