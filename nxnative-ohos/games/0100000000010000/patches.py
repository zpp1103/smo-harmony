# patches.py —— Super Mario Odyssey (v393216 / 1.4.1) per-build 指令补丁
# odyssey 当前无重写器补丁（设计 §1：boot 尸检未要求 CUTS/OBS/FRAME_STEP）。
# 格式对齐 nxnative：按 NSO build ID 键控的 CUTS（每条断言原字节）/ OBS（观察门）/ FRAME_STEP（帧步进桩）。
# 一个 build 被两个档案声明即重写报错；sdk 模块公共裁剪走 dm/tools/sdk_patches.py（nxnative 语义）。
