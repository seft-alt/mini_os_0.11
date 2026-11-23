/* ==============================================================
 * CPU 异常处理接口
 *
 * 目前实现三个向量：0（除零）、13（通用保护）、14（缺页）。
 * 0 是纯 panic；13/14 各有“收回内核”的恢复路径：
 *   13：ring3 的 in/out 被 I/O 位图拒绝 → 记 ring3_io_deny、回内核（内核自己踩到仍 panic）；
 *   14：补页成功 → iret 重试原指令；guard 页命中 → 让该任务死掉、内核继续跑。
 * ============================================================== */
#ifndef MINI_OS_EXCEPTION_H
#define MINI_OS_EXCEPTION_H

/* 安装向量 0 / 13 / 14 的中断门 */
void exception_init(void);

/* 由 boot/intr.asm 的桩调用（向量 0/13：没有恢复语义，一律 panic）：
 *   vector   异常向量号
 *   error    错误码（向量 0 没有，桩传 0；13 由 CPU 压入） */
void exception_handler(unsigned int vector, unsigned int error);

/* 缺页专用入口：ctx 是桩压好现场后的 esp（见 boot/intr.asm 的布局说明）。
 * 返回值是 iret 之前要装载的 esp；可以修改 ctx 指向的 eip 来改变 iret 的去向。 */
unsigned long exception_handler_pf(unsigned long ctx);

/* 通用保护专用入口（同 ctx 约定）。ring3 执行端口 I/O 时 iomap 拒绝会走这里；
 * 内核自己的 #GP 仍然打印现场后停机（panic 语义不变）。 */
unsigned long exception_handler_gp(unsigned long ctx);

/* ring3 越权访问被锁内核区时记录下的现场（cr2 与错误码）。
 * err 的 bit2=1 表示“用户态发起”，bit0=1 表示“页存在但权限不够”；
 * 期望值 0x05 = 存在 + 用户发起 + 读。0 表示这次访问根本没被拦。 */
extern unsigned long ring3_fault_va;
extern unsigned long ring3_fault_err;

/* ring3 端口访问被 #GP 拦下的证据：1 = 拦住，0 = 没拦住（TSS 位图摆错了）。 */
extern unsigned long ring3_io_deny;

#endif /* MINI_OS_EXCEPTION_H */
