#include "virtio_net.h"
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/dmabuf.h>
#include <libs/user/driver.h>
#include <libs/user/ipc.h>
#include <libs/user/syscall.h>
#include <libs/user/virtio/virtio_mmio.h>

static task_t tcpip_server;//TCP/IP 服务器任务 ID
static struct virtio_mmio device;//virtio设备管理结构
static struct virtio_virtq *rx_virtq;//接收数据包的虚拟队列
static struct virtio_virtq *tx_virtq;//发送数据包的virtqueue
static dmabuf_t rx_dmabuf;//virtqueue 用于接收数据包的缓冲区
static dmabuf_t tx_dmabuf;//virtqueue发送数据包使用的缓冲区
//读取MAC地址
static void read_macaddr(uint8_t *macaddr) {
    offset_t base = offsetof(struct virtio_net_config, macaddr);
    for (int i = 0; i < 6; i++) {
        macaddr[i] = virtio_read_device_config8(&device, base + i);
    }
}

//发送一个数据包
static error_t transmit(const void *payload, size_t len) {
    if (len > VIRTIO_NET_MAX_PACKET_SIZE) {
        return ERR_TOO_LARGE;
    }

    //分配一个缓冲区来处理请求
    struct virtio_net_req *req;
    paddr_t paddr;
    if ((req = dmabuf_alloc(tx_dmabuf, &paddr)) == NULL) {
        WARN("no free TX buffers");
        return ERR_TRY_AGAIN;
    }

    //创建处理请求
    req->header.flags = 0;
    req->header.gso_type = VIRTIO_NET_HDR_GSO_NONE;
    req->header.gso_size = 0;
    req->header.checksum_start = 0;
    req->header.checksum_offset = 0;
    memcpy((uint8_t *) &req->payload, payload, len);

    //创建描述符链
    struct virtio_chain_entry chain[1];
    chain[0].addr = paddr;
    chain[0].len = sizeof(struct virtio_net_header) + len;
    chain[0].device_writable = false;

    //将描述符链添加到 Virtqueue
    int index_or_err = virtq_push(tx_virtq, chain, 1);
    if (IS_ERROR(index_or_err)) {
        return index_or_err;
    }

    //通知设备
    virtq_notify(&device, tx_virtq);
    return OK;
}

//中断处理程序
static void irq_handler(void) {
    //通知设备已收到中断
    uint8_t status = virtio_read_interrupt_status(&device);
    virtio_ack_interrupt(&device, status);

    //中断原因：设备更新virtqueue
    if (status & VIRTIO_ISR_STATUS_QUEUE) {
        //查看发送的数据包的循环
        struct virtio_chain_entry chain[1];
        size_t total_len;
        while (virtq_pop(tx_virtq, chain, 1, &total_len) > 0) {
            //释放分配用于发送的缓冲区
            dmabuf_free(tx_dmabuf, chain[0].addr);
        }

        //查看接收到的数据包的循环
        while (virtq_pop(rx_virtq, chain, 1, &total_len) > 0) {
            //从描述符的物理地址获取对应的虚拟地址
            struct virtio_net_req *req = dmabuf_p2v(rx_dmabuf, chain[0].addr);

            //发送数据包到 TCP/ip 服务器
            struct message m;
            m.type = NET_RECV_MSG;
            memcpy(m.net_recv.payload, &req->payload, total_len);
            m.net_recv.payload_len = total_len;
            OOPS_OK(ipc_send(tcpip_server, &m));

            //将接收到的内存缓冲区放回到队列中
            virtq_push(rx_virtq, chain, 1);
        }

        //通知设备已重新插入接收队列
        virtq_notify(&device, rx_virtq);
    }
}

//初始化设备
static void init_device(void) {
    //初始化 Virtio 设备。
    ASSERT_OK(virtio_init(&device, VIRTIO_NET_PADDR, 2));

    //启用设备功能。没有特别需要的功能，只需使用设备建议的功能即可。
//启用。
    uint64_t features = virtio_read_device_features(&device);
    ASSERT_OK(virtio_negotiate_feature(&device, features));

    //激活设备。
    ASSERT_OK(virtio_enable(&device));

    //获取 Virtqueue 的指针。
    rx_virtq = virtq_get(&device, 0);
    tx_virtq = virtq_get(&device, 1);

    //分配 DMA 缓冲区来处理请求。
    tx_dmabuf = dmabuf_create(sizeof(struct virtio_net_req), NUM_TX_BUFFERS);
    rx_dmabuf = dmabuf_create(sizeof(struct virtio_net_req), NUM_RX_BUFFERS);
    ASSERT(tx_dmabuf != NULL);
    ASSERT(rx_dmabuf != NULL);

    //用接收内存缓冲区填充接收虚拟队列。
    for (int i = 0; i < NUM_RX_BUFFERS; i++) {
        paddr_t paddr;
        if (!dmabuf_alloc(rx_dmabuf, &paddr)) {
            PANIC("failed to allocate a RX buffer");
        }

        struct virtio_chain_entry chain[1];
        chain[0].addr = paddr;
        chain[0].len = sizeof(struct virtio_net_req);
        chain[0].device_writable = true;
        int desc_index = virtq_push(rx_virtq, chain, 1);
        ASSERT_OK(desc_index);
    }

    //配置接收中断。
    ASSERT_OK(sys_irq_listen(VIRTIO_NET_IRQ));
}

void main(void) {
    //初始化设备
    init_device();

    //读取设备mac地址
    uint8_t macaddr[6];
    read_macaddr(macaddr);
    INFO("MAC address = %02x:%02x:%02x:%02x:%02x:%02x", macaddr[0], macaddr[1],
         macaddr[2], macaddr[3], macaddr[4], macaddr[5]);

    //注册为网络设备
    ASSERT_OK(ipc_register("net_device"));
    TRACE("ready");

    //主循环
    while (true) {
        struct message m;
        ASSERT_OK(ipc_recv(IPC_ANY, &m));
        switch (m.type) {
            //中断处理
            case NOTIFY_IRQ_MSG:
                irq_handler();
                break;
            //打开网络设备
            case NET_OPEN_MSG: {
                tcpip_server = m.src;//接收数据包的目的地
                m.type = NET_OPEN_REPLY_MSG;
                memcpy(m.net_open_reply.macaddr, macaddr,
                       sizeof(m.net_open_reply.macaddr));
                ipc_reply(m.src, &m);
                break;
            }
            //发送数据包
            case NET_SEND_MSG: {
                OOPS_OK(transmit(m.net_send.payload, m.net_send.payload_len));
                break;
            }
            default:
                WARN("unhandled message: %s (%x)", msgtype2str(m.type), m.type);
                break;
        }
    }
}
