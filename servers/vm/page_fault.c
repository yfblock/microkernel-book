#include "page_fault.h"
#include "bootfs.h"
#include "task.h"
#include <libs/common/print.h>
#include <libs/user/syscall.h>

//页面错误处理。准备并映射页面。如果失败，则返回错误。
error_t handle_page_fault(struct task *task, uaddr_t uaddr, uaddr_t ip,
                          unsigned fault) {
    if (uaddr < PAGE_SIZE) {
        //地址0附近的地址无法映射，因此访问该区域需要空指针引用。
//被视为uaddr == 0 的原因是对于指向结构的空指针
//因为当尝试访问成员时，uaddr 将是该成员的偏移量，而不是零。
        WARN("%s (%d): null pointer dereference at vaddr=%p, ip=%p", task->name,
             task->tid, uaddr, ip);
        return ERR_NOT_ALLOWED;
    }

    //与页面边界对齐。
    uaddr_t uaddr_original = uaddr;
    uaddr = ALIGN_DOWN(uaddr, PAGE_SIZE);

    if (fault & PAGE_FAULT_PRESENT) {
        //页面已存在。如果访问权限无效，例如只读页面。
//如果你尝试去写。
        WARN(
            "%s: invalid memory access at %p (IP=%p, reason=%s%s%s, perhaps segfault?)",
            task->name, uaddr_original, ip,
            (fault & PAGE_FAULT_READ) ? "read" : "",
            (fault & PAGE_FAULT_WRITE) ? "write" : "",
            (fault & PAGE_FAULT_EXEC) ? "exec" : "");
        return ERR_NOT_ALLOWED;
    }

    //找到发生页错误的地址上的段。
    elf_phdr_t *phdr = NULL;
    for (unsigned i = 0; i < task->ehdr->e_phnum; i++) {
        if (task->phdrs[i].p_type != PT_LOAD) {
            //除 Pt load 之外未扩展到内存的段将被忽略。
            continue;
        }

        //检查发生缺页的地址是否在该段的范围内。
        uaddr_t start = task->phdrs[i].p_vaddr;
        uaddr_t end = start + task->phdrs[i].p_memsz;
        if (start <= uaddr && uaddr < end) {
            phdr = &task->phdrs[i];
            break;
        }
    }

    //如果没有对应的段，则认为该地址无效。
    if (!phdr) {
        ERROR("unknown memory address (addr=%p, IP=%p), killing %s...",
              uaddr_original, ip, task->name);
        return ERR_INVALID_ARG;
    }

    //准备物理页。
    pfn_t pfn_or_err = sys_pm_alloc(task->tid, PAGE_SIZE, 0);
    if (IS_ERROR(pfn_or_err)) {
        return pfn_or_err;
    }

    //pm alloc 返回的是物理页号，所以将其转换为物理地址。
    paddr_t paddr = PFN2PADDR(pfn_or_err);

    //将片段的内容从 elf 映像复制到分配的物理页。
    size_t offset = uaddr - phdr->p_vaddr;
    if (offset < phdr->p_filesz) {
        //用于复制段内容的虚拟地址空间。这里保护的内存区域是
//并没有实际使用，该区域的虚拟地址被映射到其他物理页。
        static __aligned(PAGE_SIZE) uint8_t tmp_page[PAGE_SIZE];

        //取消映射 Tmp 页面一次。因为它是在启动时由内核映射的。
        ASSERT_OK(sys_vm_unmap(sys_task_self(), (uaddr_t) tmp_page));

        //将 tmp_page 映射到 paddr。这允许您通过虚拟地址使用 tmp_page
//您将能够访问 paddr 的内容。
        ASSERT_OK(sys_vm_map(sys_task_self(), (uaddr_t) tmp_page, paddr,
                             PAGE_READABLE | PAGE_WRITABLE));

        //从 boot fs 读取段内容。
        size_t copy_len = MIN(PAGE_SIZE, phdr->p_filesz - offset);
        bootfs_read(task->file, phdr->p_offset + offset, tmp_page, copy_len);
    }

    //从段信息确定页面属性。
    unsigned attrs = 0;
    attrs |= (phdr->p_flags & PF_R) ? PAGE_READABLE : 0;
    attrs |= (phdr->p_flags & PF_W) ? PAGE_WRITABLE : 0;
    attrs |= (phdr->p_flags & PF_X) ? PAGE_EXECUTABLE : 0;

    //映射页面。
    ASSERT(phdr->p_filesz <= phdr->p_memsz);
    ASSERT_OK(sys_vm_map(task->tid, uaddr, paddr, attrs));
    return OK;
}
