//设备驱动程序 API。基本上是虚拟机服务器的消息传递包装器。
#include <libs/user/driver.h>
#include <libs/user/ipc.h>

//将指定的物理内存区域映射到空闲的虚拟地址区域。我想访问MMIO区域
//有时可以使用。
//
//在参数map_flags中指定内存区域权限PAGE_(READABLE|WRITABLE|EXECUTABLE)。
error_t driver_map_pages(paddr_t paddr, size_t size, int map_flags,
                         uaddr_t *uaddr) {
    struct message m;
    m.type = VM_MAP_PHYSICAL_MSG;
    m.vm_map_physical.paddr = paddr;
    m.vm_map_physical.size = size;
    m.vm_map_physical.map_flags = map_flags;
    error_t err = ipc_call(VM_SERVER, &m);
    if (err != OK) {
        return err;
    }

    *uaddr = m.vm_map_physical_reply.uaddr;
    return OK;
}

//分配物理内存区域。
//
//在参数map_flags中指定内存区域权限PAGE_(READABLE|WRITABLE|EXECUTABLE)。
error_t driver_alloc_pages(size_t size, int map_flags, uaddr_t *uaddr,
                           paddr_t *paddr) {
    struct message m;
    m.type = VM_ALLOC_PHYSICAL_MSG;
    m.vm_alloc_physical.size = size;
    m.vm_alloc_physical.alloc_flags = 0;
    m.vm_alloc_physical.map_flags = map_flags;
    error_t err = ipc_call(VM_SERVER, &m);
    if (err != OK) {
        return err;
    }

    *uaddr = m.vm_alloc_physical_reply.uaddr;
    *paddr = m.vm_alloc_physical_reply.paddr;
    return OK;
}
