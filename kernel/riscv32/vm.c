#include "vm.h"
#include "asm.h"
#include "mp.h"
#include "plic.h"
#include "uart.h"
#include <kernel/arch.h>
#include <kernel/memory.h>
#include <kernel/printk.h>
#include <libs/common/string.h>

//内核内存区域映射到的页表。启动时生成，这个
//页表的内容被复制。
static struct arch_vm kernel_vm;

//将 PAGE_*宏指定的页面属性转换为 Sv32 的页面属性。
static pte_t page_attrs_to_pte_flags(unsigned attrs) {
    return ((attrs & PAGE_READABLE) ? PTE_R : 0)
           | ((attrs & PAGE_WRITABLE) ? PTE_W : 0)
           | ((attrs & PAGE_EXECUTABLE) ? PTE_X : 0)
           | ((attrs & PAGE_USER) ? PTE_U : 0);
}

//构建页表条目。
static pte_t construct_pte(paddr_t paddr, pte_t flags) {
    DEBUG_ASSERT((paddr & ~PTE_PADDR_MASK) == 0);
    return ((paddr >> 12) << 10) | flags;
}

//从页表中获取页表项。参数base是页表的物理地址，
//vaddr是要搜索的虚拟地址，如果alloc为true，则在未设置页表时将使用它。
//分配一个新的。
//
//如果成功，则返回 pte 参数中页表条目的地址。
static error_t walk(paddr_t base, vaddr_t vaddr, bool alloc, pte_t **pte) {
    ASSERT(IS_ALIGNED(vaddr, PAGE_SIZE));

    pte_t *l1table = (pte_t *) arch_paddr_to_vaddr(base);//第一个表
    int index = PTE_INDEX(1, vaddr);//第一行索引
    if (l1table[index] == 0) {
        //第二个表没有设置。
        if (!alloc) {
            return ERR_NOT_FOUND;
        }

        //分配第二个表
        paddr_t paddr = pm_alloc(PAGE_SIZE, NULL, PM_ALLOC_ZEROED);
        if (!paddr) {
            return ERR_NO_MEMORY;
        }

        l1table[index] = construct_pte(paddr, PTE_V);//在第一个表中注册
    }

    //第二层表
    pte_t *l2table = (pte_t *) arch_paddr_to_vaddr(PTE_PADDR(l1table[index]));
    //指向 Vaddr 中页表条目的指针
    *pte = &l2table[PTE_INDEX(0, vaddr)];
    return OK;
}

//映射页面。
error_t arch_vm_map(struct arch_vm *vm, vaddr_t vaddr, paddr_t paddr,
                    unsigned attrs) {
    DEBUG_ASSERT(IS_ALIGNED(vaddr, PAGE_SIZE));
    DEBUG_ASSERT(IS_ALIGNED(paddr, PAGE_SIZE));

    //查找页表条目
    pte_t *pte;
    error_t err = walk(vm->table, vaddr, true, &pte);
    if (err != OK) {
        return err;
    }

    //如果页面已映射则中止
    DEBUG_ASSERT(pte != NULL);
    if (*pte & PTE_V) {
        return ERR_ALREADY_EXISTS;
    }

    //设置页表条目
    *pte = construct_pte(paddr, page_attrs_to_pte_flags(attrs) | PTE_V);

    //清除 Tlb
    asm_sfence_vma();

    //通知其他CPU清除TLB（TLB shotdown）
    arch_send_ipi(IPI_TLB_FLUSH);
    return OK;
}

//取消页面映射。
error_t arch_vm_unmap(struct arch_vm *vm, vaddr_t vaddr) {
    //查找页表条目
    pte_t *pte;
    error_t err = walk(vm->table, vaddr, false, &pte);
    if (err != OK) {
        return err;
    }

    //如果页面未映射则中止
    if (!pte || (*pte & PTE_V) == 0) {
        return ERR_NOT_FOUND;
    }

    //释放页面
    paddr_t paddr = PTE_PADDR(*pte);
    *pte = 0;
    pm_free(paddr, PAGE_SIZE);

    //清除 Tlb
    asm_sfence_vma();

    //通知其他CPU清除TLB（TLB shotdown）
    arch_send_ipi(IPI_TLB_FLUSH);
    return OK;
}

//返回虚拟地址是否映射到页表。
bool riscv32_is_mapped(uint32_t satp, vaddr_t vaddr) {
    satp = (satp & SATP_PPN_MASK) << SATP_PPN_SHIFT;
    uint32_t *pte;
    error_t err = walk(satp, ALIGN_DOWN(vaddr, PAGE_SIZE), false, &pte);
    return err == OK && pte != NULL && (*pte & PTE_V);
}

//初始化页表。
error_t arch_vm_init(struct arch_vm *vm) {
    //分配页表（第一行）
    vm->table = pm_alloc(PAGE_SIZE, NULL, PM_ALLOC_ZEROED);
    if (!vm->table) {
        return ERR_NO_MEMORY;
    }

    //复制内核空间映射
    memcpy((void *) arch_paddr_to_vaddr(vm->table),
           (void *) arch_paddr_to_vaddr(kernel_vm.table), PAGE_SIZE);
    return OK;
}

//丢弃页表。
void arch_vm_destroy(struct arch_vm *vm) {
    //遍历虚拟地址以释放用户空间页面
    uint32_t *l1table = (uint32_t *) arch_paddr_to_vaddr(vm->table);
    for (int i = 0; i < 512; i++) {
        uint32_t pte1 = l1table[i];
        //如果未设置条目则跳过
        if (!(pte1 & PTE_V)) {
            continue;
        }

        //第二层表
        uint32_t *l2table = (uint32_t *) arch_paddr_to_vaddr(PTE_PADDR(pte1));
        for (int j = 0; j < 512; j++) {
            uint32_t pte2 = l2table[j];

            //如果不是用户空间页面则跳过
            if ((pte2 & (PTE_V | PTE_U)) != (PTE_V | PTE_U)) {
                continue;
            }

            //释放页面
            paddr_t paddr = PTE_PADDR(pte2);
            pm_free(paddr, PAGE_SIZE);
        }
    }

    //释放存储第一页表的物理页
    pm_free(vm->table, PAGE_SIZE);
}

//绘制一个连续区域的地图。
static error_t map_pages(struct arch_vm *vm, vaddr_t vaddr, paddr_t paddr,
                         size_t size, unsigned attrs) {
    //将每一页逐一映射
    for (offset_t offset = 0; offset < size; offset += PAGE_SIZE) {
        error_t err = arch_vm_map(vm, vaddr + offset, paddr + offset, attrs);
        if (err != OK) {
            return err;
        }
    }

    return OK;
}

//初始化分页管理机制。
void riscv32_vm_init(void) {
    //为内核内存分配页表
    kernel_vm.table = pm_alloc(PAGE_SIZE, NULL, PM_ALLOC_ZEROED);
    ASSERT(kernel_vm.table != 0);

    //获取链接描述文件中定义的每个地址
    vaddr_t kernel_text = (vaddr_t) __text;
    vaddr_t kernel_text_end = (vaddr_t) __text_end;
    vaddr_t kernel_data = (vaddr_t) __data;
    vaddr_t kernel_data_end = (vaddr_t) __data_end;
    vaddr_t ram_start = (vaddr_t) __ram_start;
    vaddr_t free_ram_start = (vaddr_t) __free_ram_start;

    DEBUG_ASSERT(IS_ALIGNED(kernel_text, PAGE_SIZE));
    DEBUG_ASSERT(IS_ALIGNED(kernel_text_end, PAGE_SIZE));

    //可用物理内存的大小（不包括内核内存的静态区域）
    paddr_t free_ram_size = RAM_SIZE - (free_ram_start - ram_start);
    //内核代码和数据区域大小
    size_t kernel_text_size = kernel_text_end - kernel_text;
    size_t kernel_data_size = kernel_data_end - kernel_data;

    //内核代码区
    ASSERT_OK(map_pages(&kernel_vm, kernel_text, kernel_text, kernel_text_size,
                        PAGE_WRITABLE | PAGE_READABLE | PAGE_EXECUTABLE));
    //内核数据区
    ASSERT_OK(map_pages(&kernel_vm, kernel_data, kernel_data, kernel_data_size,
                        PAGE_READABLE | PAGE_WRITABLE));
    //映射整个物理内存区域以供内核访问
    ASSERT_OK(map_pages(&kernel_vm, free_ram_start, free_ram_start,
                        free_ram_size, PAGE_READABLE | PAGE_WRITABLE));
    //串口
    ASSERT_OK(map_pages(&kernel_vm, UART_ADDR, UART_ADDR, PAGE_SIZE,
                        PAGE_READABLE | PAGE_WRITABLE));
    //普利克
    ASSERT_OK(map_pages(&kernel_vm, PLIC_ADDR, PLIC_ADDR, PLIC_SIZE,
                        PAGE_READABLE | PAGE_WRITABLE));
    //克林特
    ASSERT_OK(map_pages(&kernel_vm, CLINT_PADDR, CLINT_PADDR, CLINT_SIZE,
                        PAGE_READABLE | PAGE_WRITABLE));
    //阿克林特
    ASSERT_OK(map_pages(&kernel_vm, ACLINT_SSWI_PADDR, ACLINT_SSWI_PADDR,
                        PAGE_SIZE, PAGE_READABLE | PAGE_WRITABLE));
}
