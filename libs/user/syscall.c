#include <libs/common/hinavm_types.h>
#include <libs/common/print.h>
#include <libs/user/ipc.h>
#include <libs/user/syscall.h>

//ipc系统调用：发送和接收消息
error_t sys_ipc(task_t dst, task_t src, struct message *m, unsigned flags) {
    return arch_syscall(dst, src, (uintptr_t) m, flags, 0, SYS_IPC);
}

//通知系统调用：发送通知
error_t sys_notify(task_t dst, notifications_t notifications) {
    return arch_syscall(dst, notifications, 0, 0, 0, SYS_NOTIFY);
}

//task_create系统调用：创建任务
task_t sys_task_create(const char *name, vaddr_t ip, task_t pager) {
    return arch_syscall((uintptr_t) name, ip, pager, 0, 0, SYS_TASK_CREATE);
}

//hinavm系统调用：执行HinaVM程序
task_t sys_hinavm(const char *name, hinavm_inst_t *insts, size_t num_insts,
                  task_t pager) {
    return arch_syscall((uintptr_t) name, (uintptr_t) insts, num_insts, pager,
                        0, SYS_HINAVM);
}

//task_destroy系统调用：删除任务
error_t sys_task_destroy(task_t task) {
    return arch_syscall(task, 0, 0, 0, 0, SYS_TASK_DESTROY);
}

//task_exit系统调用：终止正在运行的任务
__noreturn void sys_task_exit(void) {
    arch_syscall(0, 0, 0, 0, 0, SYS_TASK_EXIT);
    UNREACHABLE();
}

//task_self系统调用：获取正在运行的任务的ID
task_t sys_task_self(void) {
    return arch_syscall(0, 0, 0, 0, 0, SYS_TASK_SELF);
}

//pm_alloc系统调用：分配物理内存
pfn_t sys_pm_alloc(task_t tid, size_t size, unsigned flags) {
    return arch_syscall(tid, size, flags, 0, 0, SYS_PM_ALLOC);
}

//vm_map系统调用：映射页面
error_t sys_vm_map(task_t task, uaddr_t uaddr, paddr_t paddr, unsigned attrs) {
    return arch_syscall(task, uaddr, paddr, attrs, 0, SYS_VM_MAP);
}

//vm_unmap 系统调用：取消映射页面
error_t sys_vm_unmap(task_t task, uaddr_t uaddr) {
    return arch_syscall(task, uaddr, 0, 0, 0, SYS_VM_UNMAP);
}

//irq_listen系统调用：订阅中断通知
error_t sys_irq_listen(unsigned irq) {
    return arch_syscall(irq, 0, 0, 0, 0, SYS_IRQ_LISTEN);
}

//irq_unlisten 系统调用：取消订阅中断通知
error_t sys_irq_unlisten(unsigned irq) {
    return arch_syscall(irq, 0, 0, 0, 0, SYS_IRQ_UNLISTEN);
}

//Serial_write 系统调用：字符串的支柱
int sys_serial_write(const char *buf, size_t len) {
    return arch_syscall((uintptr_t) buf, len, 0, 0, 0, SYS_SERIAL_WRITE);
}

//Serial_read系统调用：读取字符输入
int sys_serial_read(const char *buf, int max_len) {
    return arch_syscall((uintptr_t) buf, max_len, 0, 0, 0, SYS_SERIAL_READ);
}

//time系统调用：设置超时
error_t sys_time(int milliseconds) {
    return arch_syscall(milliseconds, 0, 0, 0, 0, SYS_TIME);
}

//uptime系统调用：获取系统启动时间（以毫秒为单位）
int sys_uptime(void) {
    return arch_syscall(0, 0, 0, 0, 0, SYS_UPTIME);
}

//shutdown系统调用：关闭系统
__noreturn void sys_shutdown(void) {
    arch_syscall(0, 0, 0, 0, 0, SYS_SHUTDOWN);
    UNREACHABLE();
}
