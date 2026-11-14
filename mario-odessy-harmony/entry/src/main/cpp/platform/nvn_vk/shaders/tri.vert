// M3l：无 VBO 的三角形顶点着色器（顶点由 gl_VertexIndex 生成）。
// 离线编译（构建机）：glslangValidator -V tri.vert -o tri.vert.spv
// 产物经 xxd -i 变成 ../tri_spv.h 的 C 数组入库；运行期不引入 glslang。
#version 450

void main()
{
    // 三个顶点覆盖屏幕大部分（约 79%），右上角留一块背景色——背景色 = loadOp=CLEAR 的深色，
    // 便于回读时同时判读「draw 生效」与「renderPass 清色生效」。
    const vec2 pos[3] = vec2[3](vec2(-0.9, -0.9), vec2(1.6, -0.9), vec2(-0.9, 1.6));
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
}
