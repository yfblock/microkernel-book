#pragma once
#include <libs/common/list.h>
#include <libs/common/message.h>
#include <libs/common/types.h>

#define VIRTIO_BLK_T_IN  0  // 从磁盘读取
#define VIRTIO_BLK_T_OUT 1  // 写入磁盘

#define VIRTIO_BLK_S_OK 0   // 处理成功
// 用于读/写处理请求的 DMA 缓冲区数量。由于是顺序处理的，一个就够了。
#define NUM_REQUEST_BUFFERS 1

// 扇区大小（以字节为单位）。磁盘读/写的最小单位。
#define SECTOR_SIZE 512

// 一次可以读取或写入的最大字节数。必须与扇区大小对齐。
#define REQUEST_BUFFER_SIZE SECTOR_SIZE

STATIC_ASSERT(IS_ALIGNED(REQUEST_BUFFER_SIZE, SECTOR_SIZE),
              "virtio-blk buffer size must be aligned to the sector size");

// 对 Virtio blk 的读写请求
struct virtio_blk_req {
    uint32_t type;                      // VIRTIO_BLK_T_IN 或 VIRTIO_BLK_T_OUT
    uint32_t reserved;                  // 预订的
    uint64_t sector;                    // 读/写的扇区号
    uint8_t data[REQUEST_BUFFER_SIZE];  // 要读取和写入的数据
    uint8_t status;                     // 处理结果。 VIRTIO_BLK_S_OK 如果成功。
} __packed;
