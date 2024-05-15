#include "trap.h"
#include "asm.h"
#include "debug.h"
#include "mp.h"
#include "plic.h"
#include "uart.h"
#include "usercopy.h"
#include "vm.h"
#include <kernel/interrupt.h>
#include <kernel/memory.h>
#include <kernel/printk.h>
#include <kernel/syscall.h>
#include <kernel/task.h>

//系统调用
static void handle_syscall_trap(struct riscv32_trap_frame *frame) {
    //调用系统调用处理程序并将返回值设置到a0寄存器
    frame->a0 = handle_syscall(frame->a0, frame->a1, frame->a2, frame->a3,
                               frame->a4, frame->a5);

    //更新要恢复的程序计数器的值并更新调用系统调用的指令（ecall指令）。
//返回到下一个命令
    frame->pc += 4;
}

//软件中断：来自riscv32_timer_handler的进程间中断和定时器中断。
static void handle_soft_interrupt_trap(void) {
    write_sip(read_sip() & ~SIP_SSIP);//清除 SSIP 位

    //处理直到没有更多的 ipis 需要处理
    while (true) {
        //获取要处理的 IPI。与 ipi_pending = ipi_pending & 0 相同。
//即获取ipi_pending的值并将ipi_pending设置为0
        unsigned pending = atomic_fetch_and_and(&CPUVAR->ipi_pending, 0);
        if (pending == 0) {
            break;
        }

        //TLB击落
        if (pending & IPI_TLB_FLUSH) {
            asm_sfence_vma();
        }

        if (pending & IPI_RESCHEDULE) {
            task_switch();
        }
    }

    //如果定时器正在运行，则调用定时器中断处理程序
    unsigned now = *MTIME;
    unsigned ticks = MTIME_TO_TICKS(now - CPUVAR->arch.last_mtime);
    CPUVAR->arch.last_mtime = now;
    if (ticks > 0) {
        handle_timer_interrupt(ticks);
    }
}

//硬件中断
static void handle_external_interrupt_trap(void) {
    //获取发生的中断
    unsigned irq = riscv32_plic_pending();
    riscv32_plic_ack(irq);

    if (irq == UART0_IRQ) {
        //串口中断（接收字符）
        handle_serial_interrupt();
    } else {
        //其他中断
        handle_interrupt(irq);
    }
}

//页面错误
static void handle_page_fault_trap(struct riscv32_trap_frame *frame) {
    //获取发生原因
    uint32_t reason;
    switch (read_scause()) {
        case SCAUSE_INST_PAGE_FAULT://执行指令时发生页错误
            reason = PAGE_FAULT_EXEC;
            break;
        case SCAUSE_LOAD_PAGE_FAULT://读取内存时发生页错误
            reason = PAGE_FAULT_READ;
            break;
        case SCAUSE_STORE_PAGE_FAULT://写入内存时发生页面错误
            reason = PAGE_FAULT_WRITE;
            break;
        default:
            UNREACHABLE();
    }

    //获取导致缺页的虚拟地址
    vaddr_t vaddr = read_stval();

    //检查页面是否已经映射。如果已映射，则为只读页面
//违反页面权限，例如尝试写入。如果没有映射，则请求分页
//NULL 指针引用是可能的。
    reason |= riscv32_is_mapped(read_satp(), vaddr) ? PAGE_FAULT_PRESENT : 0;

    if ((frame->sstatus & SSTATUS_SPP) == 0) {
        //发生在用户态
        reason |= PAGE_FAULT_USER;
    }

    //获取发生时的程序计数器
    uint32_t sepc = read_sepc();

    if (sepc == (uint32_t) riscv32_usercopy1
        || sepc == (uint32_t) riscv32_usercopy2) {
        //如果在复制用户指针时发生页面错误，
//被视为在用户模式下发生的事情（PAGE_FAULT_USER）。
//
//在这种情况下我们已经有一个内核锁，所以我们在这里获取锁
//（调用mp_lock函数）不需要。
        reason |= PAGE_FAULT_USER;
        handle_page_fault(vaddr, sepc, reason);
    } else {
        //内核模式中的页面错误被视为致命错误。
        if ((reason & PAGE_FAULT_USER) == 0) {
            PANIC("page fault in kernel: vaddr=%p, sepc=%p, reason=%x", vaddr,
                  sepc, reason);
        }

        //用户模式下的页面错误调用寻呼任务。寻呼机任务图
//请注意，我会阻止你，直到你这样做为止。
        mp_lock();
        handle_page_fault(vaddr, sepc, reason);
        mp_unlock();
    }
}

//中断/异常处理程序：引导过程完成后，该函数是内核模式的入口点。
void riscv32_handle_trap(struct riscv32_trap_frame *frame) {
    stack_check();//检查堆栈溢出

    //
//注意：不要忘记获取和释放内核锁（mp_lock/mp_unlock 函数）
//

    uint32_t scause = read_scause();//获取中断原因
    switch (scause) {
        //系统调用
        case SCAUSE_ENV_CALL:
            mp_lock();
            handle_syscall_trap(frame);
            mp_unlock();
            break;
        //软件中断
        case SCAUSE_S_SOFT_INTR:
            mp_lock();
            handle_soft_interrupt_trap();
            mp_unlock();
            break;
        //外部中断
        case SCAUSE_S_EXT_INTR:
            mp_lock();
            handle_external_interrupt_trap();
            mp_unlock();
            break;
        //页面错误
        case SCAUSE_INST_PAGE_FAULT:
        case SCAUSE_LOAD_PAGE_FAULT:
        case SCAUSE_STORE_PAGE_FAULT:
            handle_page_fault_trap(frame);
            break;
        //运行任务引发的异常
        case SCAUSE_INS_MISS_ALIGN:
        case SCAUSE_INST_ACCESS_FAULT:
        case SCAUSE_ILLEGAL_INST:
        case SCAUSE_BREAKPOINT:
        case SCAUSE_LOAD_ACCESS_FAULT:
        case SCAUSE_AMO_MISS_ALIGN:
        case SCAUSE_STORE_ACCESS_FAULT:
            WARN("%s: invalid exception: scause=%d, stval=%p",
                 CURRENT_TASK->name, read_scause(), read_stval());
            mp_lock();
            task_exit(EXP_ILLEGAL_EXCEPTION);
        default:
            PANIC("unknown trap: scause=%p, stval=%p", read_scause(),
                  read_stval());
    }

    if (frame->sstatus & SSTATUS_SPP) {
        //对于内核态发生的异常（所谓嵌套异常），frame->tp
//异常发生时CPU保存的CPUVAR和当前CPU继续异常处理的CPUVAR为
//有不同的情况。例如，可能有以下情况：
//
//CPU 0：异常处理期间发生另一个异常（例如用户复制期间的页面错误）。
//CPU 0：向寻呼任务发送缺页处理请求，并继续处理其他任务。
//CPU 1：来自寻呼任务的回复，因此该任务因页面错误而停止
//恢复。
//CPU 1：从页面错误处理程序返回，恢复帧内容，并恢复原始异常处理
//（例如系统调用处理程序）继续。然而，frame->tp 位于 CPU 0 上
//由于它指向 CPUVAR，因此 CPU 1 将访问 CPU 0 的 CPU 局部变量。
//我最终参考了它。
//
//因此，要恢复的tp寄存器的内容被重写为执行CPU的内容才有意义。
        frame->tp = (uint32_t) CPUVAR;
    }

    stack_check();//检查堆栈溢出
}
