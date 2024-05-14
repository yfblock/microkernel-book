#include "dmabuf.h"
#include "driver.h"
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/malloc.h>

// 检查传递的物理地址是否在dmabuf的控制下
static void check_paddr(dmabuf_t dmabuf, paddr_t paddr) {
    ASSERT(dmabuf->paddr <= paddr);
    ASSERT(paddr < dmabuf->paddr + dmabuf->entry_size * dmabuf->num_entries);
}

// 生成 DMA 缓冲区管理结构。 Entry_size 是缓冲区中一个元素的大小。 num_entries 是
// 缓冲区中的元素数量。失败时返回 NULL。
dmabuf_t dmabuf_create(size_t entry_size, size_t num_entries) {
    // 保证返回的地址对齐
    entry_size = ALIGN_UP(entry_size, 4);

    // 保护并初始化缓冲区管理信息
    struct dmabuf *dmabuf = malloc(sizeof(struct dmabuf));
    dmabuf->entry_size = entry_size;
    dmabuf->num_entries = num_entries;
    dmabuf->used = malloc(sizeof(bool) * num_entries);
    memset(dmabuf->used, 0, sizeof(bool) * num_entries);

    // 保留 DMA 缓冲区
    error_t err = driver_alloc_pages(
        ALIGN_UP(entry_size * num_entries, PAGE_SIZE),
        PAGE_READABLE | PAGE_WRITABLE, &dmabuf->uaddr, &dmabuf->paddr);
    if (err != OK) {
        WARN("failed to allocate a DMA region for dmabuf: %s", err2str(err));
        return NULL;
    }

    DEBUG_ASSERT(dmabuf->paddr != 0);
    return dmabuf;
}

// 分配一个 DMA 缓冲区。失败时返回 null。
void *dmabuf_alloc(dmabuf_t dmabuf, paddr_t *paddr) {
    for (size_t i = 0; i < dmabuf->num_entries; i++) {
        if (!dmabuf->used[i]) {
            dmabuf->used[i] = true;
            offset_t offset = i * dmabuf->entry_size;
            *paddr = dmabuf->paddr + offset;
            return (void *) (dmabuf->uaddr + offset);
        }
    }
    return NULL;
}

// 从Dmabuf alloc函数分配的物理地址中获取对应的虚拟地址。
void *dmabuf_p2v(dmabuf_t dmabuf, paddr_t paddr) {
    check_paddr(dmabuf, paddr);
    return (void *) (dmabuf->uaddr + paddr - dmabuf->paddr);
}

// 释放 Dmabuf alloc 函数分配的 dma 缓冲区。
void dmabuf_free(dmabuf_t dmabuf, paddr_t paddr) {
    check_paddr(dmabuf, paddr);
    size_t index = (paddr - dmabuf->paddr) / dmabuf->entry_size;
    dmabuf->used[index] = false;
}
