// M4s P2：合成顶点着色器（只读 location 0 = 位置属性，硬件直通 clip 空间）。
// 离线编译（构建机）：glslangValidator -V dbg.vert -o dbg.vert.spv
// 产物经 xxd -i 变成 ../dbg_spv.h 的 C 数组入库；运行期不引入 glslang。
//
// 用途：把「游戏那笔 draw 的顶点缓冲 + 索引」喂给一个**已知正确**的 VS，判
//   「顶点/索引/管线输入装配」与「翻译产物 VS + 常量」谁的问题。
// 设计：x*2 / y*-2 —— 针对 M4s 实测的 -0.5..0.5 全屏 quad 能铺满画布（NVN y 向上，Vulkan y 向下）。
#version 450

layout(location = 0) in vec4 a_pos;

void main()
{
    gl_Position = vec4(a_pos.x * 2.0, -a_pos.y * 2.0, 0.0, 1.0);
}
