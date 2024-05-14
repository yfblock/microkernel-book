#pragma once
#include <libs/common/types.h>

//初始化列表。列表初始化函数的替代方案。当列表被声明为静态变量时很有用。
#define LIST_INIT(list)                                                        \
    { .prev = &(list), .next = &(list) }

//从列表中删除第一个条目。容器是一个包含条目的结构体，字段是list_elem_t
//字段名称。与list_pop_front函数不同，它不包含指向list_elem_t的指针。
//返回指向结构（容器）的指针。如果没有条目则返回 NULL。
#define LIST_POP_FRONT(list, container, field)                                 \
    ({                                                                         \
        list_elem_t *__elem = list_pop_front(list);                            \
        (__elem) ? LIST_CONTAINER(__elem, container, field) : NULL;            \
    })

//从指针（elem）获取指向包含list_elem_t的结构体（容器）的指针。
#define LIST_CONTAINER(elem, container, field)                                 \
    ((container *) ((vaddr_t) (elem) -offsetof(container, field)))

//为列表中的每个条目实现所谓的 foreach 语句的宏。 elem 是一个 foreach 语句
//您想要使用的任何变量名称，list 是列表，container 是包含条目的结构，field 是
//list_elem_t 的字段名称。
//
//如何使用：
//
//结构体元素{
//list_elem_t 下一个；
//int foo;
//};
//
//LIST_FOR_EACH(elem, &elems, struct element, next) { printf("foo: %d", elem->foo);
//}
//
//请注意，提取 __next 是因为如果在 foreach 语句中删除 elem，则 elem->next 为
//变得无效。
#define LIST_FOR_EACH(elem, list, container, field)                            \
    for (container *elem = LIST_CONTAINER((list)->next, container, field),     \
                   *__next = NULL;                                             \
         (&elem->field != (list)                                               \
          && (__next = LIST_CONTAINER(elem->field.next, container, field)));   \
         elem = __next)

//列表（侵入式数据结构）
struct list {
    struct list *prev;//结束 (list_t) 或上一个条目 (list_elem_t)
    struct list *next;//第一个 (list_t) 或下一个条目 (list_elem_t)
};

typedef struct list list_t;//列表
typedef struct list list_elem_t;//列表条目

void list_init(list_t *list);
void list_elem_init(list_elem_t *elem);
bool list_is_empty(list_t *list);
bool list_is_linked(list_elem_t *elem);
size_t list_len(list_t *list);
bool list_contains(list_t *list, list_elem_t *elem);
void list_remove(list_elem_t *elem);
void list_push_back(list_t *list, list_elem_t *new_tail);
list_elem_t *list_pop_front(list_t *list);
