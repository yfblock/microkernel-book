#include <libs/common/list.h>
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/ipc.h>
#include <libs/user/malloc.h>
#include <libs/user/syscall.h>
#include <libs/user/task.h>

//异步消息
struct async_message {
    list_elem_t next;//发送队列列表
    task_t dst;//目标任务
    struct message m;//信息
};

//从该任务发送到其他任务的异步消息的列表。
//当有来自另一个任务的查询（ASYNC_RECV_MSG）时，将从该列表中搜索消息。
static list_t async_messages = LIST_INIT(async_messages);
//收到通知（位字段）。
static notifications_t pending_notifications = 0;

//接收ASYNC_RECV_MSG时的处理（非阻塞）
static error_t async_reply(task_t dst) {
    //查找从发送队列到 dst 的未发送消息
    bool sent = false;
    LIST_FOR_EACH (am, &async_messages, struct async_message, next) {
        if (am->dst == dst) {
            if (sent) {
                //如果已发送一条消息，ipc_reply 将失败
//（目标任务未处于接收等待状态），请发送通知。
//再次发送ASYNC_RECV_MSG。
                return ipc_notify(dst, NOTIFY_ASYNC(task_self()));
            }

            //回复未发送的消息
            ipc_reply(dst, &am->m);
            list_remove(&am->next);
            free(am);
            sent = true;
        }
    }

    return OK;
}

//发送异步消息（非阻塞）
error_t ipc_send_async(task_t dst, struct message *m) {
    //将消息插入发送队列
    struct async_message *am = malloc(sizeof(*am));
    am->dst = dst;
    memcpy(&am->m, m, sizeof(am->m));
    list_elem_init(&am->next);
    list_push_back(&async_messages, &am->next);

    //向目标任务发送通知
    return ipc_notify(dst, NOTIFY_ASYNC(task_self()));
}

//发送一个消息。阻塞直到目标任务处于接收状态。
error_t ipc_send(task_t dst, struct message *m) {
    return sys_ipc(dst, 0, m, IPC_SEND);
}

//发送一个消息。如果消息发送不能立即完成，则返回ERR_WOULD_BLOCK。
error_t ipc_send_noblock(task_t dst, struct message *m) {
    return sys_ipc(dst, 0, m, IPC_SEND | IPC_NOBLOCK);
}

//发送一个消息。如果无法立即完成消息发送，则会输出警告消息。
//丢弃该消息。
void ipc_reply(task_t dst, struct message *m) {
    error_t err = ipc_send_noblock(dst, m);
    OOPS_OK(err);
}

//发送错误消息。如果无法立即完成消息发送，则会输出警告消息。
//丢弃该消息。
void ipc_reply_err(task_t dst, error_t error) {
    struct message m;
    m.type = error;
    ipc_reply(dst, &m);
}

//获取收到的通知之一并将其转换为消息。另外，异步消息的接收流程是
//透明地做。
static error_t recv_notification_as_message(struct message *m) {
    error_t err;

    //检查接收到的通知位字段中设置了什么位。
    int index = __builtin_ffsll(pending_notifications) - 1;
    DEBUG_ASSERT(index >= 0);

    //根据通知类型创建消息。
    switch (1 << index) {
        //中断通知
        case NOTIFY_IRQ:
            m->type = NOTIFY_IRQ_MSG;
            err = OK;
            break;
        //超时通知
        case NOTIFY_TIMER:
            m->type = NOTIFY_TIMER_MSG;
            err = OK;
            break;
        //异步消息接收通知
        case NOTIFY_ASYNC_START ... NOTIFY_ASYNC_END: {
            //查询通知发送者是否有待处理的消息
            task_t src = index - NOTIFY_ASYNC_BASE;
            m->type = ASYNC_RECV_MSG;
            err = ipc_call(src, m);
            break;
        }
        case NOTIFY_ABORTED:
            //此通知在内核内部使用，因此不应出现在此处。
        default:
            PANIC("unhandled notification: %x (index=%d)",
                  pending_notifications, index);
    }

    //删除已处理的通知
    pending_notifications &= ~(1 << index);
    return err;
}

//接收来自任何任务的消息（开放接收）。通知/异步消息传递
//处理也是透明执行的。
static error_t ipc_recv_any(struct message *m) {
    while (true) {
        //如果有收到通知，则将通知转换为消息并返回。
        if (pending_notifications) {
            return recv_notification_as_message(m);
        }

        //接收消息。
        error_t err = sys_ipc(0, IPC_ANY, m, IPC_RECV);
        if (err != OK) {
            return err;
        }

        //根据消息类型进行处理。
        switch (m->type) {
            //通知处理：将通知添加到收到的通知位字段。
            case NOTIFY_MSG:
                if (m->src != FROM_KERNEL) {
                    WARN(
                        "received a notification from a non-kernel task #%d, ignoring",
                        m->src);
                    continue;
                }

                pending_notifications |= m->notify.notifications;
                return recv_notification_as_message(m);
            //异步消息查询处理：将任何异步消息返回给发送方任务。
            case ASYNC_RECV_MSG: {
                error_t err = async_reply(m->src);
                if (err != OK) {
                    WARN("failed to send a async message to #%d: %s", m->src,
                         err2str(err));
                }
                continue;
            }
            //其他消息：如果没有错误，则按原样返回。
            default:
                if (IS_ERROR(m->type)) {
                    return m->type;
                }

                return OK;
        }
    }
}

//接收消息。阻塞直到消息到达。
//
//如果 src 为 IPC_ANY，则从任何任务接收消息（开放接收）。
error_t ipc_recv(task_t src, struct message *m) {
    if (src == IPC_ANY) {
        //打开接收
        return ipc_recv_any(m);
    }

    //封闭式接待
    error_t err = sys_ipc(0, src, m, IPC_RECV);
    if (err != OK) {
        return err;
    }

    //如果返回错误消息，则返回该错误。
    if (IS_ERROR(m->type)) {
        return m->type;
    }

    return OK;
}

//发送消息并等待收件人的消息。
error_t ipc_call(task_t dst, struct message *m) {
    error_t err = sys_ipc(dst, dst, m, IPC_CALL);
    if (err != OK) {
        return err;
    }

    //如果返回错误消息，则返回该错误。
    if (IS_ERROR(m->type)) {
        return m->type;
    }

    return OK;
}

//发送通知。
error_t ipc_notify(task_t dst, notifications_t notifications) {
    return sys_notify(dst, notifications);
}

//注册您的服务。
error_t ipc_register(const char *name) {
    struct message m;
    m.type = SERVICE_REGISTER_MSG;
    strcpy_safe(m.service_register.name, sizeof(m.service_register.name), name);
    return ipc_call(VM_SERVER, &m);
}

//从服务名称中搜索任务 ID。阻塞直到服务注册。
task_t ipc_lookup(const char *name) {
    struct message m;
    m.type = SERVICE_LOOKUP_MSG;
    strcpy_safe(m.service_lookup.name, sizeof(m.service_lookup.name), name);
    error_t err = ipc_call(VM_SERVER, &m);
    if (err != OK) {
        return err;
    }

    ASSERT(m.type == SERVICE_LOOKUP_REPLY_MSG);
    return m.service_lookup_reply.task;
}
