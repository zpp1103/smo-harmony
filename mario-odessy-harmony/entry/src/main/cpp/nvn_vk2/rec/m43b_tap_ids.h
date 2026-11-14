// M43B / S2b-1：直喂侧 ↔ 录制侧对账的**命令族 id**（唯一事实源）。
//
// 为什么单独一个零依赖头文件：直喂侧的挂点（platform/nvn_replay/cmdbuf.c）与录制侧的探针
// （nvn_vk2/rec/m43b_rec_tap.c）必须用同一套编号，否则两边悄悄错位、对账结果全假。
// 这个头**不 include 任何东西**，所以 platform/ 与 rec/ 两个隔离编译域都能安全包含
// （rec/ 不能看见 platform/ 的头；platform/ 包含本头不会反向污染它自己的类型）。
#ifndef M43B_TAP_IDS_H
#define M43B_TAP_IDS_H

enum m43b_tap {
    T_DRAW_ARRAYS = 0, T_DRAW_ARRAYS_I, T_DRAW_ELEMENTS, T_DRAW_ELEMENTS_BV, T_DRAW_ELEMENTS_I,
    T_DRAW_ARRAYS_IND, T_DRAW_ELEMENTS_IND,
    T_PROGRAM, T_BLEND, T_CHMASK, T_COLOR, T_DSST, T_MS, T_POLY,
    T_VATTR, T_VSTREAM, T_VBUF, T_VBUF1, T_UBO, T_UBO1, T_SSBO, T_SSBO1,
    T_TEX, T_TEX1, T_IMG, T_IMG1, T_TEXPOOL, T_SAMPPOOL, T_RT,
    T_VIEWPORTS, T_VIEWPORT1, T_SCISSOR, T_SCISSOR1, T_DRANGE, T_DRANGE1, T_BLENDCOLOR,
    T_STENCIL_REF, T_STENCIL_MASK, T_STENCIL_VMASK,
    T_PRIMRESTART, T_POLYOFF, T_RENDEREN, T_CLEARCOLOR, T_CLEARCOLOR_I, T_CLEARCOLOR_UI,
    T_CLEARDS, T_CLEARTEX, T_CLEARTEX_I, T_CLEARTEX_UI, T_CLEARBUF,
    T_BARRIER, T_DISPATCH, T_DISPATCH_IND, T_COPYB2B, T_UPDUB, T_COPYROW, T_COPYIMG,
    T_DEPTHCLAMP,
    // M44A / S2b-2 前置：执行链必需的 4 个 opcode（原 M43B 的 17 个未镜像里挑出来的）
    T_CALL, T_SIGNAL_EVENT, T_FENCE_SYNC, T_REPORT_COUNTER,
    T_COUNT
};

#endif
