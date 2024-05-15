#include "asm.h"
#include "debug.h"
#include "handler.h"
#include "mp.h"
#include "plic.h"
#include "trap.h"
#include "uart.h"
#include "vm.h"
#include <kernel/arch.h>
#include <kernel/main.h>
#include <kernel/memory.h>
#include <kernel/printk.h>
#include <kernel/task.h>
#include <libs/common/string.h>

//表示第0个cpu已启动的标志
static volatile bool hart0_ready = false;

//第0个CPU的引导处理：从riscv32_setup函数末尾跳转。从这里开始是 S 模式。
__noreturn void riscv32_setup(void) {
    //获取内核锁。从这里您可以操作与其他 CPU 共享的数据。
    mp_lock();

    //初始化 PLIC（中断控制器）。
    riscv32_plic_init_percpu();

    //从这里构造要传递给内核主函数的引导信息。
    struct bootinfo bootinfo;
    bootinfo.boot_elf = (paddr_t) __boot_elf;//vm服务器的ELF镜像

    struct memory_map_entry *frees =
        (struct memory_map_entry *) &bootinfo.memory_map.frees;
    struct memory_map_entry *devices =
        (struct memory_map_entry *) &bootinfo.memory_map.devices;

    vaddr_t ram_start = (vaddr_t) __ram_start;//RAM区域的起始地址
    vaddr_t free_ram_start = (vaddr_t) __free_ram_start;//内核内存之后

    //用户态可以使用的空闲内存区域
    frees[0].paddr = ALIGN_UP((paddr_t) __free_ram_start, PAGE_SIZE);
    frees[0].size = RAM_SIZE - (free_ram_start - ram_start);
    bootinfo.memory_map.num_frees = 1;

    //virtio的MMIO区域-(blk|net)
    devices[0].paddr = VIRTIO_BLK_PADDR;
    devices[0].size = 0x1000;
    devices[1].paddr = VIRTIO_NET_PADDR;
    devices[1].size = 0x1000;
    bootinfo.memory_map.num_devices = 2;

    DEBUG_ASSERT(bootinfo.memory_map.num_devices <= NUM_MEMORY_MAP_ENTRIES_MAX);

    //设置一个值以检测当前使用的堆栈底部的堆栈溢出。
    stack_reset_current_canary();

    //继续内核引导过程。
    kernel_main(&bootinfo);
    UNREACHABLE();
}

//0号以外CPU的引导处理：从riscv32_setup函数末尾跳转。从这里开始是 S 模式。
__noreturn void riscv32_setup_mp(void) {
    //获取内核锁。从这里您可以操作与其他 CPU 共享的数据。
    mp_lock();

    //初始化 PLIC（中断控制器）。
    riscv32_plic_init_percpu();

    //设置一个值以检测当前使用的堆栈底部的堆栈溢出。
    stack_reset_current_canary();

    //继续内核引导过程。
    kernel_mp_main();
    UNREACHABLE();
}

//在 M 模式下执行引导处理。执行只能在 M 模式下执行的初始化处理并转换到 S 模式。
//
//在引导处理期间，所有仅执行一次的初始化处理都在第 0 个 CPU 上执行。对于其他CPU，第0个CPU是
//等待启动过程完成后启动启动过程。
//
//注意：该函数不得访问CPU局部变量以外的数据区域。内核锁
//由于尚未获取，它会与其他CPU竞争并破坏数据。
__noreturn void riscv32_boot(void) {
    int hartid = read_mhartid();//CPU编号
    if (hartid == 0) {
        //将 .bss 部分清除为零。应该首先完成。
        memset(__bss, 0, (vaddr_t) __bss_end - (vaddr_t) __bss);
        //初始化 UART 并使用 <libs/common/print.h> 中定义的宏。
        riscv32_uart_init();
    } else {
        //除第0个CPU之外的所有CPU都等待，直到第0个CPU完成初始化处理。
        while (!hart0_ready)
            ;
    }

    //在s模式下处理所有中断和异常。
    write_medeleg(0xffff);
    write_mideleg(0xffff);

    //设置物理内存访问权限。
    write_pmpaddr0(0xffffffff);
    write_pmpcfg0(0xf);

    //初始化CPU局部变量。
    struct cpuvar *cpuvar = riscv32_cpuvar_of(hartid);
    memset(cpuvar, 0, sizeof(struct cpuvar));
    cpuvar->magic = CPUVAR_MAGIC;
    cpuvar->online = false;//仍在启动
    cpuvar->id = hartid;
    cpuvar->ipi_pending = 0;
    cpuvar->arch.interval = MTIME_PER_1MS;
    cpuvar->arch.mtimecmp = CLINT_MTIMECMP(hartid);
    cpuvar->arch.mtime = CLINT_MTIME;

    //在中断处理程序中使用 CPU 局部变量的寄存器（sscratch、mscratch）和 tp 寄存器
//设置 。从这里开始，您可以使用 CPUVAR 宏访问 CPU 局部变量。
    write_sscratch((uint32_t) cpuvar);
    write_mscratch((uint32_t) cpuvar);
    write_tp((uint32_t) cpuvar);

    //设置一个计时器，但要确保它足够长，因为您还不想被打扰。
    *MTIMECMP = 0xffffffff;

    //配置 S 模式中断处理程序和异常处理程序使用的内核堆栈。
//
//然而，在启动过程完成并且第一个用户任务开始执行之前，异常和中断都会发生。
//它不应该发生。但是，这很可能是由于错误而发生的，因此请使用随机值（0xdeadbeef）
//请进行设置，以便您能够注意到。
    cpuvar->arch.sp_top = 0xdeadbeef;
    write_stvec((uint32_t) riscv32_trap_handler);

    //配置 M 模式中断处理程序和各种设置。顾名思义，只有定时器中断是M模式。
//调用中断处理程序。
    write_mtvec((uint32_t) riscv32_timer_handler);
    write_mstatus(read_mstatus() | MSTATUS_MIE);
    write_mie(read_mie() | MIE_MTIE);

    //使用Mret指令设置目标地址。
    if (hartid == 0) {
        write_mepc((uint32_t) riscv32_setup);
    } else {
        write_mepc((uint32_t) riscv32_setup_mp);
    }

    //用Mret指令返回s模式。
    uint32_t mstatus = read_mstatus();
    mstatus &= ~MSTATUS_MPP_MASK;
    mstatus |= MSTATUS_MPP_S;
    write_mstatus(mstatus);

    //mret指令原本是从中断处理程序中恢复发生时的状态并返回的指令，但是这种机制
//这里，它用于转换到 mepc/mstatus 寄存器中设置的“状态”。
    asm_mret();
    UNREACHABLE();
}

void arch_init(void) {
    riscv32_vm_init();
}

void arch_init_percpu(void) {
    //该任务应该已经初始化
    ASSERT(CURRENT_TASK == IDLE_TASK);

    //由于在第一次上下文切换之前不会发生中断，因此此处设置的值是
//没用过。但是，由于内核错误可能会出现异常，因此以防万一
//放在这里。
    CPUVAR->arch.sp_top = IDLE_TASK->arch.sp_top;

    //使用户页面可以从 S 模式（内核）访问。
    write_sstatus(read_sstatus() | SSTATUS_SUM);

    //设置S模式下接受的中断类型（sie寄存器）。但是，在状态寄存器中
//由于中断仍然被禁用，因此不应调用中断处理程序。
    write_sie(read_sie() | SIE_SEIE | SIE_STIE | SIE_SSIE);

    //将 CPU 标记为已启动。
    riscv32_mp_init_percpu();

    //设置定时器中断。
    uint32_t last_mtime = *MTIME;
    *MTIMECMP = last_mtime + CPUVAR->arch.interval;
    CPUVAR->arch.last_mtime = last_mtime;

    if (CPUVAR->id == 0) {
        hart0_ready = true;
        arch_irq_enable(UART0_IRQ);
    }
}

//主要处理空闲任务。让CPU休息直到中断到来。
void arch_idle(void) {
    //中断处理程序拥有内核锁，因此这里我们释放锁。
    mp_unlock();

    //启用中断并等待。换句话说，CPU会休眠直到中断到来。
    write_sstatus(read_sstatus() | SSTATUS_SIE);
    asm_wfi();

    //中断处理程序完成处理并返回到该函数。
//禁用中断并重新获取内核锁。
    write_sstatus(read_sstatus() & ~SSTATUS_SIE);
    mp_lock();
}

__noreturn void arch_shutdown(void) {
    //禁用寻呼
    write_satp(0);

    //调用名为 SiFive 测试设备的虚构设备并退出模拟器。
//由于分页已禁用，因此可以访问物理地址 0x100000。
    *((volatile uint32_t *) 0x100000) = 0x5555/*正常终止*/;

    PANIC("failed to shutdown");
}

//调用panic函数并在打印panic消息之前调用。
void panic_before_hook(void) {
    //强制获取内核锁并输出恐慌消息。
    mp_force_lock();
}

//在调用panic函数并输出panic消息后调用。
__noreturn void panic_after_hook(void) {
    //关闭你的电脑。但是，我不会将其关闭，这样我就可以排除故障。
    halt();
}
