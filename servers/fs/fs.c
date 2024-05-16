#include "fs.h"
#include "block.h"
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/malloc.h>

//位图块对应的块缓存用于管理空闲块
static struct block *bitmap_blocks[NUM_BITMAP_BLOCKS];
//根目录中的块缓存
static struct block *root_dir_block;

//分配未使用的块号并将位图块记录为正在使用。
static block_t alloc_block(void) {
    for (int i = 0; i < NUM_BITMAP_BLOCKS; i++) {
        struct block *b = bitmap_blocks[i];
        for (int j = 0; j < BLOCK_SIZE; j++) {
            //检查每一位，如果发现 0 位，请将其设置为 1 并返回。
            for (int k = 0; k < 8; k++) {
                if ((b->data[j] & (1 << k)) == 0) {
                    //记录该位图正在使用中。
                    b->data[j] |= (1 << k);

                    //将位图块标记为已修改。
                    block_mark_as_dirty(b);

                    block_t block_off = (i * BLOCK_SIZE + j) * 8 + k;
                    //由于存在头/根目录块，因此添加 2。
                    return 2 + NUM_BITMAP_BLOCKS + block_off;
                }
            }
        }
    }

    WARN("no free data blocks");
    return 0;
}

//接收块号并记录它在位图块中未被使用。
static void free_block(block_t index) {
    int i = index / (BLOCK_SIZE * 8);
    int j = (index - (i * BLOCK_SIZE * 8)) / 8;
    int k = index % 8;

    struct block *b = bitmap_blocks[i];
    b->data[j] &= ~(1 << k);
    block_mark_as_dirty(b);
}

//从文件路径中找到目录项块。如果parent_dir为真，
//查找路径指示的条目的父目录。
static error_t lookup(const char *path, bool parent_dir,
                      struct block **entry_block) {
    char *p = strdup(path);
    char *p_original = p;
    struct hinafs_entry *dir = (struct hinafs_entry *) root_dir_block->data;

    //跳过前导斜杠。
    while (*p == '/') {
        p++;
    }

    //指向根目录时的处理。
    if (*p == '\0' || (parent_dir && !strchr(p, '/'))) {
        free(p_original);
        *entry_block = root_dir_block;
        return OK;
    }

    //从上到下搜索路径中的每个目录（/path/to/file、path、to、file）。
    while (true) {
        //将字符串提取到下一个斜杠并以空字符终止。在上面的例子中，
//p 指字符串“path”、“to”和“file”。
        char *slash = strchr(p, '/');
        bool last = slash == NULL;
        if (!last) {
            *slash = '\0';
        }

        //跳过空字符串（例如 /path//to/file）和“.”。
        if (strlen(p) == 0 || !strcmp(p, ".")) {
            p = slash + 1;
            continue;
        }

        //“..”被视为错误。
        if (!strcmp(p, "..")) {
            WARN(".. is not supported");
            return ERR_INVALID_ARG;
        }

        DEBUG_ASSERT(dir->type == FS_TYPE_DIR);

        //逐步浏览每个目录条目，查找名称匹配。
        struct block *eb = NULL;
        bool found = false;
        for (uint16_t i = 0; i < dir->num_entries; i++) {
            //加载入口块。
            block_t index = dir->blocks[i];
            error_t err = block_read(index, &eb);
            if (err != OK) {
                WARN("failed to read block %d: %s", index, err2str(err));
                free(p_original);
                return err;
            }

            //检查名称是否匹配。
            struct hinafs_entry *e = (struct hinafs_entry *) eb->data;
            if (!strcmp(p, e->name)) {
                dir = e;
                found = true;
                break;
            }
        }

        //没有找到匹配的条目。错误，因为路径不存在。
        if (!found) {
            free(p_original);
            return ERR_NOT_FOUND;
        }

        //当路径末端匹配时游戏结束。
        if (last || (parent_dir && strchr(p + 1, '/') == NULL)) {
            free(p_original);
            *entry_block = eb;
            return OK;
        }

        //找到下一个目录。
        p = slash + 1;
    }

    UNREACHABLE();
}

//读写文件
//
//读取和写入目录条目 (entry_block) 指示的文件。
static error_t readwrite(struct block *entry_block, void *buf, size_t size,
                         size_t offset, bool write) {
    //检查它是否真的是一个文件
    struct hinafs_entry *entry = (struct hinafs_entry *) entry_block->data;
    if (entry->type != FS_TYPE_FILE) {
        return ERR_NOT_A_FILE;
    }

    //如果偏移量超过文件长度，则返回 EOF。然而，在写作的时候，
//考虑扩展文件末尾的情况。
    bool valid_offset =
        offset < entry->size || (write && offset == entry->size);
    if (!valid_offset) {
        return ERR_EOF;
    }

    //读取和写入每个数据块。
    int first_i = offset / BLOCK_SIZE;
    int first_offset = offset % BLOCK_SIZE;
    size_t total_len = 0;//读/写的总字节数
    for (int i = first_i; total_len < size && i < BLOCKS_PER_ENTRY; i++) {
        //检查数据块是否存在
        block_t index = entry->blocks[i];
        if (!index) {
            DEBUG_ASSERT(write);

            //由于该数据块不存在，因此分配一个新的数据块。
            index = alloc_block();
            if (index == 0) {
                return ERR_NO_RESOURCES;
            }

            //将新分配的数据块添加到目录项。
            entry->blocks[i] = index;
            //由于目录项已更改，因此将其注册为已更改的块。
            block_mark_as_dirty(entry_block);
        }

        //读取数据块。即使是写操作，也会被读取一次并存储在块缓存上。
//改变。
        struct block *data_block;
        error_t err = block_read(index, &data_block);
        if (err != OK) {
            WARN("failed to read block %d: %s", index, err2str(err));
            return err;
        }

        size_t copy_len = MIN(size - total_len, BLOCK_SIZE);
        if (write) {
            //写入数据块并注册为更改块
            memcpy(&data_block->data[first_offset], buf + total_len, copy_len);
            block_mark_as_dirty(data_block);
        } else {
            //从数据块读取
            memcpy(buf + total_len, &data_block->data[first_offset], copy_len);
        }

        total_len += copy_len;
        first_offset = 0;
    }

    if (write) {
        //更新文件长度并注册修改后的目录条目块
        entry->size = offset + total_len;
        block_mark_as_dirty(entry_block);
    }

    return OK;
}

//删除文件或目录。
error_t fs_delete(const char *path) {
    //打开父目录
    struct block *dir_block;
    error_t err = lookup(path, true, &dir_block);
    if (err != OK) {
        return err;
    }

    //打开要删除的目录项
    struct block *entry_block;
    err = lookup(path, false, &entry_block);
    if (err != OK) {
        return err;
    }

    struct hinafs_entry *entry = (struct hinafs_entry *) entry_block->data;
    switch (entry->type) {
        case FS_TYPE_FILE:
            //如果是文件，则释放其所有数据块。
            for (int i = 0; i < BLOCKS_PER_ENTRY; i++) {
                block_t index = entry->blocks[i];
                if (index == 0) {
                    break;
                }

                free_block(index);
            }
            break;
        case FS_TYPE_DIR:
            //如果是目录，检查是否已经为空
            if (entry->num_entries > 0) {
                return ERR_NOT_EMPTY;
            }
            break;
        default:
            UNREACHABLE();
    }

    //删除父目录中要删除的目录项
    struct hinafs_entry *dir = (struct hinafs_entry *) dir_block->data;
    for (uint16_t i = 0; i < dir->num_entries; i++) {
        if (dir->blocks[i] == entry_block->index) {
            //将最后一个条目与要删除的条目交换以减少条目数。
//通过交换，无需将所有条目一一移动。
            dir->blocks[i] = dir->blocks[dir->num_entries - 1];
            dir->blocks[dir->num_entries - 1] = 0;
            dir->num_entries--;

            //由于条目列表已更改，请将父目录注册为已更改。
            block_mark_as_dirty(dir_block);
            break;
        }
    }

    return OK;
}

//提取文件路径的最后一个元素。例如，“baz”代表“/foo/bar/baz”
//把它返还。
//
//调用者必须确保路径不以空字符串或“/”结尾。
static const char *basename(const char *path) {
    const char *slash = &path[strlen(path)];
    while (true) {
        if (slash == path) {
            return path;
        }

        slash--;

        if (*slash == '/') {
            return slash + 1;
        }
    }
}

//创建文件或目录。
error_t fs_create(const char *path, uint8_t type) {
    const char *name = basename(path);
    //检查文件名是否太长
    if (strlen(name) >= FS_NAME_LEN) {
        return ERR_INVALID_ARG;
    }

    //检查文件名是否不是空字符串
    if (strlen(name) == 0) {
        return ERR_INVALID_ARG;
    }

    //检查文件名中的控制字符
    for (size_t i = 0; i < strlen(name); i++) {
        if (name[i] < 0x20 || name[i] > 0x7e) {
            return ERR_INVALID_ARG;
        }
    }

    //检查文件名是否重复
    struct block *tmp_block;
    if (lookup(path, false, &tmp_block) == OK) {
        return ERR_ALREADY_EXISTS;
    }

    //打开父目录
    struct block *dir_block;
    error_t err = lookup(path, true, &dir_block);
    if (err != OK) {
        return err;
    }

    //获取目录项块
    struct hinafs_entry *dir = (struct hinafs_entry *) dir_block->data;
    if (dir->num_entries >= BLOCKS_PER_ENTRY) {
        //目录条目已满
        return ERR_NO_RESOURCES;
    }

    //为目录条目分配一个新块
    block_t new_index = alloc_block();
    if (new_index == 0) {
        return ERR_NO_RESOURCES;
    }

    //将分配的块放入块缓存中
    struct block *entry_block;
    err = block_read(new_index, &entry_block);
    if (err != OK) {
        WARN("failed to read block %d: %s", new_index, err2str(err));
        free_block(new_index);
        return err;
    }

    //初始化目录项
    struct hinafs_entry *entry = (struct hinafs_entry *) entry_block->data;
    memset(entry, 0, sizeof(*entry));
    entry->type = type;
    entry->size = 0;
    strcpy_safe(entry->name, sizeof(entry->name), name);

    ASSERT(strchr(entry->name, '/') == NULL);

    //向目录添加目录条目
    dir->blocks[dir->num_entries] = new_index;
    dir->num_entries++;

    //将磁盘标记为写回
    block_mark_as_dirty(dir_block);
    block_mark_as_dirty(entry_block);
    return OK;
}

//查找指定路径对应的目录项。
error_t fs_find(const char *path, struct block **entry_block) {
    return lookup(path, false, entry_block);
}

//读取和写入文件。
error_t fs_readwrite(struct block *entry_block, void *buf, size_t size,
                     size_t offset, bool write) {
    return readwrite(entry_block, buf, size, offset, write);
}

//获取目录中的索引条目。
error_t fs_readdir(const char *path, int index, struct hinafs_entry **entry) {
    //读取目录块
    struct block *dir_block;
    error_t err = lookup(path, false, &dir_block);
    if (err != OK) {
        return err;
    }

    //检查是否目录
    struct hinafs_entry *dir = (struct hinafs_entry *) dir_block->data;
    if (dir->type != FS_TYPE_DIR) {
        return ERR_NOT_A_DIR;
    }

    //检查Index是否不超过目录中的条目数
    if (index >= dir->num_entries) {
        return ERR_EOF;
    }

    //读取索引条目
    struct block *entry_block;
    block_t block_index = dir->blocks[index];
    err = block_read(block_index, &entry_block);
    if (err != OK) {
        WARN("failed to read block %d: %s", block_index, err2str(err));
        return err;
    }

    *entry = (struct hinafs_entry *) entry_block->data;
    return OK;
}

//初始化文件系统层。
void fs_init(void) {
    //读取文件系统头
    struct block *header_block;
    error_t err = block_read(FS_HEADER_BLOCK, &header_block);
    if (err != OK) {
        PANIC("failed to read the header block: %s", err2str(err));
    }

    //检查文件系统头
    struct hinafs_header *header = (struct hinafs_header *) header_block->data;
    if (header->magic != FS_MAGIC) {
        PANIC("invalid file system magic: %x", header->magic);
    }

    //加载根目录
    err = block_read(ROOT_DIR_BLOCK, &root_dir_block);
    if (err != OK) {
        PANIC("failed to read the root directory block: %s", err2str(err));
    }

    //检查根目录
    struct hinafs_entry *root_dir =
        (struct hinafs_entry *) root_dir_block->data;
    if (root_dir->type != FS_TYPE_DIR) {
        PANIC("invalid root directory type: %x", root_dir->type);
    }

    //加载每个位图块
    for (int i = 0; i < NUM_BITMAP_BLOCKS; i++) {
        err = block_read(BITMAP_FIRST_BLOCK + i, &bitmap_blocks[i]);
        if (err != OK) {
            PANIC("failed to read the bitmap block: %s", err2str(err));
        }
    }

    INFO("successfully loaded the file system");
}
