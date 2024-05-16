#pragma once
#include "ipcstub.h"

#define IPC_ANY  0
#define IPC_DENY -1

#define IPC_SEND    (1 << 16)
#define IPC_RECV    (1 << 17)
#define IPC_NOBLOCK (1 << 18)
#define IPC_KERNEL  (1 << 19)
#define IPC_CALL    (IPC_SEND | IPC_RECV)

#define NOTIFY_TIMER       (1 << 0)
#define NOTIFY_IRQ         (1 << 1)
#define NOTIFY_ABORTED     (1 << 2)
#define NOTIFY_ASYNC_BASE  3
#define NOTIFY_ASYNC(tid)  (1 << (NOTIFY_ASYNC_BASE + tid))
#define NOTIFY_ASYNC_START (NOTIFY_ASYNC(0))
#define NOTIFY_ASYNC_END   (NOTIFY_ASYNC(NUM_TASKS_MAX))

//每个任务在通知中都有一个专用的异步位字段
STATIC_ASSERT(NOTIFY_ASYNC_BASE + NUM_TASKS_MAX < sizeof(notifications_t) * 8,
              "too many tasks for notifications_t");

struct message {
    int32_t type;//消息类型（负数则为错误值）
    task_t src;//消息源
    union {
        uint8_t data[0];//指向消息数据的开头
///每个自动生成的消息的字段定义：
//
//结构体{ int x };
//结构体 { int 答案 } add_reply;
//...
//
        IPCSTUB_MESSAGE_FIELDS
    };
};

STATIC_ASSERT(sizeof(struct message) < 2048,
              "sizeof(struct message) too large");

const char *msgtype2str(int type);
