// M4s P2：合成片元着色器（不采样任何纹理、不读任何 UBO，固定输出绿色）。
// 离线编译：glslangValidator -V dbg.frag -o dbg.frag.spv
//
// 用途：与 dbg.vert 组合成「已知正确」的对照管线；单独与**游戏的 VS** 组合时，
// 用来判「真实 FS（纹理采样）是不是那片黑/无输出的来源」。
#version 450

layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(0.0, 1.0, 0.0, 1.0);
}
