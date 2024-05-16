#include "interrupt.h"
#include "arch.h"
#include "ipc.h"
#include "task.h"
#include <libs/common/print.h>

//接受中断通知的任务列表。
static struct task *irq_listeners[IRQ_MAX];
//自启动以来经过的时间。单位取决于定时器中断周期（TICK_HZ）。
unsigned uptime_ticks = 0;

//允许接受中断通知。
error_t irq_listen(struct task *task, unsigned irq) {
    if (irq >= IRQ_MAX) {
        return ERR_INVALID_ARG;
    }

    if (irq_listeners[irq] != NULL) {
        return ERR_ALREADY_USED;
    }

    error_t err = arch_irq_enable(irq);
    if (err != OK) {
        return err;
    }

    irq_listeners[irq] = task;
    return OK;
}

//防止接受中断通知。
error_t irq_unlisten(struct task *task, unsigned irq) {
    if (irq >= IRQ_MAX) {
        return ERR_INVALID_ARG;
    }

    if (irq_listeners[irq] != task) {
        return ERR_NOT_ALLOWED;
    }

    error_t err = arch_irq_disable(irq);
    if (err != OK) {
        return err;
    }

    irq_listeners[irq] = NULL;
    return OK;
}

//硬件中断处理程序（定时器中断除外）
void handle_interrupt(unsigned irq) {
    if (irq >= IRQ_MAX) {
        WARN("invalid IRQ: %u", irq);
        return;
    }

    //获取接受中断并发送通知的任务。
    struct task *task = irq_listeners[irq];
    if (!task) {
        WARN("unhandled IRQ %u", irq);
        return;
    }

    notify(task, NOTIFY_IRQ);
}

//定时器中断处理程序
void handle_timer_interrupt(unsigned ticks) {
    //更新自启动以来经过的时间
    uptime_ticks += ticks;

    if (CPUVAR->id == 0) {
        //更新每个任务的计时器
        LIST_FOR_EACH (task, &active_tasks, struct task, next) {
            if (task->timeout > 0) {
                task->timeout -= MIN(task->timeout, ticks);
                if (!task->timeout) {
                    //通知任务已超时
                    notify(task, NOTIFY_TIMER);
                }
            }
        }
    }

    //更新正在运行的任务的剩余可运行时间，当剩余可运行时间为零时切换任务。
    struct task *current = CURRENT_TASK;
    DEBUG_ASSERT(current->quantum >= 0 || current == IDLE_TASK);
    current->quantum -= MIN(ticks, current->quantum);
    if (!current->quantum) {
        task_switch();
    }
}
