#include "syscall.h"
#include "arch.h"
#include "hinavm.h"
#include "interrupt.h"
#include "ipc.h"
#include "memory.h"
#include "printk.h"
#include "task.h"
#include <libs/common/string.h>

//从用户空间进行内存复制。与普通memcpy不同的是，如果复制过程中出现页面错误
//在这种情况下，将页面错误视为发生在用户任务中而不是发生在内核区域中。
error_t memcpy_from_user(void *dst, __user const void *src, size_t len) {
    if (!arch_is_mappable_uaddr((uaddr_t) src)) {
        //在内核模式下，您有权限访问所有虚拟地址，因此传递的指针
//验证您确实指向用户空间地址。
        return ERR_INVALID_UADDR;
    }

    arch_memcpy_from_user(dst, src, len);
    return OK;
}

//内存复制到用户空间。与普通memcpy不同的是，如果复制过程中出现页面错误
//在这种情况下，页面错误被视为发生在正在运行的任务中而不是内核中的页面错误。
error_t memcpy_to_user(__user void *dst, const void *src, size_t len) {
    if (!arch_is_mappable_uaddr((uaddr_t) dst)) {
        //在内核模式下，您有权限访问所有虚拟地址，因此传递的指针
//验证您确实指向用户空间地址。
        return ERR_INVALID_UADDR;
    }

    arch_memcpy_to_user(dst, src, len);
    return OK;
}

//从用户空间复制Dst len字节。复制后，如果不是以null结尾，则会返回错误。
static error_t strcpy_from_user(char *dst, size_t dst_len,
                                __user const char *src) {
    DEBUG_ASSERT(dst_len > 0);

    error_t err = memcpy_from_user(dst, src, dst_len);
    if (err != OK) {
        return err;
    }

    //检查它是否以空终止。
    for (size_t i = 0; i < dst_len; i++) {
        if (dst[i] == '\0') {
            return OK;
        }
    }

    return ERR_INVALID_ARG;
}

//创建新的用户任务。
static task_t sys_task_create(__user const char *name, uaddr_t ip,
                              task_t pager) {
    //获取任务名称
    char namebuf[TASK_NAME_LEN];
    error_t err = strcpy_from_user(namebuf, sizeof(namebuf), name);
    if (err != OK) {
        return err;
    }

    //获取寻呼机任务
    struct task *pager_task = task_find(pager);
    if (!pager_task) {
        return ERR_INVALID_ARG;
    }

    //创建常规用户任务时
    return task_create(namebuf, ip, pager_task);
}

//生成 Hina vm 任务。 insts是hina vm指令序列，num insts是指令数，pager是分页任务。
static task_t sys_hinavm(__user const char *name, __user hinavm_inst_t *insts,
                         size_t num_insts, task_t pager) {
    //获取任务名称
    char namebuf[TASK_NAME_LEN];
    error_t err = strcpy_from_user(namebuf, sizeof(namebuf), name);
    if (err != OK) {
        return err;
    }

    //获取寻呼机任务
    struct task *pager_task = task_find(pager);
    if (!pager_task) {
        return ERR_INVALID_ARG;
    }

    hinavm_inst_t instsbuf[HINAVM_INSTS_MAX];
    if (num_insts > HINAVM_INSTS_MAX) {
        WARN("too many instructions: %u (max=%u)", num_insts, HINAVM_INSTS_MAX);
        return ERR_INVALID_ARG;
    }

    //获取HinaVM程序（指令序列）
    err = memcpy_from_user(instsbuf, insts, num_insts * sizeof(*insts));
    if (err != OK) {
        return err;
    }

    //创建Hina vm任务
    return hinavm_create(namebuf, instsbuf, num_insts, pager_task);
}

//删除任务。
static error_t sys_task_destroy(task_t tid) {
    struct task *task = task_find(tid);
    if (!task || task == CURRENT_TASK) {
        return ERR_INVALID_TASK;
    }

    return task_destroy(task);
}

//通常终止正在运行的任务。
__noreturn static void sys_task_exit(void) {
    task_exit(EXP_GRACE_EXIT);
}

//获取正在运行的任务的任务ID。
static task_t sys_task_self(void) {
    return CURRENT_TASK->tid;
}

//分配物理页。
//
//如果在标志中指定了 PM_ALLOC_ALIGN，则与 size 字节对齐的地址为
//分配。
static pfn_t sys_pm_alloc(task_t tid, size_t size, unsigned flags) {
    //检查是否指定了未知/不允许的标志
    if ((flags & ~(PM_ALLOC_ZEROED | PM_ALLOC_ALIGNED)) != 0) {
        return ERR_INVALID_ARG;
    }

    //获取任务所有者
    struct task *task = task_find(tid);
    if (!task) {
        return ERR_INVALID_TASK;
    }

    if (task != CURRENT_TASK && task->pager != CURRENT_TASK) {
        return ERR_INVALID_TASK;
    }

    flags |= PM_ALLOC_ZEROED;
    paddr_t paddr = pm_alloc(size, task, flags);
    if (!paddr) {
        return ERR_NO_MEMORY;
    }

    return PADDR2PFN(paddr);
}

//将页面映射到虚拟地址空间。
static paddr_t sys_vm_map(task_t tid, uaddr_t uaddr, paddr_t paddr,
                          unsigned attrs) {
    //获取要操作的任务
    struct task *task = task_find(tid);
    if (!task) {
        return ERR_INVALID_TASK;
    }

    //检查是否指定了未知/不允许的标志
    if ((attrs & ~(PAGE_WRITABLE | PAGE_READABLE | PAGE_EXECUTABLE)) != 0) {
        return ERR_INVALID_ARG;
    }

    //检查是否与页面边界对齐
    if (!IS_ALIGNED(uaddr, PAGE_SIZE) || !IS_ALIGNED(paddr, PAGE_SIZE)) {
        return ERR_INVALID_ARG;
    }

    //检查虚拟地址是否可映射
    if (!arch_is_mappable_uaddr(uaddr)) {
        return ERR_INVALID_UADDR;
    }

    attrs |= PAGE_USER;//始终映射为用户页面
    return vm_map(task, uaddr, paddr, attrs);
}

//从虚拟地址空间取消映射页面。
static paddr_t sys_vm_unmap(task_t tid, uaddr_t uaddr) {
    //获取要操作的任务
    struct task *task = task_find(tid);
    if (!task) {
        return ERR_INVALID_TASK;
    }

    //检查是否与页面边界对齐
    if (!IS_ALIGNED(uaddr, PAGE_SIZE)) {
        return ERR_INVALID_ARG;
    }

    //检查虚拟地址是否不可映射
    if (!arch_is_mappable_uaddr(uaddr)) {
        return ERR_INVALID_UADDR;
    }

    return vm_unmap(task, uaddr);
}

//发送和接收消息。
static error_t sys_ipc(task_t dst, task_t src, __user struct message *m,
                       unsigned flags) {
    //检查不允许的标志
    if ((flags & ~(IPC_SEND | IPC_RECV | IPC_NOBLOCK)) != 0) {
        return ERR_INVALID_ARG;
    }

    //检查它是否是有效的任务ID
    if (src < 0 || src > NUM_TASKS_MAX) {
        return ERR_INVALID_ARG;
    }

    //如果包含发送处理，则获取发送目的地任务
    struct task *dst_task = NULL;
    if (flags & IPC_SEND) {
        dst_task = task_find(dst);
        if (!dst_task) {
            return ERR_INVALID_TASK;
        }
    }

    return ipc(dst_task, src, m, flags);
}

//发送通知。
static error_t sys_notify(task_t dst, notifications_t notifications) {
    struct task *dst_task = task_find(dst);
    if (!dst_task) {
        return ERR_INVALID_TASK;
    }

    notify(dst_task, notifications);
    return OK;
}

//订阅未经请求的通知。
static error_t sys_irq_listen(unsigned irq) {
    return irq_listen(CURRENT_TASK, irq);
}

//取消订阅未经请求的通知。
static error_t sys_irq_unlisten(unsigned irq) {
    return irq_unlisten(CURRENT_TASK, irq);
}

//写入串口。
static int sys_serial_write(__user const char *buf, size_t buf_len) {
    //写入串口需要时间，因此限制最大字符数。
    int written_len = MIN(buf_len, 4096);

    char kbuf[512];
    int remaining = written_len;
    while (remaining > 0) {
        //将要写入的字符串复制到临时缓冲区中。
        int copy_len = MIN(remaining, (int) sizeof(kbuf));
        memcpy_from_user(kbuf, buf, copy_len);

        //将临时缓冲区的内容写入串行端口。
        for (int i = 0; i < copy_len; i++) {
            arch_serial_write(kbuf[i]);
        }

        remaining -= copy_len;
    }

    return written_len;
}

//从串口读取。
static int sys_serial_read(__user char *buf, int max_len) {
    //将串行端口接收到的字符串复制到临时缓冲区。
    char tmp[128];
    int len = serial_read((char *) &tmp, MIN(max_len, (int) sizeof(tmp)));

    //将临时缓冲区的内容复制到用户缓冲区。
    memcpy_to_user(buf, tmp, len);
    return len;
}

//设置超时。自调用以来指定的时间（以毫秒为单位）过去后，任务将收到通知。
//发送。如果该值为零，则取消超时。
static error_t sys_time(int timeout) {
    if (timeout < 0) {
        return ERR_INVALID_ARG;
    }

    //更新超时时间
    CURRENT_TASK->timeout = (timeout == 0) ? 0 : (timeout * (TICK_HZ / 1000));
    return OK;
}

//返回自启动以来经过的时间（以毫秒为单位）。
static int sys_uptime(void) {
    return uptime_ticks / TICK_HZ;
}

//关闭你的电脑。
__noreturn static int sys_shutdown(void) {
    arch_shutdown();
}

//系统调用处理程序
long handle_syscall(long a0, long a1, long a2, long a3, long a4, long n) {
    long ret;
    switch (n) {
        case SYS_IPC:
            ret = sys_ipc(a0, a1, (__user struct message *) a2, a3);
            break;
        case SYS_NOTIFY:
            ret = sys_notify(a0, a1);
            break;
        case SYS_SERIAL_WRITE:
            ret = sys_serial_write((__user const char *) a0, a1);
            break;
        case SYS_SERIAL_READ:
            ret = sys_serial_read((__user char *) a0, a1);
            break;
        case SYS_TASK_CREATE:
            ret = sys_task_create((__user const char *) a0, a1, a2);
            break;
        case SYS_TASK_DESTROY:
            ret = sys_task_destroy(a0);
            break;
        case SYS_TASK_EXIT:
            sys_task_exit();
            UNREACHABLE();
        case SYS_TASK_SELF:
            ret = sys_task_self();
            break;
        case SYS_PM_ALLOC:
            ret = sys_pm_alloc(a0, a1, a2);
            break;
        case SYS_VM_MAP:
            ret = sys_vm_map(a0, a1, a2, a3);
            break;
        case SYS_VM_UNMAP:
            ret = sys_vm_unmap(a0, a1);
            break;
        case SYS_IRQ_LISTEN:
            ret = sys_irq_listen(a0);
            break;
        case SYS_IRQ_UNLISTEN:
            ret = sys_irq_unlisten(a0);
            break;
        case SYS_HINAVM:
            ret = sys_hinavm((__user const char *) a0,
                             (__user hinavm_inst_t *) a1, a2, a3);
            break;
        case SYS_TIME:
            ret = sys_time(a0);
            break;
        case SYS_UPTIME:
            ret = sys_uptime();
            break;
        case SYS_SHUTDOWN:
            ret = sys_shutdown();
            break;
        default:
            ret = ERR_INVALID_ARG;
    }

    return ret;
}
