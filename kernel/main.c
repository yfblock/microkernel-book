#include "main.h"
#include "arch.h"
#include "memory.h"
#include "printk.h"
#include "task.h"
#include <libs/common/elf.h>
#include <libs/common/string.h>

//创建第一个用户任务（VM服务器）
static void create_first_task(struct bootinfo *bootinfo) {
    //获取Elf header指针并检查幻数
    elf_ehdr_t *header = (elf_ehdr_t *) arch_paddr_to_vaddr(bootinfo->boot_elf);
    if (memcmp(header->e_ident, ELF_MAGIC, 4) != 0) {
        PANIC("bootelf: invalid ELF magic\n");
    }

    //创建第一个任务（VM服务器）
    task_t tid = task_create("vm", header->e_entry, NULL);
    ASSERT_OK(tid);
    struct task *task = task_find(tid);

    //加载程序头
    elf_phdr_t *phdrs = (elf_phdr_t *) ((vaddr_t) header + header->e_phoff);
    for (uint16_t i = 0; i < header->e_phnum; i++) {
        elf_phdr_t *phdr = &phdrs[i];

        //忽略非PT_LOAD段（不需要映射的段）
        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        ASSERT(phdr->p_memsz >= phdr->p_filesz);

        //输出段信息以供调试
        char r = (phdr->p_flags & PF_R) ? 'r' : '-';
        char w = (phdr->p_flags & PF_W) ? 'w' : '-';
        char x = (phdr->p_flags & PF_X) ? 'x' : '-';
        size_t size_in_kb = phdr->p_memsz / 1024;
        TRACE("bootelf: %p - %p %c%c%c (%d KiB)", phdr->p_vaddr,
              phdr->p_vaddr + phdr->p_memsz, r, w, x, size_in_kb);

        //分配一个物理内存区域来映射该段
        paddr_t paddr = pm_alloc(phdr->p_memsz, task, PM_ALLOC_ZEROED);
        ASSERT(paddr != 0);

        //将段内容复制到保留内存区域
        memcpy((void *) arch_paddr_to_vaddr(paddr),
               (void *) ((vaddr_t) header + phdr->p_offset), phdr->p_filesz);

        //检索段所需的页面访问权限
        unsigned attrs = PAGE_USER;
        attrs |= (phdr->p_flags & PF_R) ? PAGE_READABLE : 0;
        attrs |= (phdr->p_flags & PF_W) ? PAGE_WRITABLE : 0;
        attrs |= (phdr->p_flags & PF_X) ? PAGE_EXECUTABLE : 0;

        //映射每个页面的任务
        size_t memsz = ALIGN_UP(phdr->p_memsz, PAGE_SIZE);
        for (offset_t offset = 0; offset < memsz; offset += PAGE_SIZE) {
            error_t err =
                vm_map(task, phdr->p_vaddr + offset, paddr + offset, attrs);
            if (err != OK) {
                PANIC("bootelf: failed to map %p - %p", phdr->p_vaddr,
                      phdr->p_vaddr + phdr->p_memsz);
            }
        }
    }
}

//空闲任务：使 CPU 处于可中断状态，直到另一个任务准备好运行为止。
__noreturn static void idle_task(void) {
    for (;;) {
        task_switch();
        arch_idle();
    }
}

//启动第0个CPU：初始化内核和第一个任务（VM服务器）后，空闲任务
//运作为
void kernel_main(struct bootinfo *bootinfo) {
    printf("Booting HinaOS...\n");
    memory_init(bootinfo);
    arch_init();
    task_init_percpu();
    create_first_task(bootinfo);
    arch_init_percpu();
    TRACE("CPU #%d is ready", CPUVAR->id);

    //从这里开始，它作为一个空闲任务运行
    idle_task();
}

//第 0 个 CPU 以外的 CPU 的引导处理：初始化每个 CPU 后作为空闲任务运行。
void kernel_mp_main(void) {
    task_init_percpu();
    arch_init_percpu();
    TRACE("CPU #%d is ready", CPUVAR->id);

    //从这里开始，它作为一个空闲任务运行
    idle_task();
}
