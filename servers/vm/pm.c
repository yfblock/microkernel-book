#include "pm.h"
#include "task.h"
#include <libs/common/print.h>
#include <libs/user/syscall.h>

//返回任务未使用的虚拟地址空间。虚拟地址仍保持分配状态且无法释放。
static uaddr_t valloc(struct task *task, size_t size) {
    if (task->valloc_next >= VALLOC_END) {
        return 0;
    }

    uaddr_t uaddr = task->valloc_next;
    task->valloc_next += ALIGN_UP(size, PAGE_SIZE);
    return uaddr;
}

//将物理地址映射到任务的页表。分配的虚拟地址返回到uaddr。
error_t map_pages(struct task *task, size_t size, int map_flags, paddr_t paddr,
                  uaddr_t *uaddr) {
    DEBUG_ASSERT(IS_ALIGNED(size, PAGE_SIZE));
    *uaddr = valloc(task, size);
    if (!*uaddr) {
        return ERR_NO_RESOURCES;
    }

    //映射每个页面。
    for (offset_t offset = 0; offset < size; offset += PAGE_SIZE) {
        error_t err =
            sys_vm_map(task->tid, *uaddr + offset, paddr + offset, map_flags);
        if (err != OK) {
            WARN("vm_map failed: %s", err2str(err));
            return err;
        }
    }

    return OK;
}

//分配物理页并将它们映射到任务的页表。分配的虚拟地址返回到uaddr。
error_t alloc_pages(struct task *task, size_t size, int alloc_flags,
                    int map_flags, paddr_t *paddr, uaddr_t *uaddr) {
    pfn_t pfn = sys_pm_alloc(task->tid, size,
                             alloc_flags | PM_ALLOC_ALIGNED | PM_ALLOC_ZEROED);
    if (IS_ERROR(pfn)) {
        return pfn;
    }

    *paddr = PFN2PADDR(pfn);
    return map_pages(task, size, map_flags, *paddr, uaddr);
}
