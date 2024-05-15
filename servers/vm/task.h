#pragma once
#include <libs/common/elf.h>
#include <libs/common/list.h>
#include <libs/common/types.h>

//服务名称的最大长度
#define SERVICE_NAME_LEN 64
//动态分配虚拟地址的起始地址
#define VALLOC_BASE 0x20000000
//动态分配的虚拟地址的结束地址
#define VALLOC_END 0x40000000

//服务管理架构。它维护服务名称和任务ID之间的对应关系，用于服务发现。
struct service {
    list_elem_t next;
    char name[SERVICE_NAME_LEN];//服务名称
    task_t task;//任务ID
};

//任务管理结构
struct bootfs_file;
struct task {
    task_t tid;//任务ID
    task_t pager;//寻呼机任务ID
    char name[TASK_NAME_LEN];//任务名称
    void *file_header;//指向ELF文件的开头
    struct bootfs_file *file;//BootFS 上的 ELF 文件
    elf_ehdr_t *ehdr;//ELF 头
    elf_phdr_t *phdrs;//程序头
    uaddr_t valloc_next;//下一个动态分配的虚拟地址
    char waiting_for[SERVICE_NAME_LEN];//等待服务注册的服务名
    bool watch_tasks;//是否监控任务完成情况
};

struct task *task_find(task_t tid);
task_t task_spawn(struct bootfs_file *file);
void task_destroy(struct task *task);
error_t task_destroy_by_tid(task_t tid);
void service_register(struct task *task, const char *name);
task_t service_lookup_or_wait(struct task *task, const char *name);
void service_dump(void);
