#include "task.h"
#include "bootfs.h"
#include <libs/common/elf.h>
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/ipc.h>
#include <libs/user/malloc.h>
#include <libs/user/syscall.h>
#include <libs/user/task.h>

static struct task *tasks[NUM_TASKS_MAX];//任务管理结构
static list_t services = LIST_INIT(services);//服务管理结构列表
//从任务ID获取任务管理结构。
struct task *task_find(task_t tid) {
    if (tid <= 0 || tid > NUM_TASKS_MAX) {
        PANIC("invalid tid %d", tid);
    }

    return tasks[tid - 1];
}

//从指定的 elf 文件生成任务。如果成功则返回任务 ID，如果不成功则返回错误。
task_t task_spawn(struct bootfs_file *file) {
    TRACE("launching %s...", file->name);
    struct task *task = malloc(sizeof(*task));
    if (!task) {
        PANIC("too many tasks");
    }

    //读取elf文件的前4096字节以访问elf程序头。
    void *file_header = malloc(4096);
    bootfs_read(file, 0, file_header, PAGE_SIZE);

    //检查是否是Elf文件。
    elf_ehdr_t *ehdr = (elf_ehdr_t *) file_header;
    if (memcmp(ehdr->e_ident, ELF_MAGIC, 4) != 0) {
        WARN("%s: invalid ELF magic", file->name);
        free(file_header);
        return ERR_INVALID_ARG;
    }

    //检查该文件是否可执行。
    if (ehdr->e_type != ET_EXEC) {
        WARN("%s: not an executable file", file->name);
        free(file_header);
        return ERR_INVALID_ARG;
    }

    //如果程序头太多，则 file_header 无法容纳，并会导致错误。如果有32个
//那应该足够了。
    if (ehdr->e_phnum > 32) {
        WARN("%s: too many program headers", file->name);
        free(file_header);
        return ERR_INVALID_ARG;
    }

    //强制内核生成新任务。
    task_t tid_or_err = sys_task_create(file->name, ehdr->e_entry, task_self());
    if (IS_ERROR(tid_or_err)) {
        return tid_or_err;
    }

    //初始化任务管理结构。
    task->file = file;
    task->file_header = file_header;
    task->tid = tid_or_err;
    task->pager = task_self();
    task->ehdr = ehdr;
    task->phdrs = (elf_phdr_t *) ((uaddr_t) file_header + ehdr->e_phoff);
    task->watch_tasks = false;
    strcpy_safe(task->waiting_for, sizeof(task->waiting_for), "");

    //在虚拟地址空间中搜索空虚拟地址区域的开头。动态创建虚拟地址
//分配时避免与ELF段重叠。
    vaddr_t valloc_next = VALLOC_BASE;
    for (unsigned i = 0; i < task->ehdr->e_phnum; i++) {
        elf_phdr_t *phdr = &task->phdrs[i];
        if (phdr->p_type != PT_LOAD) {
            //忽略不在内存中的段。
            continue;
        }

        uaddr_t end = ALIGN_UP(phdr->p_vaddr + phdr->p_memsz, PAGE_SIZE);
        valloc_next = MAX(valloc_next, end);
    }

    //现在您知道了该片段的结尾，请将其记录下来。虚拟地址区域是从此地址动态扩展的。
//将被分配。
    ASSERT(VALLOC_BASE <= valloc_next && valloc_next < VALLOC_END);
    task->valloc_next = valloc_next;

    //将 Elf 段映射到虚拟地址空间。
    strcpy_safe(task->name, sizeof(task->name), file->name);

    //在任务id表中注册任务管理结构。
    tasks[task->tid - 1] = task;
    return task->tid;
}

//完成任务。
void task_destroy(struct task *task) {
    for (int i = 0; i < NUM_TASKS_MAX; i++) {
        //通知监控任务任务完成。
        struct task *server = tasks[i];
        if (server && server->watch_tasks) {
            struct message m;
            m.type = TASK_DESTROYED_MSG;
            m.task_destroyed.task = task->tid;
            ipc_send_async(server->tid, &m);
        }
    }

    //让内核终止任务。
    OOPS_OK(sys_task_destroy(task->tid));
    free(task->file_header);
    free(task);

    //从任务id表中删除任务管理结构。
    tasks[task_self() - 1] = NULL;
}

//指定任务ID并结束任务。
error_t task_destroy_by_tid(task_t tid) {
    for (int i = 0; i < NUM_TASKS_MAX; i++) {
        struct task *task = tasks[i];
        if (task && task->tid == tid) {
            task_destroy(task);
            return OK;
        }
    }

    return ERR_NOT_FOUND;
}

//注册您的服务。
void service_register(struct task *task, const char *name) {
    //注册您的服务。
    struct service *service = malloc(sizeof(*service));
    service->task = task->tid;
    strcpy_safe(service->name, sizeof(service->name), name);
    list_elem_init(&service->next);
    list_push_back(&services, &service->next);
    INFO("service \"%s\" is up", name);

    //如果有任务正在等待该服务，则回复该任务以将其从等待状态释放。
    for (int i = 0; i < NUM_TASKS_MAX; i++) {
        struct task *task = tasks[i];
        if (task && !strcmp(task->waiting_for, name)) {
            struct message m;
            m.type = SERVICE_LOOKUP_REPLY_MSG;
            m.service_lookup_reply.task = service->task;
            ipc_reply(task->tid, &m);

            //我不会再等了，所以我会清除它。
            strcpy_safe(task->waiting_for, sizeof(task->waiting_for), "");
        }
    }
}

//返回服务名称对应的任务ID。 ERR_WOULD_BLOCK 如果服务尚未注册
//把它返还。
task_t service_lookup_or_wait(struct task *task, const char *name) {
    LIST_FOR_EACH (s, &services, struct service, next) {
        if (!strcmp(s->name, name)) {
            return s->task;
        }
    }

    TRACE("%s: waiting for service \"%s\"", task->name, name);
    strcpy_safe(task->waiting_for, sizeof(task->waiting_for), name);
    return ERR_WOULD_BLOCK;
}

//如果有任务仍在等待服务，则会提醒您。
void service_dump(void) {
    for (int i = 0; i < NUM_TASKS_MAX; i++) {
        struct task *task = tasks[i];
        if (task && strlen(task->waiting_for) > 0) {
            WARN(
                "%s: stil waiting for a service \"%s\""
                " (hint: add the server to BOOT_SERVERS in Makefile)",
                task->name, task->waiting_for);
        }
    }
}
