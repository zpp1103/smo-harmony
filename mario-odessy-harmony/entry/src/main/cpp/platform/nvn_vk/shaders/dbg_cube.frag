// M4ab P1：把**真实 FS 实际用于 texture() 的最终采样坐标**与 r6 写进颜色。
//
// 依据（SPIR-V 8832429cd37c79fe 直读，/tmp/fs8832.dis）：
//   %379 = OpTypeImage %float Cube ...   ⇒ t0/t1 都是 samplerCube，采样坐标是立方体方向向量
//   coord.xy = v4.xy * gl_FragCoord.w * (1/r6)          %184..%205
//   r6       = 1/gl_FragCoord.w                          %178..%181
//   m        = max(|v4.z|, |coord.x|, |coord.y|)         %299
//   dir      = (coord.x/m, coord.y/m, -v4.z/m)           %357..%374 → %391 → texture(t0, dir)
// 输出编码（8-bit 量化，取值反解公式在 nvn_vk.c 的 [p2] J 行）：
//   R = dir.x*0.5+0.5   G = dir.y*0.5+0.5   B = dir.z*0.5+0.5
//   A = clamp(r6*0.5, 0, 1)（r6 = 1/gl_FragCoord.w；A=128 ⇒ r6=1.0，A=255 ⇒ r6>=2.0）
// 判据：
//   · 若 R/G/B 三个通道都出现 0 与 255、且存在像素某通道 |dir|=1 ⇒ 坐标是立方体方向（合法），
//     “[-1,1] ⇒ 越界”不成立；
//   · 若 R/G/B 只在 128..255（即 [0,1]）⇒ 坐标其实是 2D UV；
//   · A 恒 128 ⇒ r6=1（w=1）；A 偏离则 r6≠1，需回查 gl_Position.w。
// 注意：本变体只读取 varying v4（location 4）与 gl_FragCoord，不采样任何纹理、不读 cbuf。
#version 450

layout(location = 4) in vec4 v4;
layout(location = 0) out vec4 o_color;

void main()
{
    float w  = gl_FragCoord.w;
    float r6 = 1.0 / w;                 // SPIR-V %181
    vec2  c  = v4.xy * w * r6;          // SPIR-V %191/%203（= v4.xy）
    float m  = max(abs(c.x), max(abs(c.y), abs(v4.z)));   // SPIR-V %299
    float s  = 1.0 / m;                 // SPIR-V %322（r5）
    vec3  d  = vec3(c.x * s, c.y * s, -v4.z * s);         // SPIR-V %374/%391
    o_color = vec4(d * 0.5 + 0.5, clamp(r6 * 0.5, 0.0, 1.0));
}
