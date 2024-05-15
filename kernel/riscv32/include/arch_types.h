#pragma once
#include "../asmdefs.h"
#include <libs/common/types.h>

//虚拟地址空间中内核内存区域的起始地址。
#define KERNEL_BASE 0x80000000
//最大 Irq 数。
#define IRQ_MAX 32

//RISC V 特定任务管理结构。
struct arch_task {
    uint32_t sp;//下次运行时恢复的内核堆栈值
    uint32_t sp_top;//内核栈顶
    paddr_t sp_bottom;//内核栈底部
};

//RISC v 特定的页表管理结构。
struct arch_vm {
    paddr_t table;//页表的物理地址（Sv32）
};

//Risc v 特定的 cpu 局部变量。更改顺序时，还要更新 asmdefs.h 中定义的宏。
struct arch_cpuvar {
    uint32_t sscratch;//变量的临时存储位置
    uint32_t sp_top;//运行任务的内核栈顶

    //用于定时器中断处理程序（M 模式）。
    uint32_t mscratch0;//变量的临时存储位置
    uint32_t mscratch1;//变量第 2 部分的临时存储位置
    paddr_t mtimecmp;//MTIMECMP 地址
    paddr_t mtime;//MTIME地址
    uint32_t interval;//要添加到 MTIMECMP 的值
    uint64_t last_mtime;//最后的 mtime 值
};

//用于检查 CPUVAR_*宏定义是否正确的宏。
#define ARCH_TYPES_STATIC_ASSERTS                                              \
    STATIC_ASSERT(offsetof(struct cpuvar, arch.sscratch) == CPUVAR_SSCRATCH,   \
                  "CPUVAR_SSCRATCH is incorrect");                             \
    STATIC_ASSERT(offsetof(struct cpuvar, arch.sp_top) == CPUVAR_SP_TOP,       \
                  "CPUVAR_SP_TOP is incorrect");                               \
    STATIC_ASSERT(offsetof(struct cpuvar, arch.mscratch0) == CPUVAR_MSCRATCH0, \
                  "CPUVAR_MSCRATCH0 is incorrect");                            \
    STATIC_ASSERT(offsetof(struct cpuvar, arch.mscratch1) == CPUVAR_MSCRATCH1, \
                  "CPUVAR_MSCRATCH1 is incorrect");                            \
    STATIC_ASSERT(offsetof(struct cpuvar, arch.mtimecmp) == CPUVAR_MTIMECMP,   \
                  "CPUVAR_MTIMECMP is incorrect");                             \
    STATIC_ASSERT(offsetof(struct cpuvar, arch.mtime) == CPUVAR_MTIME,         \
                  "CPUVAR_MTIME is incorrect");                                \
    STATIC_ASSERT(offsetof(struct cpuvar, arch.interval) == CPUVAR_INTERVAL,   \
                  "CPUVAR_INTERVAL is incorrect");

//Cpuvar 宏的内容。返回当前 cpu 局部变量的地址。
static inline struct cpuvar *arch_cpuvar_get(void) {
    //CPU局部变量的地址存储在Tp寄存器中。
    uint32_t tp;
    __asm__ __volatile__("mv %0, tp" : "=r"(tp));
    return (struct cpuvar *) tp;
}

//将物理地址转换为虚拟地址。
static inline vaddr_t arch_paddr_to_vaddr(paddr_t paddr) {
    //0x80000000以上的物理地址映射到同一个虚拟地址，所以
//按原样返回。
    return paddr;
}

//返回虚拟地址是否可以被用户任务使用。
static inline bool arch_is_mappable_uaddr(uaddr_t uaddr) {
    //地址 0 附近不允许，因为空指针引用的可能性很高。另外，在 KERNEL_BASE 之后
//它由内核使用，因此不允许它。
    return PAGE_SIZE <= uaddr && uaddr < KERNEL_BASE;
}
