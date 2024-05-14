#pragma once
#include "arch.h"
#include "hinavm.h"
#include "interrupt.h"
#include <libs/common/list.h>
#include <libs/common/message.h>
#include <libs/common/types.h>

// 任务的最大连续执行时间
#define TASK_QUANTUM (20 * (TICK_HZ / 1000))/*20毫秒*/
// 当前CPU空闲任务（struct task *）
#define IDLE_TASK (arch_cpuvar_get()->idle_task)
// 正在运行的任务（结构任务*）
#define CURRENT_TASK (arch_cpuvar_get()->current_task)

// 任务状态
#define TASK_UNUSED   0
#define TASK_RUNNABLE 1
#define TASK_BLOCKED  2

// 任务管理结构
struct task {
    struct arch_task arch;          // 依赖于CPU的任务信息
    struct arch_vm vm;              // 页表
    task_t tid;                     // 任务ID
    char name[TASK_NAME_LEN];       // 任务名称
    int state;                      // 任务状态
    bool destroyed;                 // 任务是否正在被删除？
    struct task *pager;             // 寻呼机任务
    unsigned timeout;               // 剩余超时时间
    int ref_count;                  // 任务被引用的次数（不为零则无法删除）
    unsigned quantum;               // 任务剩余量
    list_elem_t waitqueue_next;     // 指向每个等待列表中下一个元素的指针
    list_elem_t next;               // 指向完整任务列表中下一个元素的指针
    list_t senders;                 // 等待发送到该任务的任务列表
    task_t wait_for;                // 可以向该任务发送消息的任务ID
                                    // （全部针对IPC_ANY）
    list_t pages;                   // 正在使用的内存页列表
    notifications_t notifications;  // 收到通知
    struct message m;               // 消息临时存储区
};

extern list_t active_tasks;

struct task *task_find(task_t tid);
task_t task_create(const char *name, uaddr_t ip, struct task *pager);
task_t hinavm_create(const char *name, hinavm_inst_t *insts, uint32_t num_insts,
                     struct task *pager);
error_t task_destroy(struct task *task);
__noreturn void task_exit(int exception);
void task_resume(struct task *task);
void task_block(struct task *task);
void task_switch(void);
void task_dump(void);
void task_init_percpu(void);
