#pragma once

#include "block.h"
#include <libs/common/types.h>

#define FS_MAGIC           0xf2005346//幻数
#define FS_HEADER_BLOCK    0//文件系统头块号
#define ROOT_DIR_BLOCK     1//根目录的块号
#define BITMAP_FIRST_BLOCK 2//位图表的第一个块号
#define NUM_BITMAP_BLOCKS  4//位图表中的块数

#define BLOCKS_PER_ENTRY 1908//一个条目中包含的最大数据块数
#define FS_TYPE_DIR      0xdd//目录项类型：目录
#define FS_TYPE_FILE     0xff//文件条目类型：文件
#define FS_NAME_LEN      256//条目名称的最大长度
//文件系统上的每个条目
//
//例如，如果您有一个名为 /foo/bar/hello.txt 的文件，则将包含三个条目：
//
//-条目名称为“foo”的目录条目
//-条目名称为“bar”的目录条目
//-条目名称为“hello.txt”的文件条目
struct hinafs_entry {
    uint8_t type;//条目类型（文件或目录）
    uint8_t padding[3];//填充
    char name[FS_NAME_LEN];//条目名称（以空字符结尾）
    union {
        //字段仅对文件条目有效
        struct {
            uint32_t size;//文件大小
        };

        //仅对目录条目有效的字段
        struct {
            uint16_t num_entries;//目录中的条目数
            uint16_t padding2;//填充
        };
    };
    int64_t created_at;//创建日期和时间（未使用）
    int64_t modified_at;//最后更新时间（未使用）
    block_t blocks[BLOCKS_PER_ENTRY];//数据块列表：
//-文件：文件数据
//-目录：目录中的每个条目
} __packed;

//文件系统头
struct hinafs_header {
    uint32_t magic;//幻数。必须是 FS_MAGIC。
    uint32_t num_data_blocks;//数据块的数量。
    uint8_t padding[4088];//填充。需要匹配块大小。

    //该标头后面跟随以下数据：
//struct hinafs_entry root_dir; //根目录
//uint8_t bitmap_blocks[num_bitmap_blocks *BLOCK_SIZE]; //位图
//uint8_t 块[num_data_blocks *BLOCK_SIZE]; //数据块
} __packed;

STATIC_ASSERT(sizeof(struct hinafs_header) == BLOCK_SIZE,
              "hinafs_header size must be equal to block size");
STATIC_ASSERT(sizeof(struct hinafs_entry) == BLOCK_SIZE,
              "hinafs_entry size must be equal to block size");

error_t fs_find(const char *path, struct block **entry_block);
error_t fs_create(const char *path, uint8_t type);
error_t fs_readwrite(struct block *entry_block, void *buf, size_t size,
                     size_t offset, bool write);
error_t fs_readdir(const char *path, int index, struct hinafs_entry **entry);
error_t fs_delete(const char *path);
void fs_init(void);
