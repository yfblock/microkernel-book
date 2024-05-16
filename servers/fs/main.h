#pragma once
#include <libs/common/types.h>

#define WRITE_BACK_INTERVAL 1000
#define OPEN_FILES_MAX      64

//打开文件信息
struct open_file {
    bool used;//您使用这种管理结构吗？
    task_t task;//打开此文件的任务
    struct hinafs_entry *entry;//文件入口
    struct block *entry_block;//包含文件条目的块
    uint32_t offset;//当前偏移量（读/写操作时移动）
};
