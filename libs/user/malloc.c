#include <libs/common/list.h>
#include <libs/common/print.h>
#include <libs/common/string.h>
#include <libs/user/malloc.h>

extern char __heap[];//堆区起始地址
extern char __heap_end[];//堆区结束地址
//未使用的块列表
static list_t free_chunks = LIST_INIT(free_chunks);

//将 Ptr 中的 len 字节内存区域注册为新块。
static void insert(void *ptr, size_t len) {
    ASSERT(len > sizeof(struct malloc_chunk));

    //初始化 chunk 的每个字段
    struct malloc_chunk *new_chunk = ptr;
    new_chunk->magic = MALLOC_FREE;
    new_chunk->capacity = len - sizeof(struct malloc_chunk);
    new_chunk->size = 0;
    list_elem_init(&new_chunk->next);

    //添加到空闲列表
    list_push_back(&free_chunks, &new_chunk->next);
}

//将块大小减少到上限字节。分割后剩余的部分作为新的 chunk。
//添加到空闲列表。
static void split(struct malloc_chunk *chunk, size_t cap) {
    ASSERT(chunk->capacity >= cap + sizeof(struct malloc_chunk) + 8);

    //计算新块的总体大小
    size_t new_chunk_size = chunk->capacity - cap;

    //在块数据区域的末尾创建一个新块并相应地缩小块。
    void *new_chunk = &chunk->data[chunk->capacity - new_chunk_size];
    chunk->capacity = cap;

    //将新创建的块添加到空闲列表中。
    insert(new_chunk, new_chunk_size);
}

//动态内存分配。从堆中分配内存。与C标准库不同，内存分配
//如果失败，则终止程序。
void *malloc(size_t size) {
    //使请求大小为大于或等于 8 的 8 对齐数字。
//换句话说，以 8、16、24、32 等为单位进行分配。
    size = ALIGN_UP((size == 0) ? 1 : size, 8);

    LIST_FOR_EACH (chunk, &free_chunks, struct malloc_chunk, next) {
        ASSERT(chunk->magic == MALLOC_FREE);

        if (chunk->capacity >= size) {
            //如果它大到足以分割，则将其分割成两块：
//剩余的块将添加到空闲列表中。
//
//-分配的块（确切的分配大小）
//-剩余块（最小分配大小为 8 字节或更多）
            if (chunk->capacity >= size + sizeof(struct malloc_chunk) + 8) {
                split(chunk, size);
            }

            //将其标记为正在使用，将其从未使用的块列表中删除，然后
//将数据部分的起始地址返回给应用程序。
            chunk->magic = MALLOC_IN_USE;
            chunk->size = size;
            list_remove(&chunk->next);

            //将分配的内存区域清零。原来是来电者
//它应该被正确初始化，但是如果忘记初始化它，则很难调试错误。
//我会在这里做。
            memset(chunk->data, 0, chunk->size);
            return chunk->data;
        }
    }

    PANIC("out of memory");
}

//从指针获取块头。如果该指针不是由 malloc 函数分配的，则会出现恐慌。
static struct malloc_chunk *get_chunk_from_ptr(void *ptr) {
    struct malloc_chunk *chunk =
        (struct malloc_chunk *) ((uintptr_t) ptr - sizeof(struct malloc_chunk));

    ASSERT(chunk->magic == MALLOC_IN_USE);
    return chunk;
}

//释放由 Malloc 函数分配的内存区域。
void free(void *ptr) {
    struct malloc_chunk *chunk = get_chunk_from_ptr(ptr);
    if (chunk->magic == MALLOC_FREE) {
        //尝试释放已释放的内存区域（双重释放错误）
        PANIC("double-free bug!");
    }

    //将块返回到空闲列表
    chunk->magic = MALLOC_FREE;
    list_push_back(&free_chunks, &chunk->next);
}

//内存重新分配。将使用 malloc 函数分配的内存区域扩展为 size 字节
//返回一个新的内存区域。
void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }

    struct malloc_chunk *chunk = get_chunk_from_ptr(ptr);
    size_t prev_size = chunk->size;
    if (size <= chunk->capacity) {
        //如果当前块中有足够的剩余空间，则按原样返回。
        return ptr;
    }

    //分配新的内存空间并复制数据。
    void *new_ptr = malloc(size);
    memcpy(new_ptr, ptr, prev_size);
    free(ptr);
    return new_ptr;
}

//将字符串复制到新分配的内存区域并返回其起始地址。
//为了防止内存泄漏，必须使用free函数释放它。
char *strdup(const char *s) {
    size_t len = strlen(s);
    char *new_s = malloc(len + 1);
    memcpy(new_s, s, len + 1);
    return new_s;
}

//初始化动态内存分配。将堆空间添加到未使用的块列表中。
void malloc_init(void) {
    //堆区域（__heap、__heap_end）在链接描述文件中定义。
    insert(__heap, (size_t) __heap_end - (size_t) __heap);
}
