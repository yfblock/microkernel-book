#include "block.h"
#include "fs.h"
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/ipc.h>
#include <libs/user/malloc.h>
#include <servers/virtio_blk/virtio_blk.h>  //扇区大小
//块设备驱动程序服务器任务 ID。
static task_t blk_server;
//缓存块的列表。
static list_t cached_blocks = LIST_INIT(cached_blocks);
//已修改块的列表。必须写回磁盘。
static list_t dirty_blocks = LIST_INIT(dirty_blocks);

//将块号转换为扇区号。
static uint64_t block_to_sector(block_t index) {
    return (index * BLOCK_SIZE) / SECTOR_SIZE;
}

//返回块是否已被修改。
static bool block_is_dirty(struct block *block) {
    return list_is_linked(&block->dirty_next);
}

//将块写入磁盘。
static void block_write(struct block *block) {
    unsigned sector_base = block_to_sector(block->index);
    //分别写入每个扇区
    for (int offset = 0; offset < BLOCK_SIZE; offset += SECTOR_SIZE) {
        struct message m;
        m.type = BLK_WRITE_MSG;
        m.blk_write.sector = sector_base + (offset / SECTOR_SIZE);
        m.blk_write.data_len = SECTOR_SIZE;
        memcpy(m.blk_write.data, block->data + offset, SECTOR_SIZE);
        error_t err = ipc_call(blk_server, &m);
        if (err != OK) {
            OOPS("failed to write block %d: %s", block->index, err2str(err));
        }
    }
}

//将块读入块缓存。
error_t block_read(block_t index, struct block **block) {
    if (index == 0xffff) {
        OOPS("invalid block index: %x", index);
        return ERR_INVALID_ARG;
    }

    //如果已经缓存，则返回。
    LIST_FOR_EACH (b, &cached_blocks, struct block, cache_next) {
        if (b->index == index) {
            *block = b;
            return OK;
        }
    }

    //分配块高速缓冲存储器区域并读取每个扇区。
    TRACE("block %d is not in cache, reading from disk", index);
    struct block *new_block = malloc(sizeof(struct block));
    for (int offset = 0; offset < BLOCK_SIZE; offset += SECTOR_SIZE) {
        //向设备驱动服务器发送扇区读请求。
        struct message m;
        m.type = BLK_READ_MSG;
        m.blk_read.sector = block_to_sector(index) + (offset / SECTOR_SIZE);
        m.blk_read.len = SECTOR_SIZE;
        error_t err = ipc_call(blk_server, &m);

        if (err != OK) {
            OOPS("failed to read block %d: %s", index, err2str(err));
            free(new_block);
            return err;
        }

        if (m.type != BLK_READ_REPLY_MSG) {
            OOPS("unexpected reply message type \"%s\" (expected=%s)",
                 msgtype2str(m.type), msgtype2str(BLK_READ_REPLY_MSG));
            free(new_block);
            return ERR_UNEXPECTED;
        }

        if (m.blk_read_reply.data_len != SECTOR_SIZE) {
            OOPS("invalid data length from the device: %d",
                 m.blk_read_reply.data_len);
            free(new_block);
            return ERR_UNEXPECTED;
        }

        //将读取到的磁盘数据复制到块缓存中。
        memcpy(&new_block->data[offset], m.blk_read_reply.data, SECTOR_SIZE);
    }

    //将块缓存添加到列表中并返回其指针。
    new_block->index = index;
    list_elem_init(&new_block->cache_next);
    list_elem_init(&new_block->dirty_next);
    list_push_back(&cached_blocks, &new_block->cache_next);
    *block = new_block;
    return OK;
}

//将块标记为已修改。
void block_mark_as_dirty(struct block *block) {
    if (!block_is_dirty(block)) {
        list_push_back(&dirty_blocks, &block->dirty_next);
    }
}

//将所有修改的块写入磁盘。
void block_flush_all(void) {
    LIST_FOR_EACH (b, &dirty_blocks, struct block, dirty_next) {
        block_write(b);
        list_remove(&b->dirty_next);
    }
}

//块缓存层的初始化。
void block_init(void) {
    //获取设备驱动服务器的任务ID。
    blk_server = ipc_lookup("blk_device");
}
