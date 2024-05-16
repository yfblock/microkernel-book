#pragma once

#include <libs/common/list.h>
#include <libs/common/types.h>

//块大小（字节）
#define BLOCK_SIZE 4096

//块号
typedef uint16_t block_t;

//块缓存
//
//当读取或写入存储设备的内容时，首先从设备中读取和写入 BLOCK_SIZE 数据。
//文件系统实现读取内存中的缓存数据并将其添加为块缓存。
//读和写。
struct block {
    block_t index;//磁盘上的块号
    list_elem_t cache_next;//块缓存列表的元素
    list_elem_t dirty_next;//修改块缓存列表的元素
    uint8_t data[BLOCK_SIZE];//块内容
};

error_t block_read(block_t index, struct block **block);
void block_mark_as_dirty(struct block *block);
void block_flush_all(void);
void block_init(void);
