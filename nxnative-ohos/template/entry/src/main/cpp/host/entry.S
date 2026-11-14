/* Guest-call gate and thread entry/leave trampolines (OHOS/ELF port).
   来源：infra/MacNativeGames/games/by3/runtime/gate.S 与 thread_enter.S，符号去 Mach-O 下划线，
   风格与同目录 hle_stack.S 对齐（ELF/PIC：局部标号 adr，外部函数 bl，不引用 C 全局变量故无 GOT 取数）。
   gate.S 的 by3 原版在门内先切宿主栈并保存 q 寄存器/nzcv/fpsr/fpcr；本移植按设计契约改为最小帧：
   门运行在客体小栈上，只存取 x0..x30，不许碰 q 寄存器（宿主栈切换由 hle_stack.S 的
   runtime_hle_switch 负责，与本门不重叠）。dm_thread_enter / runtime_thread_leave 与 by3 原版
   语义逐条对齐：host_context[0]=宿主 sp，host_context[1]=宿主 fp。 */
.text

/* runtime_gate: patched guest code calls this via blr, so x30 on entry is the patch return address
   that runtime_dispatch (host/loader.c) matches against the gate table. x0..x30 are saved in an
   array on the (guest) stack, passed to runtime_dispatch(ret, regs), and written back afterwards.
   注意：全程在客体小栈上，帧只有 256 字节寄存器数组；不碰 q 寄存器（契约要求）。 */
    .globl runtime_gate
    .p2align 2
runtime_gate:
    sub sp, sp, #256            // regs[31]：x0..x29 成对存放，x30 单独在 +240
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]         // 补丁点返回地址（blr 进来时的 lr）＝门标识
    mov x0, x30                 // runtime_dispatch(ret, regs)
    mov x1, sp
    bl runtime_dispatch
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #256
    ret

/* dm_thread_enter(entry, arg, sp, host_context, tcb): x0=entry, x1=arg, x2=sp, x3=host_context, x4=tcb。
   把宿主 sp 与 fp 记进 host_context（布局与 by3 原版一致：[0]=sp，[1]=fp，threads.c 的
   thread_exit 用它回宿主栈），装 x18=合成 TCB（OHOS 封 tpidrro 写，TLS 走 x18 方案），
   然后转到客体入口在客体栈上执行。不返回：客体须经 ExitThread 退出（host/threads.c）。 */
    .globl dm_thread_enter
    .p2align 2
dm_thread_enter:
    mov x5, sp
    stp x5, x29, [x3]
    mov x18, x4
    mov sp, x2
    mov x29, #0
    mov x16, x0
    mov x0, x1
    adr x30, 1f
    br x16
1:  bl dm_thread_returned       // host/threads.c：客体入口返回即 die（Horizon 要求 ExitThread）
    brk #0

/* runtime_guest_call_tcb(tcb, fn, a0, a1, a2, a3)：boot 线程进客体用。C 调用会踩 x18，
   装 TCB 与跳转必须在同一段裸汇编里完成；fn 不返回（nn::init::Start）。 */
    .globl runtime_guest_call_tcb
    .p2align 2
runtime_guest_call_tcb:
    mov x18, x0
    mov x16, x1            // fn 先存 IP 暂存寄存器（同 dm_thread_enter 的 mov x16/br x16 范式），避免被参数洗牌覆盖
    mov x0, x2
    mov x1, x3
    mov x2, x4
    mov x3, x5
    br x16

/* runtime_thread_leave(host_context, arg, fn): 从 host_context 取回 dm_thread_enter 记下的宿主
   sp/fp，切回宿主栈后在上面调用 fn(arg)。不返回（threads.c thread_exit_on_host 以 pthread_exit
   结束）。 */
    .globl runtime_thread_leave
    .p2align 2
runtime_thread_leave:
    ldp x4, x29, [x0]
    mov sp, x4
    mov x0, x1
    br x2
