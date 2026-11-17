/* Guest-call gate and thread entry/leave trampolines (OHOS/ELF port).
   来源：infra/MacNativeGames/games/by3/runtime/gate.S 与 thread_enter.S，符号去 Mach-O 下划线，
   风格与同目录 hle_stack.S 对齐（ELF/PIC：局部标号 adr，外部函数 bl，不引用 C 全局变量故无 GOT 取数）。
   gate.S 的 by3 原版在门内先切宿主栈并保存 q 寄存器/nzcv/fpsr/fpcr；本移植 M6m 起恢复"门内切
   宿主栈"（只切 sp，不碰 q 寄存器，故仍是 x0..x30 最小帧；切换细节见下方 runtime_gate 注释）。
   dm_thread_enter / runtime_thread_leave 与 by3 原版
   语义逐条对齐：host_context[0]=宿主 sp，host_context[1]=宿主 fp。 */
.text

/* runtime_gate: patched guest code calls this via blr, so x30 on entry is the patch return address
   that runtime_dispatch (platform/loader.c) matches against the gate table. x0..x30 are saved in an
   array on the (guest) stack, passed to runtime_dispatch(ret, regs), and written back afterwards.
   M6m: 派发本身改到本线程的宿主栈上跑（hle_stack.S 放在合成 TCB 里的槽）。原因：宿主侧 SVC
   实现要 2784 字节栈（门 256 + dispatch 176 + svc 128 + svc_thread 64 + svc_thread_body 1952
   + block 64 + core_put 32 + core_grant 64 + now 48），而客体线程的栈可以小到 4 KiB；客体在
   栈快到底时发一个等待，这 2.7 KiB 就压进它的守卫页（M6l boot B cppcrash：
   SEGV_ACCERR@0x6de894fff0，sp=0x6de894ffd0，runtime_now_ns+4 的帧记录存储）。
   现在客体栈上只留 256 字节寄存器数组 + 64 字节切换状态；其余全在 8 MiB 宿主栈。
   没有登记宿主栈的线程（主线程/boot 线程、客体→宿主→客体的嵌套调用）仍在原地派发，与原先一致。
   不碰 q 寄存器（契约要求）。 */
    .globl runtime_gate
    .p2align 2
runtime_gate:
    sub sp, sp, #320            // regs[31]：x0..x29 成对存放，x30 单独在 +240；+256 起是切换状态
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
    add x9, sp, #320            // M7u：门入口时的客体 sp（进本帧之前）存进 regs[31] 垫槽
    str x9, [sp, #248]          // M7u: guest sp -> regs[31]（+248 垫槽；写回段只回读 +0..+240，不受影响）
    mov x9, sp                  // regs 基址（客体栈）
    str xzr, [sp, #256]         // 先记"没切宿主栈"，两条早退路径都靠它
    mov x10, x18
    adrp x11, :got:runtime_hle_tsd_off
    ldr x11, [x11, :got_lo12:runtime_hle_tsd_off]
    ldr x11, [x11]
    cbz x11, 2f                 // 宿主栈槽未初始化：按原样在客体栈上派发
    ldr x12, [x10, x11]         // 本线程的宿主栈顶；0 = 没有（主线程 / 已在他宿主栈上）
    str x12, [sp, #256]
    cbz x12, 2f
    str xzr, [x10, x11]         // 槽置 0：客体栈期间的嵌套宿主调用留在宿主栈上
    sub sp, x12, #32
    stp x29, x30, [sp]
    stp x9, x12, [sp, #16]
    mov x29, sp
    mov x0, x30                 // runtime_dispatch(ret, regs)
    mov x1, x9
    bl runtime_dispatch
    ldp x9, x12, [sp, #16]
    ldp x29, x30, [sp]
    mov sp, x9                  // 回到客体栈上的 regs 帧
    b 3f
2:  mov x0, x30                 // runtime_dispatch(ret, regs)
    mov x1, sp
    bl runtime_dispatch
3:  ldr x12, [sp, #256]         // 用过宿主栈就把槽还回去
    cbz x12, 4f
    ldr x18, [sp, #144]         // 客体 x18（合成 TCB）：宿主调用可能踩过
    mov x10, x18
    adrp x11, :got:runtime_hle_tsd_off
    ldr x11, [x11, :got_lo12:runtime_hle_tsd_off]
    ldr x11, [x11]
    str x12, [x10, x11]
4:  ldp x0, x1, [sp, #0]
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
    add sp, sp, #320
    ret

/* dm_thread_enter(entry, arg, sp, host_context, tcb): x0=entry, x1=arg, x2=sp, x3=host_context, x4=tcb。
   把宿主 sp 与 fp 记进 host_context（布局与 by3 原版一致：[0]=sp，[1]=fp，threads.c 的
   thread_exit 用它回宿主栈），装 x18=合成 TCB（OHOS 封 tpidrro 写，TLS 走 x18 方案），
   然后转到客体入口在客体栈上执行。不返回：客体须经 ExitThread 退出（platform/threads.c）。 */
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
1:  bl dm_thread_returned       // platform/threads.c：客体入口返回即 die（Horizon 要求 ExitThread）
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
