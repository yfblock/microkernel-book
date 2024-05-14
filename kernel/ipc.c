#include "ipc.h"
#include "syscall.h"
#include "task.h"
#include <libs/common/list.h>
#include <libs/common/message.h>
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/common/types.h>

// 消息发送流程
static error_t send_message(struct task *dst, __user struct message *m,
                            unsigned flags) {
    // 我无法给自己发送消息
    struct task *current = CURRENT_TASK;
    if (dst == current) {
        WARN("%s: tried to send a message to itself", current->name);
        return ERR_INVALID_ARG;
    }

    // 复制您要发送的消息。用户指针情况下可能出现页面错误
    // 请注意，有
    struct message copied_m;
    if (flags & IPC_KERNEL) {
        memcpy(&copied_m, (struct message *) m, sizeof(struct message));
    } else {
        error_t err = memcpy_from_user(&copied_m, m, sizeof(struct message));
        if (err != OK) {
            return err;
        }
    }

    // 检查收件人是否正在等待您的消息
    bool ready = dst->state == TASK_BLOCKED
                 && (dst->wait_for == IPC_ANY || dst->wait_for == current->tid);
    if (!ready) {
        if (flags & IPC_NOBLOCK) {
            return ERR_WOULD_BLOCK;
        }

        // 如果它们尝试互相发送消息，就会发生死锁并返回错误。
        LIST_FOR_EACH (task, &current->senders, struct task, waitqueue_next) {
            if (task->tid == dst->tid) {
                WARN(
                    "dead lock detected: %s (#%d) and %s (#%d) are trying to"
                    " send messages to each other"
                    " (hint: consider using ipc_send_async())",
                    current->name, current->tid, dst->name, dst->tid);
                return ERR_DEAD_LOCK;
            }
        }

        // 将正在运行的任务添加到目标的发送队列并将其置于阻塞状态
        list_push_back(&dst->senders, &current->waitqueue_next);
        task_block(current);

        // 将 CPU 让给其他任务。当目标任务处于接收状态时，该任务将恢复。
        task_switch();

        // 如果目标任务完成则中断发送过程
        if (current->notifications & NOTIFY_ABORTED) {
            current->notifications &= ~NOTIFY_ABORTED;
            return ERR_ABORTED;
        }
    }

    // 发送消息并恢复目标任务
    memcpy(&dst->m, &copied_m, sizeof(struct message));
    dst->m.src = (flags & IPC_KERNEL) ? FROM_KERNEL : current->tid;
    task_resume(dst);
    return OK;
}

// 消息接收处理
static error_t recv_message(task_t src, __user struct message *m,
                            unsigned flags) {
    struct task *current = CURRENT_TASK;
    struct message copied_m;
    if (src == IPC_ANY && current->notifications) {
        //以消息形式接收通知（如果有）
        copied_m.type = NOTIFY_MSG;
        copied_m.src = FROM_KERNEL;
        copied_m.notify.notifications = current->notifications;
        current->notifications = 0;
    } else {
        if (flags & IPC_NOBLOCK) {
            return ERR_WOULD_BLOCK;
        }

        //如果发送队列中有匹配`src`的任务，则重新启动它
        LIST_FOR_EACH (sender, &current->senders, struct task, waitqueue_next) {
            if (src == IPC_ANY || src == sender->tid) {
                DEBUG_ASSERT(sender->state == TASK_BLOCKED);
                DEBUG_ASSERT(sender->wait_for == IPC_DENY);
                list_remove(&sender->waitqueue_next);
                task_resume(sender);
                src = sender->tid;
                break;
            }
        }

        //等待收到消息
        current->wait_for = src;
        task_block(current);
        task_switch();

        //收到消息
        current->wait_for = IPC_DENY;
        memcpy(&copied_m, &current->m, sizeof(struct message));
    }

    //复制收到的消息。用户指针情况下可能出现页面错误
//请注意，有
    if (flags & IPC_KERNEL) {
        memcpy((void *) m, &copied_m, sizeof(struct message));
    } else {
        error_t err = memcpy_to_user(m, &copied_m, sizeof(struct message));
        if (err != OK) {
            return err;
        }
    }

    return OK;
}

//发送和接收消息。
error_t ipc(struct task *dst, task_t src, __user struct message *m,
            unsigned flags) {
    //发送操作
    if (flags & IPC_SEND) {
        error_t err = send_message(dst, m, flags);
        if (err != OK) {
            return err;
        }
    }

    //接收操作
    if (flags & IPC_RECV) {
        error_t err = recv_message(src, m, flags);
        if (err != OK) {
            return err;
        }
    }

    return OK;
}

//发送通知。
void notify(struct task *dst, notifications_t notifications) {
    if (dst->state == TASK_BLOCKED && dst->wait_for == IPC_ANY) {
        //目标任务正在等待打开接收状态。在发送 NOTIFY_MSG 消息的正文中
//立即发送通知。
        dst->m.type = NOTIFY_MSG;
        dst->m.src = FROM_KERNEL;
        dst->m.notify.notifications = dst->notifications | notifications;
        dst->notifications = 0;
        task_resume(dst);
    } else {
        //保留通知，直到目标任务打开接收。
        dst->notifications |= notifications;
    }
}
