#include "task.h"
#include "arch.h"
#include "ipc.h"
#include "memory.h"
#include "printk.h"
#include <libs/common/list.h>
#include <libs/common/string.h>

static struct task tasks[NUM_TASKS_MAX];        //所有任务管理结构（包括未使用的）
static struct task idle_tasks[NUM_CPUS_MAX];    //每个CPU的空闲任务
static list_t runqueue = LIST_INIT(runqueue);   //运行队列
list_t active_tasks = LIST_INIT(active_tasks);  //正在使用的管理结构列表
//选择下一个要执行的任务。
static struct task *scheduler(void) {
    //从运行队列中检索可执行任务。
    struct task *next = LIST_POP_FRONT(&runqueue, struct task, waitqueue_next);
    if (next) {
        return next;
    }

    if (CURRENT_TASK->state == TASK_RUNNABLE && !CURRENT_TASK->destroyed) {
        //如果没有其他任务可以执行，则继续正在运行的任务。
        return CURRENT_TASK;
    }

    return IDLE_TASK;//如果没有任务可运行，则运行空闲任务。
}

//初始化任务管理结构。
static error_t init_task_struct(struct task *task, task_t tid, const char *name,
                                vaddr_t ip, struct task *pager,
                                vaddr_t kernel_entry, void *arg) {
    task->tid = tid;
    task->destroyed = false;
    task->quantum = 0;
    task->timeout = 0;
    task->wait_for = IPC_DENY;
    task->ref_count = 0;
    task->pager = pager;

    strcpy_safe(task->name, sizeof(task->name), name);
    list_elem_init(&task->waitqueue_next);
    list_elem_init(&task->next);
    list_init(&task->senders);
    list_init(&task->pages);

    error_t err = arch_vm_init(&task->vm);
    if (err != OK) {
        return err;
    }

    err = arch_task_init(task, ip, kernel_entry, arg);
    if (err != OK) {
        arch_vm_destroy(&task->vm);
        return err;
    }

    if (pager) {
        pager->ref_count++;
    }

    task->state = TASK_BLOCKED;
    return OK;
}

//执行自发的任务切换。如果除了当前正在运行的任务之外没有其他可执行任务，则立即
//回来。否则，执行转移到另一个任务，下次再调度该任务
//回来。
void task_switch(void) {
    struct task *prev = CURRENT_TASK;//运行任务
    struct task *next = scheduler();//下一个要执行的任务

    //将 CPU 时间分配给下一个要运行的任务
    if (next != IDLE_TASK) {
        next->quantum = TASK_QUANTUM;
    }

    if (next == prev) {
        //除了当前正在运行的任务之外，没有其他可执行任务。返回并继续处理。
        return;
    }

    if (prev->state == TASK_RUNNABLE) {
        //如果正在进行的任务可执行，则将其返回到可执行任务队列。
//当分配的 CPU 时间用完时发生。
        list_push_back(&runqueue, &prev->waitqueue_next);
    }

    //切换任务
    CURRENT_TASK = next;
    arch_task_switch(prev, next);
}

//查找未使用的任务 ID。
static task_t alloc_tid(void) {
    for (task_t i = 0; i < NUM_TASKS_MAX; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            return i + 1;
        }
    }

    return 0;
}

//从任务ID获取任务管理结构。如果不存在或具有无效 id，则返回 null。
struct task *task_find(task_t tid) {
    if (tid < 0 || tid >= NUM_TASKS_MAX) {
        return NULL;
    }

    struct task *task = &tasks[tid - 1];
    if (task->state == TASK_UNUSED) {
        return NULL;
    }

    return task;
}

//将任务置于阻塞状态。如果你想阻止正在运行的任务本身，请使用task_switch函数。
//有必要调用它并将执行转移到另一个任务。
void task_block(struct task *task) {
    DEBUG_ASSERT(task != IDLE_TASK);
    DEBUG_ASSERT(task->state == TASK_RUNNABLE);

    task->state = TASK_BLOCKED;
}

//使任务可执行。
void task_resume(struct task *task) {
    DEBUG_ASSERT(task->state == TASK_BLOCKED);

    task->state = TASK_RUNNABLE;
    list_push_back(&runqueue, &task->waitqueue_next);
}

//创建任务。 ip 是在用户模式下运行的地址（入口点），寻呼机是
//寻呼机任务。
task_t task_create(const char *name, uaddr_t ip, struct task *pager) {
    task_t tid = alloc_tid();
    if (!tid) {
        return ERR_TOO_MANY_TASKS;
    }

    struct task *task = &tasks[tid - 1];
    DEBUG_ASSERT(task != NULL);

    error_t err = init_task_struct(task, tid, name, ip, pager, 0, NULL);
    if (err != OK) {
        return err;
    }

    list_push_back(&active_tasks, &task->next);
    task_resume(task);
    TRACE("created a task \"%s\" (tid=%d)", name, tid);
    return tid;
}

//创建 HinaVM 任务。 insts 为 HinaVM 指令序列，num_insts 为指令数量，pager 为分页任务。之所以写在这里而不是hinavm.c，是为了调用init_task_struct函数等。
task_t hinavm_create(const char *name, hinavm_inst_t *insts, uint32_t num_insts,
                     struct task *pager) {
    task_t tid = alloc_tid();
    if (!tid) {
        return ERR_TOO_MANY_TASKS;
    }

    struct task *task = &tasks[tid - 1];
    DEBUG_ASSERT(task != NULL);

    size_t hinavm_size = ALIGN_UP(sizeof(struct hinavm), PAGE_SIZE);
    paddr_t hinavm_paddr = pm_alloc(hinavm_size, NULL, PM_ALLOC_UNINITIALIZED);
    if (!hinavm_paddr) {
        return ERR_NO_MEMORY;
    }

    struct hinavm *hinavm = (struct hinavm *) arch_paddr_to_vaddr(hinavm_paddr);
    memcpy(&hinavm->insts, insts, sizeof(hinavm_inst_t) * num_insts);
    hinavm->num_insts = num_insts;

    error_t err = init_task_struct(task, tid, name, 0, pager,
                                   (vaddr_t) hinavm_run, hinavm);
    if (err != OK) {
        pm_free(hinavm_paddr, hinavm_size);
        return err;
    }

    pm_own_page(hinavm_paddr, task);
    list_push_back(&active_tasks, &task->next);
    task_resume(task);
    TRACE("created a HinaVM task \"%s\" (tid=%d)", name, tid);
    return tid;
}

//删除任务。 task 是要删除的任务。如果任务是正在运行的任务，则此函数
//相反，您需要调用task_exit 函数。
error_t task_destroy(struct task *task) {
    DEBUG_ASSERT(task != CURRENT_TASK);
    DEBUG_ASSERT(task != IDLE_TASK);
    DEBUG_ASSERT(task->state != TASK_UNUSED);
    DEBUG_ASSERT(task->ref_count >= 0);

    if (task->tid == 1) {
        //第一个用户任务（虚拟机服务器）无法删除。
        WARN("tried to destroy the task #1");
        return ERR_INVALID_ARG;
    }

    if (task->ref_count > 0) {
        //如果被另一个任务引用（注册为另一个任务的寻呼任务）
//无法删除。
        WARN("%s (#%d) is still referenced from %d tasks", task->name,
             task->tid, task->ref_count);
        return ERR_STILL_USED;
    }

    TRACE("destroying a task \"%s\" (tid=%d)", task->name, task->tid);

    //通过记录删除正在进行中，接收到以下处理器间中断的其他CPU可以
//防止调度程序再次选择此任务。否则，如果除了这个任务
//如果没有可执行任务，下面的循环可能会永远运行。
    task->destroyed = true;

    //等待其他CPU中断该任务的执行。
    while (true) {
        //如果任务被阻止，则它当前显然没有运行。
        if (task->state != TASK_RUNNABLE) {
            break;
        }

        //即使任务已准备好运行，如果它未包含在运行队列中，则当前不会执行该任务。
        if (list_contains(&runqueue, &task->waitqueue_next)) {
            break;
        }

        //另一个CPU当前正在执行该任务。发送 ipi 提示上下文切换。
        arch_send_ipi(IPI_RESCHEDULE);
    }

    //如果有任何任务尝试向该任务发送消息，则这些发送进程将被中断。
    LIST_FOR_EACH (sender, &task->senders, struct task, waitqueue_next) {
        notify(sender, NOTIFY_ABORTED);
    }

    //从内核中删除任务。
    list_remove(&task->next);
    list_remove(&task->waitqueue_next);
    arch_vm_destroy(&task->vm);
    arch_task_destroy(task);
    pm_free_by_list(&task->pages);
    task->state = TASK_UNUSED;
    task->pager->ref_count--;
    return OK;
}

//终止正在运行的任务 (CURRENT_TASK)。参数异常是终止的原因。
__noreturn void task_exit(int exception) {
    struct task *pager = CURRENT_TASK->pager;
    ASSERT(pager != NULL);

    TRACE("exiting a task \"%s\" (tid=%d)", CURRENT_TASK->name,
          CURRENT_TASK->tid);

    //通知寻呼任务终止原因。寻呼任务调用task_destroy系统调用。
//调用这个实际上会删除这个任务。
    struct message m;
    m.type = EXCEPTION_MSG;
    m.exception.task = CURRENT_TASK->tid;
    m.exception.reason = exception;
    error_t err = ipc(CURRENT_TASK->pager, IPC_DENY,
                      (__user struct message *) &m, IPC_SEND | IPC_KERNEL);

    if (err != OK) {
        WARN("%s: failed to send an exit message to '%s': %s",
             CURRENT_TASK->name, pager->name, err2str(err));
    }

    //执行其他任务。我再也不会回到这个任务了。
    task_block(CURRENT_TASK);
    task_switch();

    UNREACHABLE();
}

//显示每个任务的当前状态以进行调试。对于发生死锁时调查原因很有用。
//在串行端口上按下 Ctrl-P 时调用。
void task_dump(void) {
    WARN("active tasks:");
    LIST_FOR_EACH (task, &active_tasks, struct task, next) {
        switch (task->state) {
            case TASK_RUNNABLE:
                WARN("  #%d: %s: RUNNABLE", task->tid, task->name);
                LIST_FOR_EACH (sender, &task->senders, struct task,
                               waitqueue_next) {
                    WARN("    blocked sender: #%d: %s", sender->tid,
                         sender->name);
                }
                break;
            case TASK_BLOCKED:
                switch (task->wait_for) {
                    case IPC_DENY:
                        WARN(
                            "  #%d: %s: BLOCKED (send, serial_read, or exited)",
                            task->tid, task->name);
                        break;
                    case IPC_ANY:
                        WARN("  #%d: %s: BLOCKED (open receive)", task->tid,
                             task->name);
                        break;
                    default:
                        WARN("  #%d: %s: BLOCKED (closed receive from #%d)",
                             task->tid, task->name, task->wait_for);
                }
                break;
            default:
                UNREACHABLE();
        }
    }
}

//初始化任务管理系统
void task_init_percpu(void) {
    //为每个CPU创建一个空闲任务，并将其设为运行任务。
    struct task *idle_task = &idle_tasks[CPUVAR->id];
    ASSERT_OK(init_task_struct(idle_task, 0, "(idle)", 0, NULL, 0, NULL));
    IDLE_TASK = idle_task;
    CURRENT_TASK = IDLE_TASK;
}
