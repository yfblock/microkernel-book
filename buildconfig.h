#pragma once

#define RAM_SIZE          (128 * 1024 * 1024)   // 内存大小（使用 QEMU 的 -m 选项指定）
#define NUM_TASKS_MAX     16                    // 最大任务数
#define NUM_CPUS_MAX      4                     // 最大CPU数量
#define TASK_NAME_LEN     16                    // 任务名称最大长度（包括空字符）
#define KERNEL_STACK_SIZE (16 * 1024)           // 内核堆栈大小
#define VIRTIO_BLK_PADDR  0x10001000            // virtio-blk的MMIO地址
#define VIRTIO_NET_PADDR  0x10002000            // virtio-net MMIO 地址
#define VIRTIO_NET_IRQ    2                     // virtio-net中断号
#define TICK_HZ           1000                  // 定时器中断周期
