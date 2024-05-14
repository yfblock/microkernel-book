#include "virtio_blk.h"
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/dmabuf.h>
#include <libs/user/driver.h>
#include <libs/user/ipc.h>
#include <libs/user/syscall.h>
#include <libs/user/virtio/virtio_mmio.h>

static struct virtio_mmio device;       // virtio设备管理结构
static struct virtio_virtq *requestq;   // 用于读/写处理请求的virtqueue
static dmabuf_t dmabuf;                 // virtqueue 用于读/写请求的缓冲区
// 读写磁盘
static error_t read_write(task_t task, uint64_t sector, void *buf, size_t len,
                          bool is_write) {
    // 读取的字节数必须与扇区大小对齐
    if (!IS_ALIGNED(len, SECTOR_SIZE)) {
        return ERR_INVALID_ARG;
    }

    // 不太大
    if (len > REQUEST_BUFFER_SIZE) {
        return ERR_TOO_LARGE;
    }

    // 分配一个缓冲区来处理请求
    struct virtio_blk_req *req;
    paddr_t paddr;
    if ((req = dmabuf_alloc(dmabuf, &paddr)) == NULL) {
        WARN("no free TX buffers");
        return ERR_TRY_AGAIN;
    }

    // 填写处理请求
    req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;
    if (is_write) {
        memcpy(req->data, buf, len);
    }

    // 描述符链[0]：类型、保留、扇区（从设备只读）
    struct virtio_chain_entry chain[3];
    chain[0].addr = paddr;
    chain[0].len = sizeof(uint32_t) * 2 + sizeof(uint64_t);
    chain[0].device_writable = false;
    // 描述符链[1]：写入源/读取目标缓冲区
    chain[1].addr = paddr + offsetof(struct virtio_blk_req, data);
    chain[1].len = len;
    chain[1].device_writable = !is_write;
    // 描述符链[2]：处理结果的内存区域。设备写入。
    chain[2].addr = paddr + offsetof(struct virtio_blk_req, status);
    chain[2].len = sizeof(uint8_t);
    chain[2].device_writable = true;

    // 将描述符链添加到 Virtqueue
    int index_or_err = virtq_push(requestq, chain, 3);
    if (IS_ERROR(index_or_err)) {
        return index_or_err;
    }

    // 通知 Virtio blk
    virtq_notify(&device, requestq);

    // 忙等待处理完成
    while (virtq_is_empty(requestq))
        ;

    // 从 Virtqueue 中提取处理后的描述符链
    size_t total_len;
    int n = virtq_pop(requestq, chain, 3, &total_len);
    if (IS_ERROR(n)) {
        WARN("virtq_pop returned an error: %s", err2str(n));
        dmabuf_free(dmabuf, paddr);
        return ERR_UNEXPECTED;
    }

    // 检查描述符链的内容。用于此处发送的描述符链。
    // 您应该已收到回复。
    ASSERT(n == 3);
    ASSERT(chain[0].desc_index == index_or_err);
    ASSERT(chain[1].len == len);
    ASSERT(req->status == VIRTIO_BLK_S_OK);

    // 进行读处理，将读到的数据复制到内存缓冲区
    if (!is_write) {
        memcpy(buf, req->data, len);
    }

    dmabuf_free(dmabuf, paddr);
    return OK;
}

// 初始化 Virtio blk 设备
static void init_device(void) {
    // 初始化 Virtio 设备
    ASSERT_OK(virtio_init(&device, VIRTIO_BLK_PADDR, 1));

    // 启用设备功能。没有特别需要的功能，只需使用设备建议的功能即可。
    // 启用。
    uint64_t features = virtio_read_device_features(&device);
    ASSERT_OK(virtio_negotiate_feature(&device, features));

    // 激活设备。
    ASSERT_OK(virtio_enable(&device));

    // 获取 Virtqueue 的指针。
    requestq = virtq_get(&device, 0);

    // 不使用中断，因此禁用它们。相反，它使用忙等待来等待进程完成。
    requestq->avail->flags |= VIRTQ_AVAIL_F_NO_INTERRUPT;

    // 创建 DMA 缓冲区来处理请求。
    dmabuf = dmabuf_create(sizeof(struct virtio_blk_req), NUM_REQUEST_BUFFERS);
}

void main(void) {
    // 初始化 Virtio blk 设备
    init_device();

    // 注册为块设备
    ASSERT_OK(ipc_register("blk_device"));
    TRACE("ready");

    while (true) {
        struct message m;
        ASSERT_OK(ipc_recv(IPC_ANY, &m));
        switch (m.type) {
            case BLK_READ_MSG: {
                uint8_t buf[SECTOR_SIZE];
                size_t len = m.blk_read.len;
                error_t err =
                    read_write(m.src, m.blk_read.sector, buf, len, false);
                if (err != OK) {
                    ipc_reply_err(m.src, err);
                    break;
                }

                m.type = BLK_READ_REPLY_MSG;
                m.blk_read_reply.data_len = m.blk_read.len;
                memcpy(m.blk_read_reply.data, buf, len);
                ipc_reply(m.src, &m);
                break;
            }
            case BLK_WRITE_MSG: {
                error_t err =
                    read_write(m.src, m.blk_write.sector, m.blk_write.data,
                               m.blk_write.data_len, true);
                if (err != OK) {
                    ipc_reply_err(m.src, err);
                    break;
                }

                m.type = BLK_WRITE_REPLY_MSG;
                ipc_reply(m.src, &m);
                break;
            }
            default:
                WARN("unhandled message: %d", m.type);
                break;
        }
    }
}
