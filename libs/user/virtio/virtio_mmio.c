#include <libs/common/endian.h>
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/driver.h>
#include <libs/user/malloc.h>
#include <libs/user/mmio.h>
#include <libs/user/syscall.h>
#include <libs/user/virtio/virtio_mmio.h>

//读取设备状态寄存器
static uint32_t read_device_status(struct virtio_mmio *dev) {
    return mmio_read32le(dev->base + VIRTIO_REG_DEVICE_STATUS);
}

//写入设备状态寄存器
static void write_device_status(struct virtio_mmio *dev, uint8_t value) {
    mmio_write32le(dev->base + VIRTIO_REG_DEVICE_STATUS, value);
}

//初始化虚拟队列
static void virtq_init(struct virtio_mmio *dev, unsigned index) {
    //选择虚拟队列
    mmio_write32le(dev->base + VIRTIO_REG_QUEUE_SEL, index);

    //获取 Virtqueue 描述符的数量
    uint32_t num_descs_max =
        mmio_read32le(dev->base + VIRTIO_REG_QUEUE_NUM_MAX);
    ASSERT(num_descs_max > 0);

    //设置Virtqueue描述符的数量
    uint32_t num_descs = MIN(num_descs_max, 512);
    mmio_write32le(dev->base + VIRTIO_REG_QUEUE_NUM, num_descs);

    //计算Virtqueue上每个区域的偏移量
    offset_t avail_ring_off = sizeof(struct virtq_desc) * num_descs;
    size_t avail_ring_size = sizeof(uint16_t) * (3 + num_descs);
    offset_t used_ring_off =
        ALIGN_UP(avail_ring_off + avail_ring_size, PAGE_SIZE);
    size_t used_ring_size =
        sizeof(uint16_t) * 3 + sizeof(struct virtq_used_elem) * num_descs;

    //计算所需物理内存空间大小
    size_t virtq_size = used_ring_off + ALIGN_UP(used_ring_size, PAGE_SIZE);

    //为Virtqueue分配物理内存区域
    uaddr_t virtq_uaddr;
    paddr_t virtq_paddr;
    ASSERT_OK(driver_alloc_pages(virtq_size, PAGE_READABLE | PAGE_WRITABLE,
                                 &virtq_uaddr, &virtq_paddr));

    //初始化Virtqueue的管理结构
    struct virtio_virtq *vq = &dev->virtqs[index];
    vq->index = index;
    vq->num_descs = num_descs;
    vq->last_used_index = 0;
    vq->descs = (struct virtq_desc *) virtq_uaddr;
    vq->avail = (struct virtq_avail *) (virtq_uaddr + avail_ring_off);
    vq->used = (struct virtq_used *) (virtq_uaddr + used_ring_off);

    //通过连接所有描述符创建一个空闲列表
    vq->free_head = 0;
    vq->num_free_descs = num_descs;
    for (size_t i = 0; i < num_descs; i++) {
        vq->descs[i].next = (i + 1 == num_descs) ? 0 : i + 1;
    }

    //将Virtqueue物理内存区域设置为设备
    mmio_write32le(dev->base + VIRTIO_REG_QUEUE_ALIGN, 0);
    mmio_write32le(dev->base + VIRTIO_REG_QUEUE_PFN, virtq_paddr);

    //启用Virtqueue
    mmio_write32le(dev->base + VIRTIO_REG_QUEUE_READY, 1);
}

//获取第Index个virtqueue
struct virtio_virtq *virtq_get(struct virtio_mmio *dev, unsigned index) {
    DEBUG_ASSERT(index < dev->num_queues);
    return &dev->virtqs[index];
}

//获取 Virtqueue 描述符的数量
uint32_t virtq_num_descs(struct virtio_virtq *vq) {
    return vq->num_descs;
}

//通知设备描述符已添加到virtqueue
void virtq_notify(struct virtio_mmio *dev, struct virtio_virtq *vq) {
    //确保写入内存（例如描述符）已完成，并且写入操作从设备端可见。
    full_memory_barrier();

    mmio_write32le(dev->base + VIRTIO_REG_QUEUE_NOTIFY, vq->index);
}

//将描述符链添加到可用环中，如果成功，则添加第一个描述符的描述符
//返回表上的索引。此外，参数链中每个描述符条目的 desc_index
//索引也会写入该字段。
//
//注意：必须调用virtq_notify函数来处理添加的描述符链。
int virtq_push(struct virtio_virtq *vq, struct virtio_chain_entry *chain,
               int n) {
    DEBUG_ASSERT(n > 0);
    if (n > vq->num_free_descs) {
        //没有足够的可用描述符。使用环中已处理的描述符链
//返回空闲列表。
        while (vq->last_used_index != vq->used->index) {
            struct virtq_used_elem *used_elem =
                &vq->used->ring[vq->last_used_index % vq->num_descs];

            //将描述符链的每个描述符（desc）返回到空闲列表
            int num_freed = 0;
            int next_desc_index = used_elem->id;
            while (true) {
                struct virtq_desc *desc = &vq->descs[next_desc_index];
                num_freed++;

                if ((desc->flags & VIRTQ_DESC_F_NEXT) == 0) {
                    //链中的最后一个描述符
                    break;
                }

                next_desc_index = desc->next;
            }

            //添加到空闲列表顶部
            vq->free_head = used_elem->id;
            vq->num_free_descs += num_freed;
            vq->last_used_index++;
        }
    }

    //返回错误，因为仍然没有足够的可用描述符
    if (n > vq->num_free_descs) {
        return ERR_NO_MEMORY;
    }

    //从空闲列表中取出n个描述符并构造描述符
    int head_index = vq->free_head;
    int desc_index = head_index;
    struct virtq_desc *desc = NULL;
    for (int i = 0; i < n; i++) {
        struct virtio_chain_entry *e = &chain[i];
        e->desc_index = desc_index;
        desc = &vq->descs[desc_index];
        desc->addr = into_le64(e->addr);
        desc->len = into_le32(e->len);

        if (i + 1 < n) {
            //描述符位于链的中间
            desc->flags = VIRTQ_DESC_F_NEXT;//有以下描述符
        } else {
            //链中的最后一个描述符
            vq->free_head = desc->next;//释放下一个描述符
            desc->flags = 0;//没有下一个描述符
            desc->next = 0;//不使用
        }

        if (e->device_writable) {
            desc->flags |= VIRTQ_DESC_F_WRITE;//只从设备写入
        }

        desc_index = desc->next;//下一个空闲描述符的索引
        vq->num_free_descs--;
    }

    //将描述符链的第一个描述符的索引添加到Available环中
    vq->avail->ring[vq->avail->index % vq->num_descs] = head_index;
    full_memory_barrier();
    //更新Available环的索引
    vq->avail->index++;
    return head_index;
}

//返回设备驱动程序应处理的已用环中是否存在描述符链
bool virtq_is_empty(struct virtio_virtq *vq) {
    return vq->last_used_index == mmio_read16le((uaddr_t) &vq->used->index);
}

//检索设备处理的描述符链。返回链中包含的描述符的数量，
//将提取的描述符存储在“chain”中。
//
//如果没有已处理的描述符链，则返回 ERR_EMPTY。
int virtq_pop(struct virtio_virtq *vq, struct virtio_chain_entry *chain, int n,
              size_t *total_len) {
    if (virtq_is_empty(vq)) {
        return ERR_EMPTY;
    }

    //提取Used环的第一个描述符链
    struct virtq_used_elem *used_elem =
        &vq->used->ring[vq->last_used_index % vq->num_descs];

    //处理描述符链中的每个描述符
    int next_desc_index = used_elem->id;//链的起始描述符
    struct virtq_desc *desc = NULL;
    int num_popped = 0;
    while (num_popped < n) {
        desc = &vq->descs[next_desc_index];
        chain[num_popped].desc_index = next_desc_index;
        chain[num_popped].addr = desc->addr;
        chain[num_popped].len = desc->len;
        chain[num_popped].device_writable =
            (desc->flags & VIRTQ_DESC_F_WRITE) != 0;
        num_popped++;

        bool has_next = (desc->flags & VIRTQ_DESC_F_NEXT) != 0;
        if (!has_next) {
            break;
        }

        if (num_popped >= n && has_next) {
            //尝试处理比参数 n 指定的更多的描述符
            return ERR_NO_MEMORY;
        }

        next_desc_index = desc->next;
    }

    //将处理过的描述符返回到空闲列表
    DEBUG_ASSERT(desc != NULL);
    desc->next = vq->free_head;
    vq->free_head = used_elem->id;
    vq->num_free_descs += num_popped;

    *total_len = used_elem->len;
    vq->last_used_index++;
    return num_popped;
}

//读取 1 个字节的器件配置
uint8_t virtio_read_device_config8(struct virtio_mmio *dev, offset_t offset) {
    return mmio_read8(dev->base + VIRTIO_REG_DEVICE_CONFIG + offset);
}

//读取中断状态寄存器
uint32_t virtio_read_interrupt_status(struct virtio_mmio *dev) {
    return mmio_read32le(dev->base + VIRTIO_REG_INTERRUPT_STATUS);
}

//通知设备中断处理完成
void virtio_ack_interrupt(struct virtio_mmio *dev, uint32_t status) {
    mmio_write32le(dev->base + VIRTIO_REG_INTERRUPT_ACK, status);
}

//加载设备支持的功能
uint64_t virtio_read_device_features(struct virtio_mmio *dev) {
    //读取低32位
    mmio_write32le(dev->base + VIRTIO_REG_DEVICE_FEATURES_SEL, 0);
    uint32_t low = mmio_read32le(dev->base + VIRTIO_REG_DEVICE_FEATURES);

    //读取高32位
    mmio_write32le(dev->base + VIRTIO_REG_DEVICE_FEATURES_SEL, 1);
    uint32_t high = mmio_read32le(dev->base + VIRTIO_REG_DEVICE_FEATURES);
    return ((uint64_t) high << 32) | low;
}

//启用设备的参数 features 指定的功能。如果有任何不支持的功能，
//返回 ERR_NOT_SUPPORTED。
error_t virtio_negotiate_feature(struct virtio_mmio *dev, uint64_t features) {
    if ((virtio_read_device_features(dev) & features) != features) {
        return ERR_NOT_SUPPORTED;
    }

    mmio_write32le(dev->base + VIRTIO_REG_DEVICE_FEATURES_SEL, 0);
    mmio_write32le(dev->base + VIRTIO_REG_DEVICE_FEATURES, features);
    mmio_write32le(dev->base + VIRTIO_REG_DEVICE_FEATURES_SEL, 1);
    mmio_write32le(dev->base + VIRTIO_REG_DEVICE_FEATURES, features >> 32);
    write_device_status(dev, read_device_status(dev) | VIRTIO_STATUS_FEAT_OK);

    if ((read_device_status(dev) & VIRTIO_STATUS_FEAT_OK) == 0) {
        return ERR_NOT_SUPPORTED;
    }

    return OK;
}

// 初始化 virtio 设备。之后，使用 virtio_negotiate_feature 函数指定设备功能。
// 您需要使用 virtio_enable 函数启用设备。
error_t virtio_init(struct virtio_mmio *dev, paddr_t base_paddr,
                    unsigned num_queues) {
    error_t err = driver_map_pages(base_paddr, PAGE_SIZE,
                                   PAGE_READABLE | PAGE_WRITABLE, &dev->base);
    if (err != OK) {
        return err;
    }

    if (mmio_read32le(dev->base + VIRTIO_REG_MAGIC_VALUE)
        != VIRTIO_MMIO_MAGIC_VALUE) {
        return ERR_NOT_SUPPORTED;
    }

    //重置您的设备
    write_device_status(dev, 0);
    write_device_status(dev, read_device_status(dev) | VIRTIO_STATUS_ACK);
    write_device_status(dev, read_device_status(dev) | VIRTIO_STATUS_DRIVER);

    //初始化每个virtqueue
    dev->num_queues = num_queues;
    dev->virtqs = malloc(sizeof(*dev->virtqs) * num_queues);
    for (unsigned i = 0; i < num_queues; i++) {
        virtq_init(dev, i);
    }

    return OK;
}

//启用 Virtio 设备
error_t virtio_enable(struct virtio_mmio *dev) {
    write_device_status(dev, read_device_status(dev) | VIRTIO_STATUS_DRIVER_OK);
    return OK;
}
