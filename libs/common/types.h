#pragma once
#include "buildconfig.h"

typedef char int8_t;//8位有符号整数类型
typedef short int16_t;//16位有符号整数类型
typedef int int32_t;//32位有符号整数类型
typedef long long int64_t;//64位有符号整数类型
typedef unsigned char uint8_t;//8位无符号整数类型
typedef unsigned short uint16_t;//16位无符号整数类型
typedef unsigned uint32_t;//32位无符号整数类型
typedef unsigned long long uint64_t;//64位无符号整数类型

#if !defined(__LP64__)
//Int 的最大值
#    define INT_MAX 2147483647
//无符号整型的最大值
#    define UINT_MAX 4294967295U
//有符号整数类型的最大值
typedef int32_t intmax_t;
//无符号整数类型的最大值
typedef uint32_t uintmax_t;
#endif

//布尔值
typedef char bool;
#define true  1
#define false 0

//空指针
#define NULL ((void *) 0)

typedef int error_t;//表示错误码的整数类型
typedef int task_t;//任务ID
typedef int handle_t;//句柄ID
typedef uint32_t notifications_t;//代表通知的位域

typedef uintmax_t size_t;//表示大小的整数类型
typedef long pfn_t;//代表物理页号的整数类型
typedef uintmax_t paddr_t;//代表物理地址的整数类型
typedef uintmax_t vaddr_t;//代表虚拟地址的整数类型
typedef uintmax_t uaddr_t;//表示用户空间虚拟地址的整数类型
typedef uintmax_t uintptr_t;//整数类型，存储指针指向的地址
typedef uintmax_t offset_t;//表示偏移量的整数类型
//结构体属性：不插入padding
#define __packed __attribute__((packed))
//函数属性：该函数没有返回值
#define __noreturn __attribute__((noreturn))
//函数属性：应始终检查该函数的返回值。
#define __mustuse __attribute__((warn_unused_result))
//变量属性：确保与指定的对齐方式对齐
#define __aligned(aligned_to) __attribute__((aligned(aligned_to)))
//指针属性：表明该指针是由用户传递的。直接访问this指针
//相反，请使用安全函数（例如 memcpy_from_user 和 memcpy_to_user）来访问它们。
#define __user __attribute__((noderef, address_space(1)))

//变长参数
typedef __builtin_va_list va_list;
#define va_start(ap, param) __builtin_va_start(ap, param)
#define va_end(ap)          __builtin_va_end(ap)
#define va_arg(ap, type)    __builtin_va_arg(ap, type)

//如果 Expr 不为 0，则在编译时生成错误
#define STATIC_ASSERT(expr, summary) _Static_assert(expr, summary);
//获取结构体成员的偏移量
#define offsetof(type, field) __builtin_offsetof(type, field)
//将给定值向上舍入以适合指定的对齐方式
#define ALIGN_DOWN(value, align) __builtin_align_down(value, align)
//向下舍入给定值以匹配指定的对齐方式
#define ALIGN_UP(value, align) __builtin_align_up(value, align)
//判断给定值是否与指定对齐方式匹配
#define IS_ALIGNED(value, align) __builtin_is_aligned(value, align)

//原子读取指针的值
#define atomic_load(ptr) __atomic_load_n(ptr, __ATOMIC_SEQ_CST)
//以原子方式对指针值执行按位或赋值 (|=)
#define atomic_fetch_and_or(ptr, value) __sync_fetch_and_or(ptr, value)
//以原子方式对指针的值执行按位与赋值 (&=)
#define atomic_fetch_and_and(ptr, value) __sync_fetch_and_and(ptr, value)
//比较和交换 (CAS) 操作：分配新值，如果 ptr 的值是旧值则返回 true
#define compare_and_swap(ptr, old, new)                                        \
    __sync_bool_compare_and_swap(ptr, old, new)
//记忆屏障
#define full_memory_barrier __sync_synchronize

//返回 A 和 b 中较大的一个
#define MAX(a, b)                                                              \
    ({                                                                         \
        __typeof__(a) __a = (a);                                               \
        __typeof__(b) __b = (b);                                               \
        (__a > __b) ? __a : __b;                                               \
    })

//返回 A 和 b 中较小的一个
#define MIN(a, b)                                                              \
    ({                                                                         \
        __typeof__(a) __a = (a);                                               \
        __typeof__(b) __b = (b);                                               \
        (__a < __b) ? __a : __b;                                               \
    })

//
//错误代码
//
#define IS_OK(err)          (!IS_ERROR(err))//判断是否正常完成
#define IS_ERROR(err)       (((long) (err)) < 0)//判断是否有错误
#define OK                  0//正常终止
#define ERR_NO_MEMORY       -1//内存不足
#define ERR_NO_RESOURCES    -2//没有足够的资源
#define ERR_ALREADY_EXISTS  -3//已经存在
#define ERR_ALREADY_USED    -4//已使用
#define ERR_ALREADY_DONE    -5//已经完成
#define ERR_STILL_USED      -6//仍在使用中
#define ERR_NOT_FOUND       -7//找不到
#define ERR_NOT_ALLOWED     -8//未授权
#define ERR_NOT_SUPPORTED   -9//不支持
#define ERR_UNEXPECTED      -10//意外的输入值/情况
#define ERR_INVALID_ARG     -11//无效参数/输入值
#define ERR_INVALID_TASK    -12//无效的任务ID
#define ERR_INVALID_SYSCALL -13//无效的系统调用号
#define ERR_INVALID_PADDR   -14//无效的物理地址
#define ERR_INVALID_UADDR   -15//无效的用户空间地址
#define ERR_TOO_MANY_TASKS  -16//任务过多
#define ERR_TOO_LARGE       -17//太大
#define ERR_TOO_SMALL       -18//太小
#define ERR_WOULD_BLOCK     -19//被中断，因为它会阻塞
#define ERR_TRY_AGAIN       -20//暂时失败：重试可能会成功
#define ERR_ABORTED         -21//中断
#define ERR_EMPTY           -22//是空的
#define ERR_NOT_EMPTY       -23//不是空的
#define ERR_DEAD_LOCK       -24//发生死锁
#define ERR_NOT_A_FILE      -25//不是一个文件
#define ERR_NOT_A_DIR       -26//不是目录
#define ERR_EOF             -27//文件数据结束
#define ERR_END             -28//必须是最后一个错误码
//内存页大小
#define PAGE_SIZE 4096
//页框编号偏移量
#define PFN_OFFSET 12
//从物理地址中提取页框号
#define PADDR2PFN(paddr) ((paddr) >> PFN_OFFSET)
//从页框号中提取物理地址
#define PFN2PADDR(pfn) (((paddr_t) (pfn)) << PFN_OFFSET)

//如果从内核发送消息，则源任务 ID
#define FROM_KERNEL -1
//VM服务器的任务ID（第一个用户任务）
#define VM_SERVER 1

//系统调用号
#define SYS_IPC          1
#define SYS_NOTIFY       2
#define SYS_SERIAL_WRITE 3
#define SYS_SERIAL_READ  4
#define SYS_TASK_CREATE  5
#define SYS_TASK_DESTROY 6
#define SYS_TASK_EXIT    7
#define SYS_TASK_SELF    8
#define SYS_PM_ALLOC     9
#define SYS_VM_MAP       10
#define SYS_VM_UNMAP     11
#define SYS_IRQ_LISTEN   12
#define SYS_IRQ_UNLISTEN 13
#define SYS_TIME         14
#define SYS_UPTIME       15
#define SYS_HINAVM       16
#define SYS_SHUTDOWN     17

//pm_alloc() 的标志
#define PM_ALLOC_UNINITIALIZED 0//不需要清零
#define PM_ALLOC_ZEROED        (1 << 0)//要求清零
#define PM_ALLOC_ALIGNED       (1 << 1)//必须按请求大小对齐
//页面属性
#define PAGE_READABLE   (1 << 1)//可读
#define PAGE_WRITABLE   (1 << 2)//可写
#define PAGE_EXECUTABLE (1 << 3)//可执行文件
#define PAGE_USER       (1 << 4)//从用户空间访问
//页面错误的原因
#define PAGE_FAULT_READ    (1 << 0)//尝试加载页面时发生
#define PAGE_FAULT_WRITE   (1 << 1)//尝试写入页面时发生
#define PAGE_FAULT_EXEC    (1 << 2)//尝试运行页面时发生
#define PAGE_FAULT_USER    (1 << 3)//发生在用户态
#define PAGE_FAULT_PRESENT (1 << 4)//发生在已经存在的页面上
//异常类型
#define EXP_GRACE_EXIT          1//任务完成
#define EXP_INVALID_UADDR       2//尝试访问不可映射区域地址
#define EXP_INVALID_PAGER_REPLY 3//来自无效寻呼机的回复
#define EXP_ILLEGAL_EXCEPTION   4//非法CPU异常
