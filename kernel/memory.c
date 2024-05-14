#include "memory.h"
#include "arch.h"
#include "ipc.h"
#include "printk.h"
#include "task.h"
#include <libs/common/string.h>

//物理内存的每个连续区域（区域）的列表。
static list_t zones = LIST_INIT(zones);

//找到物理地址对应的区域。
static struct page *find_page_by_paddr(paddr_t paddr,
                                       enum memory_zone_type *zone_type) {
    DEBUG_ASSERT(IS_ALIGNED(paddr, PAGE_SIZE));

    LIST_FOR_EACH (zone, &zones, struct memory_zone, next) {
        if (zone->base <= paddr
            && paddr < zone->base + zone->num_pages * PAGE_SIZE) {
            size_t start = (paddr - zone->base) / PAGE_SIZE;

            if (zone_type) {
                *zone_type = zone->type;
            }
            return &zone->pages[start];
        }
    }

    return NULL;
}

//添加区域。
static void add_zone(struct memory_zone *zone, enum memory_zone_type type,
                     paddr_t paddr, size_t num_pages) {
    zone->type = type;
    zone->base = paddr;
    zone->num_pages = num_pages;
    for (size_t i = 0; i < num_pages; i++) {
        zone->pages[i].ref_count = 0;
    }

    list_elem_init(&zone->next);
    list_push_back(&zones, &zone->next);
}

//返回从 Start 开始的 num 页物理页是否空闲。
static bool is_contiguously_free(struct memory_zone *zone, size_t start,
                                 size_t num_pages) {
    for (size_t i = 0; i < num_pages; i++) {
        if (zone->pages[start + i].ref_count != 0) {
            return false;
        }
    }
    return true;
}

//在物理页中分配size字节的连续物理内存区域。该地区的所有者
//任务是成为主人。如果指定 NULL，则内核成为所有者。
//
//可以在 flags 中指定以下标志。
//
//-PM_ALLOC_ZEROED：将物理页清零
//-PM_ALLOC_ALIGNED：返回按大小对齐的物理内存地址
paddr_t pm_alloc(size_t size, struct task *owner, unsigned flags) {
    size_t aligned_size = ALIGN_UP(size, PAGE_SIZE);//实际分配的大小
    size_t num_pages = aligned_size / PAGE_SIZE;//要分配的物理页数
    LIST_FOR_EACH (zone, &zones, struct memory_zone, next) {
        if (zone->type != MEMORY_ZONE_FREE) {
            //Mmio区域无法使用
            continue;
        }

        //检查每次开始的numpages物理页是否空闲。
        for (size_t start = 0; start < zone->num_pages; start++) {
            paddr_t paddr = zone->base + start * PAGE_SIZE;
            if ((flags & PM_ALLOC_ALIGNED) != 0
                && !IS_ALIGNED(paddr, aligned_size)) {
                //由于未对准而跳过
                continue;
            }

            if (is_contiguously_free(zone, start, num_pages)) {
                //分配每个空闲的物理页
                for (size_t i = 0; i < num_pages; i++) {
                    struct page *page = &zone->pages[start + i];
                    page->ref_count = 1;
                    page->owner = owner;
                    list_elem_init(&page->next);

                    if (owner) {
                        list_push_back(&owner->pages, &page->next);
                    }
                }

                //必要时清零
                if (flags & PM_ALLOC_ZEROED) {
                    memset((void *) arch_paddr_to_vaddr(paddr), 0,
                           PAGE_SIZE * num_pages);
                }

                return paddr;
            }
        }
    }

    WARN("pm: run out of memory");
    return 0;
}

//免费一页物理页。
static void free_page(struct page *page) {
    DEBUG_ASSERT(page->ref_count > 0);

    //减少引用计数。请注意，在达到 0 之前，它将在其他地方引用。
    page->ref_count--;

    if (page->ref_count == 0) {
        list_remove(&page->next);
    }
}

//设置物理页的所有者。 Owner任务退出时指定物理页的引用计数
//将被减去。我想为一个任务分配一个物理页，但该任务还没有初始化。
//未完成时使用。
void pm_own_page(paddr_t paddr, struct task *owner) {
    struct page *page = find_page_by_paddr(paddr, NULL);

    ASSERT(page != NULL);
    ASSERT(page->owner == NULL);
    ASSERT(page->ref_count == 1);
    ASSERT(!list_is_linked(&page->next));

    page->owner = owner;
    list_push_back(&owner->pages, &page->next);
}

//释放由 Pm alloc 函数分配的连续物理内存区域。
void pm_free(paddr_t paddr, size_t size) {
    DEBUG_ASSERT(IS_ALIGNED(size, PAGE_SIZE));

    //免费每页
    for (size_t offset = 0; offset < size; offset += PAGE_SIZE) {
        //从物理地址获取页管理结构
        struct page *page = find_page_by_paddr(paddr + offset, NULL);
        ASSERT(page != NULL);
        free_page(page);
    }
}

//将列表指定为 Pm free 函数的参数的版本。
void pm_free_by_list(list_t *pages) {
    LIST_FOR_EACH (page, pages, struct page, next) {
        free_page(page);
    }
}

//将页面映射（添加到页表）到指定的物理地址。
error_t vm_map(struct task *task, uaddr_t uaddr, paddr_t paddr,
               unsigned attrs) {
    //从物理地址获取页管理结构。
    enum memory_zone_type zone_type;
    struct page *page = find_page_by_paddr(paddr, &zone_type);
    if (!page) {
        WARN("%s: vm_map: no page for paddr %p", task->name, paddr);
        return ERR_INVALID_PADDR;
    }

    //确定是否可以映射页面，或者换句话说，是否可以授予对其物理页面的访问权限
    switch (zone_type) {
        //公羊面积
        case MEMORY_ZONE_FREE:
            if (page->ref_count == 0) {
                WARN("%s: vm_map: paddr %p is not allocated", task->name,
                     paddr);
                return ERR_INVALID_PADDR;
            }

            //当满足以下条件之一时，可以映射页面：
//
//1) 其页面由任务拥有的任务
//2）页面所属任务的寻呼任务
            if (page->owner != task && page->owner->pager != task) {
                WARN("%s: vm_map: paddr %p is not owned", task->name, paddr);
                return ERR_INVALID_PADDR;
            }
            break;
        //米奥区
        case MEMORY_ZONE_MMIO:
            if (page->ref_count > 0) {
                //已经映射了。多个任务不能映射同一个MMIO区域。
//多个设备驱动程序服务器不应同时操作同一设备。
                WARN("%s: vm_map: device paddr %p is already mapped (owner=%s)",
                     task->name, paddr, page->owner ? page->owner->name : NULL);
                return ERR_INVALID_PADDR;
            }
            break;
    }

    error_t err = arch_vm_map(&task->vm, uaddr, paddr, attrs);
    if (err != OK) {
        return err;
    }

    //对于Mmio区域，将任务注册为所有者。如果是ram区域，则已经使用pm alloc函数注册了。
    if (zone_type == MEMORY_ZONE_MMIO && task) {
        list_push_back(&task->pages, &page->next);
    }

    page->ref_count++;
    return OK;
}

//取消映射（从页表中删除）页面。
error_t vm_unmap(struct task *task, uaddr_t uaddr) {
    if (!arch_is_mappable_uaddr(uaddr)) {
        return ERR_INVALID_ARG;
    }

    error_t err = arch_vm_unmap(&task->vm, uaddr);
    if (err != OK) {
        return err;
    }

    return OK;
}

//页面错误处理程序
void handle_page_fault(vaddr_t vaddr, vaddr_t ip, unsigned fault) {
    //内核中没有发生页面错误
//（复制用户指针内存时设置PAGE_FAULT_USER）
    if ((fault & PAGE_FAULT_USER) == 0) {
        PANIC("page fault in kernel: vaddr=%p, ip=%p, reason=%x", vaddr, ip,
              fault);
    }

    //检查发生缺页的地址是否是可映射地址
//（NULL页和内核区地址无法映射）
    if (!arch_is_mappable_uaddr(vaddr)) {
        WARN("%s: page fault at unmappable vaddr: vaddr=%p, ip=%p",
             CURRENT_TASK->name, vaddr, ip);
        task_exit(EXP_INVALID_UADDR);
    }

    //空闲任务和第一个用户任务不会发生页面错误
    struct task *pager = CURRENT_TASK->pager;
    if (!pager) {
        PANIC("%s: unexpected page fault: vaddr=%p, ip=%p", CURRENT_TASK->name,
              vaddr, ip);
    }

    //向寻呼任务发送缺页处理请求消息并等待回复
    struct message m;
    m.type = PAGE_FAULT_MSG;
    m.page_fault.task = CURRENT_TASK->tid;
    m.page_fault.uaddr = vaddr;
    m.page_fault.ip = ip;
    m.page_fault.fault = fault;
    error_t err = ipc(pager, pager->tid, (__user struct message *) &m,
                      IPC_CALL | IPC_KERNEL);

    //检查寻呼任务的响应消息是否正确
    if (err != OK || m.type != PAGE_FAULT_REPLY_MSG) {
        task_exit(EXP_INVALID_PAGER_REPLY);
    }
}

//初始化内存管理系统
void memory_init(struct bootinfo *bootinfo) {
    struct memory_map *memory_map = &bootinfo->memory_map;
    for (int i = 0; i < memory_map->num_frees; i++) {
        struct memory_map_entry *e = &memory_map->frees[i];

        TRACE("free memory: %p - %p (%dMiB)", e->paddr, e->paddr + e->size,
              e->size / 1024 / 1024);

        struct memory_zone *zone =
            (struct memory_zone *) arch_paddr_to_vaddr(e->paddr);
        size_t num_pages =
            ALIGN_DOWN(e->size, PAGE_SIZE) / (PAGE_SIZE + sizeof(struct page));

        void *end_of_header = &zone->pages[num_pages + 1];
        size_t header_size = ((vaddr_t) end_of_header) - ((vaddr_t) zone);
        paddr_t paddr = e->paddr + ALIGN_UP(header_size, PAGE_SIZE);

        add_zone(zone, MEMORY_ZONE_FREE, paddr, num_pages);
    }

    for (int i = 0; i < memory_map->num_devices; i++) {
        struct memory_map_entry *e = &memory_map->devices[i];
        ASSERT(IS_ALIGNED(e->size, PAGE_SIZE));

        TRACE("MMIO memory: %p - %p (%dKiB)", e->paddr, e->paddr + e->size,
              e->size / 1024);

        size_t num_pages = e->size / PAGE_SIZE;
        paddr_t zone_paddr = pm_alloc(sizeof(struct page) * num_pages, NULL,
                                      PM_ALLOC_UNINITIALIZED);
        ASSERT(zone_paddr != 0);
        struct memory_zone *zone =
            (struct memory_zone *) arch_paddr_to_vaddr(zone_paddr);
        add_zone(zone, MEMORY_ZONE_MMIO, e->paddr, num_pages);
    }
}
