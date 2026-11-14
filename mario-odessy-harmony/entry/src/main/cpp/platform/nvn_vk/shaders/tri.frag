// M3l：固定红色片元输出（判读用：红三角 = draw 真的落进了离屏 RT）。
// 离线编译：glslangValidator -V tri.frag -o tri.frag.spv
#version 450

layout(location = 0) out vec4 o_color;

void main()
{
    o_color = vec4(1.0, 0.0, 0.0, 1.0);
}
