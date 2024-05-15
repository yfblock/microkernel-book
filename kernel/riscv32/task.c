#include "asm.h"
#include "debug.h"
#include "mp.h"
#include "switch.h"
#include <kernel/arch.h>
#include <kernel/hinavm.h>
#include <kernel/memory.h>
#include <kernel/printk.h>
#include <kernel/task.h>

//第一次上下文切换到用户任务时调用的函数
__noreturn void riscv32_user_entry(uint32_t ip) {
    mp_unlock();//释放内核锁进入用户态
    write_sepc(ip);//设置用户任务的执行起始地址

    //设置 Sret 指令应恢复的状态
    uint32_t sstatus = read_sstatus();
    sstatus &= ~SSTATUS_SPP;  //
    sstatus |= SSTATUS_SPIE;
    write_sstatus(sstatus);

    //进入用户模式之前清除寄存器以避免泄漏内核信息
    __asm__ __volatile__(
        "mv x1, zero\n\t"
        "mv x2, zero\n\t"
        "mv x3, zero\n\t"
        "mv x4, zero\n\t"
        "mv x5, zero\n\t"
        "mv x6, zero\n\t"
        "mv x7, zero\n\t"
        "mv x8, zero\n\t"
        "mv x10, zero\n\t"
        "mv x11, zero\n\t"
        "mv x12, zero\n\t"
        "mv x13, zero\n\t"
        "mv x14, zero\n\t"
        "mv x15, zero\n\t"
        "mv x16, zero\n\t"
        "mv x17, zero\n\t"
        "mv x18, zero\n\t"
        "mv x19, zero\n\t"
        "mv x20, zero\n\t"
        "mv x21, zero\n\t"
        "mv x22, zero\n\t"
        "mv x23, zero\n\t"
        "mv x24, zero\n\t"
        "mv x25, zero\n\t"
        "mv x26, zero\n\t"
        "mv x27, zero\n\t"
        "mv x28, zero\n\t"
        "mv x29, zero\n\t"
        "mv x30, zero\n\t"
        "mv x31, zero\n\t"
        "sret");

    //该函数永远不会返回。每当您从用户任务返回内核模式时
//riscv32_trap_handler 是入口点。
    UNREACHABLE();
}

//切换到下一个任务（下一个）。 prev 指定当前正在运行的任务。
void arch_task_switch(struct task *prev, struct task *next) {
    //切换中断处理程序中使用的内核堆栈。在系统调用处理程序内部
//由于每个任务都可能进入休眠状态，因此需要一个专门的任务
//需要内核堆栈。
    CPUVAR->arch.sp_top = next->arch.sp_top;

    //切换页表并刷新 TLB。在写入 satp 寄存器之前一次
//之所以执行sfence.vma指令是因为在此之前对页表所做的更改是
//以确保完成。
//（RISC-V指令集手册第二卷，版本1.10，第58页）
    asm_sfence_vma();
    write_satp(SATP_MODE_SV32 | next->vm.table >> SATP_PPN_SHIFT);
    asm_sfence_vma();

    //切换寄存器并将执行移至下一个任务（下一个）。此任务（上一个）是
//执行上下文被保存，并且当再次继续时，就像从该函数返回时一样
//表现。
    riscv32_task_switch(&prev->arch.sp, &next->arch.sp);
}

//初始化任务
error_t arch_task_init(struct task *task, uaddr_t ip, vaddr_t kernel_entry,
                       void *arg) {
    //分配内核堆栈。始终使用堆栈金丝雀地址
//指定 PM_ALLOC_ALIGNED 标志，以便可以通过 stack_bottom 函数计算。
//分配堆栈大小的倍数的地址。
    paddr_t sp_bottom = pm_alloc(KERNEL_STACK_SIZE, NULL,
                                 PM_ALLOC_ALIGNED | PM_ALLOC_UNINITIALIZED);
    if (!sp_bottom) {
        return ERR_NO_MEMORY;
    }

    //准备内核堆栈。请注意，堆栈从地址开始向下增长。
    uint32_t sp_top = sp_bottom + KERNEL_STACK_SIZE;
    uint32_t *sp = (uint32_t *) arch_paddr_to_vaddr(sp_top);

    uint32_t entry;
    if (kernel_entry) {
        //Riscv32内核入口trampoline函数中弹出的值
        *--sp = (uint32_t) kernel_entry;//任务执行起始地址
        *--sp = (uint32_t) arg;//传递给 a0 寄存器的值（第一个参数）
        entry = (uint32_t) riscv32_kernel_entry_trampoline;
    } else {
        //Riscv32用户入口trampoline函数中弹出的值
        *--sp = ip;//任务执行起始地址
        entry = (uint32_t) riscv32_user_entry_trampoline;
    }

    //Riscv32任务切换函数中恢复执行上下文
    *--sp = 0;//s11
    *--sp = 0;//s10
    *--sp = 0;//s9
    *--sp = 0;//s8
    *--sp = 0;//s7
    *--sp = 0;//s6
    *--sp = 0;//s5
    *--sp = 0;//s4
    *--sp = 0;//s3
    *--sp = 0;//s2
    *--sp = 0;//s1
    *--sp = 0;//s0
    *--sp = entry;//拉

    //填充任务管理结构
    task->arch.sp = (uint32_t) sp;
    task->arch.sp_bottom = sp_bottom;
    task->arch.sp_top = (uint32_t) sp_top;

    //将堆栈金丝雀设置为内核堆栈
    stack_set_canary(sp_bottom);
    return OK;
}

//放弃任务
void arch_task_destroy(struct task *task) {
    pm_free(task->arch.sp_bottom, KERNEL_STACK_SIZE);
}
