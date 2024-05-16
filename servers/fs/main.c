#include "main.h"
#include "block.h"
#include "fs.h"
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/ipc.h>

//打开的文件列表。索引用作文件描述符。
//由所有任务共享。
static struct open_file open_files[OPEN_FILES_MAX];

//分配文件描述符。
static int alloc_fd(void) {
    for (int i = 0; i < OPEN_FILES_MAX; i++) {
        if (!open_files[i].used) {
            open_files[i].used = true;
            return i + 1;
        }
    }

    return 0;
}

//从文件描述符中获取文件管理结构。
static struct open_file *lookup_open_file(task_t task, int fd) {
    if (fd < 1 || fd > OPEN_FILES_MAX) {
        return NULL;
    }

    struct open_file *file = &open_files[fd - 1];
    if (!file->used || file->task != task) {
        return NULL;
    }

    return file;
}

//释放文件管理结构。
static void free_open_file(struct open_file *file) {
    file->used = false;
}

//释放文件描述符。
static void free_fd(task_t task, int fd) {
    struct open_file *file = lookup_open_file(task, fd);
    if (!file) {
        return;
    }

    free_open_file(file);
}

//任务完成时调用。关闭该任务打开的所有文件。
static void do_task_destroyed(task_t task) {
    for (int i = 0; i < OPEN_FILES_MAX; i++) {
        struct open_file *file = &open_files[i];
        if (file->used && file->task == task) {
            free_open_file(file);
        }
    }
}

//打开文件并返回文件描述符。
static int do_open(task_t task, const char *path) {
    struct block *entry_block;
    error_t err = fs_find(path, &entry_block);
    if (err != OK) {
        return err;
    }

    int fd = alloc_fd();
    if (!fd) {
        return ERR_NO_RESOURCES;
    }

    struct open_file *file = &open_files[fd - 1];
    file->entry_block = entry_block;
    file->entry = (struct hinafs_entry *) entry_block->data;
    file->task = task;
    file->offset = 0;
    return fd;
}

//读取和写入文件。
static int do_readwrite(task_t task, int fd, void *buf, size_t len,
                        bool write) {
    struct open_file *file = lookup_open_file(task, fd);
    if (!file) {
        return ERR_INVALID_ARG;
    }

    if (!write) {
        len = MIN(len, file->entry->size - file->offset);
        if (file->offset >= file->entry->size) {
            return ERR_EOF;
        }
    }

    error_t err =
        fs_readwrite(file->entry_block, buf, len, file->offset, write);
    if (err != OK) {
        return err;
    }

    file->offset += len;
    return len;
}

void main(void) {
    //各组件的初始化
    block_init();
    fs_init();

    //注册通知Vm服务器任务完成
    struct message m;
    m.type = WATCH_TASKS_MSG;
    ASSERT_OK(ipc_call(VM_SERVER, &m));

    //注册为文件系统服务器
    ASSERT_OK(ipc_register("fs"));
    TRACE("ready");

    while (true) {
        //将修改后的块写回磁盘
        block_flush_all();

        struct message m;
        error_t err = ipc_recv(IPC_ANY, &m);
        ASSERT_OK(err);

        switch (m.type) {
            case TASK_DESTROYED_MSG: {
                if (m.src != 1) {
                    WARN("got a message from an unexpected source: %d", m.src);
                    break;
                }
                do_task_destroyed(m.task_destroyed.task);
                break;
            }
            case FS_OPEN_MSG: {
                char path[sizeof(m.fs_open.path)];
                strcpy_safe(path, sizeof(path), m.fs_open.path);

                int fd_or_err = do_open(m.src, path);
                if (IS_ERROR(fd_or_err)) {
                    ipc_reply_err(m.src, fd_or_err);
                    break;
                }

                m.type = FS_OPEN_REPLY_MSG;
                m.fs_open_reply.fd = fd_or_err;
                ipc_reply(m.src, &m);
                break;
            }
            case FS_CLOSE_MSG: {
                free_fd(m.src, m.fs_close.fd);
                m.type = FS_CLOSE_REPLY_MSG;
                ipc_reply(m.src, &m);
                break;
            }
            case FS_READ_MSG: {
                uint8_t buf[512];
                size_t len = MIN(m.fs_read.len, sizeof(buf));
                int read_len =
                    do_readwrite(m.src, m.fs_read.fd, buf, len, false);
                if (IS_ERROR(read_len)) {
                    ipc_reply_err(m.src, read_len);
                    break;
                }

                m.type = FS_READ_REPLY_MSG;
                memcpy(m.fs_read_reply.data, buf, read_len);
                m.fs_read_reply.data_len = read_len;
                ipc_reply(m.src, &m);
                break;
            }
            case FS_WRITE_MSG: {
                size_t len = MIN(m.fs_write.data_len, sizeof(m.fs_write.data));
                size_t written_len = do_readwrite(m.src, m.fs_write.fd,
                                                  m.fs_write.data, len, true);
                if (IS_ERROR(written_len)) {
                    WARN("failed to write a file (%s)", err2str(written_len));
                    ipc_reply_err(m.src, written_len);
                    break;
                }

                m.type = FS_WRITE_REPLY_MSG;
                m.fs_write_reply.written_len = written_len;
                ipc_reply(m.src, &m);
                break;
            }
            case FS_READDIR_MSG: {
                char path[sizeof(m.fs_readdir.path)];
                strcpy_safe(path, sizeof(path), m.fs_readdir.path);

                struct hinafs_entry *entry;
                error_t err = fs_readdir(path, m.fs_readdir.index, &entry);
                if (IS_ERROR(err)) {
                    ipc_reply_err(m.src, err);
                    break;
                }

                m.type = FS_READDIR_REPLY_MSG;
                strcpy_safe(m.fs_readdir_reply.name,
                            sizeof(m.fs_readdir_reply.name), entry->name);
                m.fs_readdir_reply.type = entry->type;
                m.fs_readdir_reply.filesize =
                    (entry->type == FS_TYPE_FILE) ? entry->size : 0;
                ipc_reply(m.src, &m);
                break;
            }
            case FS_MKFILE_MSG: {
                char path[sizeof(m.fs_mkfile.path)];
                strcpy_safe(path, sizeof(path), m.fs_mkfile.path);

                error_t err = fs_create(path, FS_TYPE_FILE);
                if (err != OK) {
                    ipc_reply_err(m.src, err);
                    break;
                }

                m.type = FS_MKFILE_REPLY_MSG;
                ipc_reply(m.src, &m);
                break;
            }
            case FS_MKDIR_MSG: {
                char path[sizeof(m.fs_mkdir.path)];
                strcpy_safe(path, sizeof(path), m.fs_mkdir.path);

                error_t err = fs_create(path, FS_TYPE_DIR);
                if (IS_ERROR(err)) {
                    ipc_reply_err(m.src, err);
                    break;
                }

                m.type = FS_MKDIR_REPLY_MSG;
                ipc_reply(m.src, &m);
                break;
            }
            case FS_DELETE_MSG: {
                char path[sizeof(m.fs_delete.path)];
                strcpy_safe(path, sizeof(path), m.fs_delete.path);

                error_t err = fs_delete(path);
                if (IS_ERROR(err)) {
                    ipc_reply_err(m.src, err);
                    break;
                }

                m.type = FS_DELETE_REPLY_MSG;
                ipc_reply(m.src, &m);
                break;
            }
            default:
                WARN("unknown message type: %s from %d", msgtype2str(m.type),
                     m.src);
        }
    }
}
