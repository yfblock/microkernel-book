#include "bootfs.h"
#include <libs/common/print.h>
#include <libs/common/string.h>

extern char __bootfs[];//BootFS 映像
static struct bootfs_file *files;//BootFS 文件列表
static unsigned num_files;//BootFS 中的文件数量
//从 BootFS 加载文件。
void bootfs_read(struct bootfs_file *file, offset_t off, void *buf,
                 size_t len) {
    void *p = (void *) (((uaddr_t) __bootfs) + file->offset + off);
    memcpy(buf, p, len);
}

//打开引导 fs 文件。
struct bootfs_file *bootfs_open(const char *path) {
    //查找具有匹配文件名的条目。
    struct bootfs_file *file;
    for (int i = 0; (file = bootfs_open_iter(i)) != NULL; i++) {
        if (!strncmp(file->name, path, sizeof(file->name))) {
            return file;
        }
    }

    return NULL;
}

//返回第 Index 个文件条目。
struct bootfs_file *bootfs_open_iter(unsigned index) {
    if (index >= num_files) {
        return NULL;
    }

    return &files[index];
}

//初始化引导文件系统。
void bootfs_init(void) {
    struct bootfs_header *header = (struct bootfs_header *) __bootfs;
    num_files = header->num_files;
    files =
        (struct bootfs_file *) (((uaddr_t) &__bootfs) + header->header_size);

    TRACE("bootfs: found following %d files", num_files);
    for (unsigned i = 0; i < num_files; i++) {
        TRACE("bootfs: \"%s\" (%d KiB)", files[i].name, files[i].len / 1024);
    }
}
