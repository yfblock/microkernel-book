#include "mp.h"
#include "asm.h"
#include <kernel/arch.h>
#include <kernel/printk.h>
#include <kernel/task.h>

static struct cpuvar cpuvars[NUM_CPUS_MAX];
static uint32_t big_lock = BKL_UNLOCKED;
static int locked_cpu = -1;

//通过写入setsip 寄存器(ACLINT) 来发出IPI。
static void write_setssip(uint32_t hartid) {
    full_memory_barrier();
    mmio_write32_paddr(ACLINT_SSWI_SETSSIP(hartid), 1);
}

//检查获取内核锁之前必须满足的条件
static void check_lock(void) {
    DEBUG_ASSERT((read_sstatus() & SSTATUS_SIE) == 0);

    if (big_lock == BKL_HALTED) {
        //其他CPU处于停止状态。让该 CPU 输出紧急消息
//由于强行获取内核锁，进程继续下去就不好了。
        for (;;) {
            asm_wfi();
        }
    }
}

//获取内核锁
void mp_lock(void) {
    check_lock();//检查锁状态（用于调试）

    //继续尝试，直到写入成功
    while (true) {
        if (compare_and_swap(&big_lock, BKL_UNLOCKED, BKL_LOCKED)) {
            break;
        }
    }

    locked_cpu = CPUVAR->id;//记录拥有内核锁的CPU的ID

    //防止在获取上述锁（内存屏障）之前发生此点之后的内存读写。
//如果没有这个，CPU 或编译器就有可能重新排列它们。
    full_memory_barrier();
}

//释放内核锁
void mp_unlock(void) {
    DEBUG_ASSERT(CPUVAR->id == locked_cpu);

    //确保在释放上面的锁（内存屏障）之前执行此点之前的内存读写。
//如果没有这个，CPU 或编译器可能会重新排列它们。
    full_memory_barrier();

    //释放锁
    compare_and_swap(&big_lock, BKL_LOCKED, BKL_UNLOCKED);
}

//强制获取内核锁。当发生内核恐慌等致命错误时
//用于从其他 CPU 窃取锁并停止内核。
void mp_force_lock(void) {
    big_lock = BKL_LOCKED;
    locked_cpu = CPUVAR->id;
    full_memory_barrier();
}

//获取指定cpu的cpu局部变量
struct cpuvar *riscv32_cpuvar_of(int hartid) {
    ASSERT(hartid < NUM_CPUS_MAX);
    return &cpuvars[hartid];
}

//向其他 CPU 发送处理器间中断 (IPI)
void arch_send_ipi(unsigned ipi) {
    //将 ipi 发送到除您自己之外的所有 CPU
    for (int hartid = 0; hartid < NUM_CPUS_MAX; hartid++) {
        struct cpuvar *cpuvar = riscv32_cpuvar_of(hartid);

        //检查CPU是否已完成启动，是不是一样。
        if (cpuvar->online && hartid != CPUVAR->id) {
            //将发送IPI的原因记录在目标CPU的局部变量中（原子|=操作）
            atomic_fetch_and_or(&cpuvar->ipi_pending, ipi);

            //发送IPI
            write_setssip(hartid);
        }
    }

    //等待每个cpu处理完ipi
    for (int hartid = 0; hartid < NUM_CPUS_MAX; hartid++) {
        struct cpuvar *cpuvar = riscv32_cpuvar_of(hartid);
        if (cpuvar->online && hartid != CPUVAR->id) {
            //释放内核锁，允许其他CPU进入内核
            mp_unlock();

            //等待cpu处理ipi
            unsigned pending;
            do {
                pending = atomic_load(&cpuvar->ipi_pending);
            } while (pending != 0);

            //重新获取内核锁
            mp_lock();
        }
    }
}

//每个CPU的初始化过程
void riscv32_mp_init_percpu(void) {
    CPUVAR->online = true;
}

//停止计算机
__noreturn void halt(void) {
    big_lock = BKL_HALTED;
    full_memory_barrier();

    WARN("kernel halted (CPU #%d)", CPUVAR->id);
    for (;;) {
        asm_wfi();
    }
}
