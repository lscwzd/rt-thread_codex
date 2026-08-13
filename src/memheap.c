/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * 文件      : memheap.c
 *
 * 修改记录：
 * 日期           作者         说明
 * 2012-04-10     Bernard      首次实现
 * 2012-10-16     Bernard      为堆对象增加互斥锁
 * 2012-12-29     Bernard      支持把 memheap 用作系统堆，并将互斥锁改为信号量锁
 * 2013-04-10     Bernard      增加 rt_memheap_realloc
 * 2013-05-24     Bernard      修复 rt_memheap_realloc 问题
 * 2013-07-11     Grissiom     修复内存块拆分问题
 * 2013-07-15     Grissiom     优化 rt_memheap_realloc
 * 2021-06-03     Flybreak     修复 AC6 开启 Oz 优化后的崩溃问题
 * 2023-03-01     Bernard      修复最小块大小的对齐问题
 */

/**
 * @file memheap.c
 * @brief 在显式连续内存区上实现可变长度的双链表堆。
 *
 * memheap 同时维护两种链：
 *
 * - `next/prev` 按物理地址连接每一个块，不论它空闲还是占用；
 * - `next_free/prev_free` 只连接空闲块，使分配时无需扫描所有占用块。
 *
 * 每个用户区前面都有 `struct rt_memheap_item` 块头。块头中的 magic 最低位
 * 表示使用状态，其余位用于发现越界写或错误指针；`pool_ptr` 让 free 能从
 * 用户指针反查所属 memheap。区域末尾放置零长度、已占用的哨兵块，使左右
 * 合并逻辑不必在每次操作中单独判断数组边界。
 *
 * 默认情况下每个 memheap 用一个二值信号量串行化操作，因此分配、释放可能
 * 阻塞且不能在中断上下文使用。`locked == RT_TRUE` 表示外层（系统堆包装层）
 * 已提供锁，此文件不会再次获取信号量，避免重复加锁；此模式要求调用者严格
 * 持有外层锁。分配策略由配置决定：通常沿空闲链首次适配，
 * RT_MEMHEAP_BEST_MODE 还影响释放后插回空闲链的顺序。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_MEMHEAP

#define DBG_TAG           "kernel.memheap"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

/* 块头魔数及最低位状态编码；MASK 用于忽略 USED/FREED 状态检查魔数。 */
#define RT_MEMHEAP_MAGIC        0x1ea01ea0
#define RT_MEMHEAP_MASK         0xFFFFFFFE
#define RT_MEMHEAP_USED         0x01
#define RT_MEMHEAP_FREED        0x00

#define RT_MEMHEAP_IS_USED(i)   ((i)->magic & RT_MEMHEAP_USED)
#define RT_MEMHEAP_MINIALLOC    RT_ALIGN(12, RT_ALIGN_SIZE)

#define RT_MEMHEAP_SIZE         RT_ALIGN(sizeof(struct rt_memheap_item), RT_ALIGN_SIZE)
#define MEMITEM_SIZE(item)      ((rt_uintptr_t)item->next - (rt_uintptr_t)item - RT_MEMHEAP_SIZE)
#define MEMITEM(ptr)            (struct rt_memheap_item*)((rt_uint8_t*)ptr - RT_MEMHEAP_SIZE)

/**
 * @brief 从空闲链和物理块链中同时移除一个即将被吞并的后继块。
 *
 * 参数使用 volatile 是为规避部分 AC6/IAR 优化器对连续链指针更新的误处理。
 * 调用者必须已经独占该 memheap，且 @p next_ptr 必须为空闲、位于两条链中。
 */
static void _remove_next_ptr(volatile struct rt_memheap_item *next_ptr)
{
    /* 拆除空闲链节点，同时把物理链中的前后邻居直接相连。 */
    next_ptr->next_free->prev_free = next_ptr->prev_free;
    next_ptr->prev_free->next_free = next_ptr->next_free;
    next_ptr->next->prev = next_ptr->prev;
    next_ptr->prev->next = next_ptr->next;
}

/**
 * @brief 在调用者提供的区域上初始化一个静态 memheap。
 *
 * @note 初始化后的布局为：
 *          +-----------------------------------+--------------------------+
 *          |             一个大空闲块          |      已用的结尾哨兵      |
 *          +-----------------------------------+--------------------------+
 *
 *          block_list --> 第一个大空闲块
 *
 *          结尾哨兵的用户区长度为 0，它会阻止合并越过区域末端。
 *
 * @param memheap 调用者提供的控制块。
 *
 * @param name 内核对象和内部锁的名称。
 *
 * @param start_addr 可分配连续区域的起始地址，必须满足块头对齐要求。
 *
 * @param size 区域总字节数，函数向下对齐并从中扣除首尾块头。
 *
 * @return 初始化成功返回 RT_EOK；非法参数通过断言报告。
 *
 * `free_header` 是位于控制块内部的空闲链哨兵，本身不代表物理内存块；
 * `block_list` 则指向实际区域中的首块。两条循环双链在初始化结束时各含一个
 * 真正空闲块。底层区域仍由调用者所有，注销时不会释放它。
 */
rt_err_t rt_memheap_init(struct rt_memheap *memheap,
                         const char        *name,
                         void              *start_addr,
                         rt_size_t         size)
{
    struct rt_memheap_item *item;

    RT_ASSERT(memheap != RT_NULL);

    /* 通过静态对象路径注册 memheap。 */
    rt_object_init(&(memheap->parent), RT_Object_Class_MemHeap, name);

    memheap->start_addr     = start_addr;
    memheap->pool_size      = RT_ALIGN_DOWN(size, RT_ALIGN_SIZE);
    memheap->available_size = memheap->pool_size - (2 * RT_MEMHEAP_SIZE);
    memheap->max_used_size  = memheap->pool_size - memheap->available_size;

    /* 初始化控制块内的空闲链哨兵，使空链也能使用统一的插入/删除逻辑。 */
    item            = &(memheap->free_header);
    item->magic     = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    item->pool_ptr  = memheap;
    item->next      = RT_NULL;
    item->prev      = RT_NULL;
    item->next_free = item;
    item->prev_free = item;

    /* free_list 始终指向这个哨兵，而不是某个可分配块。 */
    memheap->free_list = item;

    /* 在实际内存区首部原地构造覆盖所有可用空间的大空闲块。 */
    item            = (struct rt_memheap_item *)start_addr;
    item->magic     = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    item->pool_ptr  = memheap;
    item->next      = RT_NULL;
    item->prev      = RT_NULL;
    item->next_free = item;
    item->prev_free = item;

#ifdef RT_USING_MEMTRACE
    rt_memset(item->owner_thread_name, ' ', sizeof(item->owner_thread_name));
#endif /* RT_USING_MEMTRACE */

    item->next = (struct rt_memheap_item *)
                 ((rt_uint8_t *)item + memheap->available_size + RT_MEMHEAP_SIZE);
    item->prev = item->next;

    /* 物理块链的入口是区域内第一个真实块。 */
    memheap->block_list = item;

    /* 把唯一的大空闲块插入 free_header 之后。 */
    item->next_free = memheap->free_list->next_free;
    item->prev_free = memheap->free_list;
    memheap->free_list->next_free->prev_free = item;
    memheap->free_list->next_free            = item;

    /* 移到区域末端构造零长度已用哨兵，阻止合并穿越边界。
     */
    item = item->next;
    /* 已用状态保证释放路径不会试图与哨兵合并。 */
    item->magic     = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED);
    item->pool_ptr  = memheap;
    item->next      = (struct rt_memheap_item *)start_addr;
    item->prev      = (struct rt_memheap_item *)start_addr;
    /* 哨兵永远不进入空闲链。 */
    item->next_free = item->prev_free = RT_NULL;

    /* 二值信号量作为可睡眠互斥锁；外层系统堆模式可通过 locked 绕过它。 */
    rt_sem_init(&(memheap->lock), name, 1, RT_IPC_FLAG_PRIO);
    memheap->locked = RT_FALSE;

    LOG_D("memory heap: start addr 0x%08x, size %d, free list header 0x%08x",
          start_addr, size, &(memheap->free_header));

    return RT_EOK;
}
RTM_EXPORT(rt_memheap_init);

/**
 * @brief 注销由 rt_memheap_init() 建立的静态 memheap。
 *
 * @param heap memheap 控制块。
 *
 * @return 返回 RT_EOK。
 *
 * 本函数只注销信号量和对象，不检查在外分配，也不释放 @p start_addr。调用者
 * 必须确保没有并发操作、等待者或仍被使用的内存块。
 */
rt_err_t rt_memheap_detach(struct rt_memheap *heap)
{
    RT_ASSERT(heap);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);
    RT_ASSERT(rt_object_is_systemobject(&heap->parent));

    rt_sem_detach(&heap->lock);
    rt_object_detach(&(heap->parent));

    /* 两个内嵌对象均已注销，底层存储仍归调用者。 */
    return RT_EOK;
}
RTM_EXPORT(rt_memheap_detach);

/**
 * @brief 从指定 memheap 分配至少 @p size 字节。
 *
 * @param heap 目标 memheap。
 *
 * @param size 请求的最小字节数；会向上对齐并提升到最小可管理长度。
 *
 * @return 成功时返回块头之后的用户区，失败返回 RT_NULL。
 *
 * 算法沿空闲链寻找第一个容量足够的块。若余量能形成“新块头 + 最小用户区”，
 * 就把原块拆成已用前半块和空闲后半块；否则整块分配。统计中的
 * available_size 只计算空闲用户字节，拆分时新增块头也会消耗可用空间。
 */
void *rt_memheap_alloc(struct rt_memheap *heap, rt_size_t size)
{
    rt_err_t result;
    rt_size_t free_size;
    struct rt_memheap_item *header_ptr;

    RT_ASSERT(heap != RT_NULL);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

    /* 对齐并避免产生小于 RT_MEMHEAP_MINIALLOC 的用户块。 */
    size = RT_ALIGN(size, RT_ALIGN_SIZE);
    if (size < RT_MEMHEAP_MINIALLOC)
        size = RT_MEMHEAP_MINIALLOC;

    LOG_D("allocate %d on heap:%8.*s",
          size, RT_NAME_MAX, heap->parent.name);

    if (size < heap->available_size)
    {
        /* 先做无锁快速容量判断，再在锁内沿空闲链寻找候选块。 */
        free_size = 0;

        /* locked 为真表示外层已锁定；否则获取可睡眠的内部信号量。 */
        if (heap->locked == RT_FALSE)
        {
            result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
            if (result != RT_EOK)
            {
                rt_set_errno(result);

                return RT_NULL;
            }
        }

        /* 从空闲链第一个真实节点开始执行首次适配搜索。 */
        header_ptr = heap->free_list->next_free;
        while (header_ptr != heap->free_list && free_size < size)
        {
            /* 物理 next 地址减去当前块头和头部开销，得到净用户容量。 */
            free_size = MEMITEM_SIZE(header_ptr);
            if (free_size < size)
            {
                /* 当前块太小，继续检查下一空闲块。 */
                header_ptr = header_ptr->next_free;
            }
        }

        /* 找到容量足够的块后，决定拆分还是整块取走。 */
        if (free_size >= size)
        {
            /* 只有余量还能形成合法空闲块时才拆分，避免不可用碎片。 */
            if (free_size >= (size + RT_MEMHEAP_SIZE + RT_MEMHEAP_MINIALLOC))
            {
                struct rt_memheap_item *new_ptr;

                /* 新块头位于本次用户区末尾。 */
                new_ptr = (struct rt_memheap_item *)
                          (((rt_uint8_t *)header_ptr) + size + RT_MEMHEAP_SIZE);

                LOG_D("split: block[0x%08x] nextm[0x%08x] prevm[0x%08x] to new[0x%08x]",
                      header_ptr,
                      header_ptr->next,
                      header_ptr->prev,
                      new_ptr);

                /* 余块进入空闲状态并记录所属 memheap。 */
                new_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);

                /* free 时依赖此指针反查控制块。 */
                new_ptr->pool_ptr = heap;

#ifdef RT_USING_MEMTRACE
                rt_memset(new_ptr->owner_thread_name, ' ', sizeof(new_ptr->owner_thread_name));
#endif /* RT_USING_MEMTRACE */

                /* 在物理块链中把 new_ptr 插到 header_ptr 与原后继之间。 */
                new_ptr->prev          = header_ptr;
                new_ptr->next          = header_ptr->next;
                header_ptr->next->prev = new_ptr;
                header_ptr->next       = new_ptr;

                /* 原块将变为已用，因此从空闲链中摘除。 */
                header_ptr->next_free->prev_free = header_ptr->prev_free;
                header_ptr->prev_free->next_free = header_ptr->next_free;
                header_ptr->next_free = RT_NULL;
                header_ptr->prev_free = RT_NULL;

                /* 余块头插到空闲链，供后续分配搜索。 */
                new_ptr->next_free = heap->free_list->next_free;
                new_ptr->prev_free = heap->free_list;
                heap->free_list->next_free->prev_free = new_ptr;
                heap->free_list->next_free            = new_ptr;
                LOG_D("new ptr: next_free 0x%08x, prev_free 0x%08x",
                      new_ptr->next_free,
                      new_ptr->prev_free);

                /* 已消耗请求长度以及新产生的块头开销。 */
                heap->available_size = heap->available_size -
                                       size -
                                       RT_MEMHEAP_SIZE;
                if (heap->pool_size - heap->available_size > heap->max_used_size)
                    heap->max_used_size = heap->pool_size - heap->available_size;
            }
            else
            {
                /* 不拆分时整块用户容量都从 available_size 中扣除。 */
                heap->available_size = heap->available_size - free_size;
                if (heap->pool_size - heap->available_size > heap->max_used_size)
                    heap->max_used_size = heap->pool_size - heap->available_size;

                /* 整块分配，同样必须从空闲链摘除。 */
                LOG_D("one block: block[0x%08x], next_free 0x%08x, prev_free 0x%08x",
                      header_ptr,
                      header_ptr->next_free,
                      header_ptr->prev_free);

                header_ptr->next_free->prev_free = header_ptr->prev_free;
                header_ptr->prev_free->next_free = header_ptr->next_free;
                header_ptr->next_free = RT_NULL;
                header_ptr->prev_free = RT_NULL;
            }

            /* 最后设置已用位；物理链关系保持不变。 */
            header_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED);

#ifdef RT_USING_MEMTRACE
            if (rt_thread_self())
                rt_memcpy(header_ptr->owner_thread_name, rt_thread_self()->parent.name, sizeof(header_ptr->owner_thread_name));
            else
                rt_memcpy(header_ptr->owner_thread_name, "NONE", sizeof(header_ptr->owner_thread_name));
#endif /* RT_USING_MEMTRACE */

            if (heap->locked == RT_FALSE)
            {
                /* 仅释放本函数实际取得的内部锁。 */
                rt_sem_release(&(heap->lock));
            }

            /* 隐藏块头，只返回紧随其后的对齐用户区。 */
            LOG_D("alloc mem: memory[0x%08x], heap[0x%08x], size: %d",
                  (void *)((rt_uint8_t *)header_ptr + RT_MEMHEAP_SIZE),
                  header_ptr,
                  size);

            return (void *)((rt_uint8_t *)header_ptr + RT_MEMHEAP_SIZE);
        }

        if (heap->locked == RT_FALSE)
        {
            /* 搜索失败也必须释放内部锁。 */
            rt_sem_release(&(heap->lock));
        }
    }

    LOG_D("allocate memory: failed");

    /* 容量不足或没有连续空闲块。 */
    return RT_NULL;
}
RTM_EXPORT(rt_memheap_alloc);

/**
 * @brief 调整 memheap 块大小，优先尝试原地完成。
 *
 * @param heap 目标 memheap；正常情况下应与旧块块头中的 pool_ptr 相同。
 *
 * @param ptr 原用户指针；RT_NULL 等价于调用 rt_memheap_alloc()。
 *
 * @param newsize 新用户区长度；0 会释放原块并返回 RT_NULL。
 *
 * @return 成功时返回原地址或新地址；扩大失败时返回 RT_NULL，原块仍有效。
 *
 * 扩大顺序为：先检查右侧相邻块，若它空闲且合并后还能留下合法余块，则移动
 * 右侧空闲块头、原地扩大；否则解锁后执行“新分配—复制—释放”。缩小时，
 * 只有尾部足以容纳新块头和最小空闲区才拆分，并会与原右侧空闲块合并。
 */
void *rt_memheap_realloc(struct rt_memheap *heap, void *ptr, rt_size_t newsize)
{
    rt_err_t result;
    rt_size_t oldsize;
    struct rt_memheap_item *header_ptr;
    struct rt_memheap_item *new_ptr;

    RT_ASSERT(heap);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

    if (newsize == 0)
    {
        rt_memheap_free(ptr);

        return RT_NULL;
    }
    /* 与普通分配使用相同的对齐和最小长度规则。 */
    newsize = RT_ALIGN(newsize, RT_ALIGN_SIZE);
    if (newsize < RT_MEMHEAP_MINIALLOC)
        newsize = RT_MEMHEAP_MINIALLOC;

    if (ptr == RT_NULL)
    {
        return rt_memheap_alloc(heap, newsize);
    }

    /* 用户区前固定是块头，通过物理后继计算旧净容量。 */
    header_ptr = (struct rt_memheap_item *)
                 ((rt_uint8_t *)ptr - RT_MEMHEAP_SIZE);
    oldsize = MEMITEM_SIZE(header_ptr);
    /* 扩大路径首先尝试使用紧邻的右侧空闲块。 */
    if (newsize > oldsize)
    {
        void *new_ptr;
        volatile struct rt_memheap_item *next_ptr;

        if (heap->locked == RT_FALSE)
        {
            /* 原地修改两条链之前取得内部锁。 */
            result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
            if (result != RT_EOK)
            {
                rt_set_errno(result);
                return RT_NULL;
            }
        }

        next_ptr = header_ptr->next;

        /* 用户块不可能是零长度尾哨兵，其物理后继地址必须更高。 */
        RT_ASSERT(next_ptr > header_ptr);

        /* 只有直接右邻空闲时，扩大才有可能在原地址完成。 */
        if (!RT_MEMHEAP_IS_USED(next_ptr))
        {
            rt_int32_t nextsize;

            nextsize = MEMITEM_SIZE(next_ptr);
            RT_ASSERT(next_ptr > 0);

            /*
             * 下图表示可以移动右侧空闲块块头、从而避免另行分配和复制的情况，
             * |*| 表示块头：
             *
             *      旧用户区             右侧空闲块
             * |*|-----------|*|----------------------|*|
             *         扩大后的用户区       剩余区不少于最小块
             * |*|----------------|*|-----------------|*|
             */
            if (nextsize + oldsize > newsize + RT_MEMHEAP_MINIALLOC)
            {
                /* 仅新增用户字节从 available_size 扣除；块头数量保持不变。 */
                heap->available_size = heap->available_size - (newsize - oldsize);
                if (heap->pool_size - heap->available_size > heap->max_used_size)
                    heap->max_used_size = heap->pool_size - heap->available_size;

                /* 旧右邻块头即将移动，先从空闲链及物理链摘除。 */
                LOG_D("remove block: block[0x%08x], next_free 0x%08x, prev_free 0x%08x",
                      next_ptr,
                      next_ptr->next_free,
                      next_ptr->prev_free);

                _remove_next_ptr(next_ptr);

                /* 在扩大后的用户区末端重新构造余下空闲块的块头。 */
                next_ptr = (struct rt_memheap_item *)((char *)ptr + newsize);

                LOG_D("new free block: block[0x%08x] nextm[0x%08x] prevm[0x%08x]",
                      next_ptr,
                      next_ptr->next,
                      next_ptr->prev);

                /* 新位置仍代表空闲余量。 */
                next_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);

                /* 保留所属 memheap，供后续释放和一致性检查。 */
                next_ptr->pool_ptr = heap;

#ifdef RT_USING_MEMTRACE
                rt_memset((void *)next_ptr->owner_thread_name, ' ', sizeof(next_ptr->owner_thread_name));
#endif /* RT_USING_MEMTRACE */

                next_ptr->prev          = header_ptr;
                next_ptr->next          = header_ptr->next;
                header_ptr->next->prev = (struct rt_memheap_item *)next_ptr;
                header_ptr->next       = (struct rt_memheap_item *)next_ptr;

                /* 把移动后的余块重新插入空闲链。 */
                next_ptr->next_free = heap->free_list->next_free;
                next_ptr->prev_free = heap->free_list;
                heap->free_list->next_free->prev_free = (struct rt_memheap_item *)next_ptr;
                heap->free_list->next_free            = (struct rt_memheap_item *)next_ptr;
                LOG_D("new ptr: next_free 0x%08x, prev_free 0x%08x",
                      next_ptr->next_free,
                      next_ptr->prev_free);
                if (heap->locked == RT_FALSE)
                {
                    /* 原地扩大完成，释放本函数取得的内部锁。 */
                    rt_sem_release(&(heap->lock));
                }

                return ptr;
            }
        }

        if (heap->locked == RT_FALSE)
        {
            /* 无法原地扩大；先解锁，再调用公开分配 API，避免递归持锁。 */
            rt_sem_release(&(heap->lock));
        }

        /* 后备方案：分配新块，复制成功后才释放旧块。 */
        new_ptr = (void *)rt_memheap_alloc(heap, newsize);
        if (new_ptr != RT_NULL)
        {
            rt_memcpy(new_ptr, ptr, oldsize < newsize ? oldsize : newsize);
            rt_memheap_free(ptr);
        }

        return new_ptr;
    }

    /* 缩小后的尾部不足“块头 + 最小用户区”时保留整块，避免细碎空洞。 */
    if (newsize + RT_MEMHEAP_SIZE + RT_MEMHEAP_MINIALLOC >= oldsize)
        return ptr;

    if (heap->locked == RT_FALSE)
    {
        /* 缩小会修改物理链和空闲链，必须串行化。 */
        result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            rt_set_errno(result);

            return RT_NULL;
        }
    }

    /* 在缩小后的用户区末端创建空闲余块。 */
    new_ptr = (struct rt_memheap_item *)
              (((rt_uint8_t *)header_ptr) + newsize + RT_MEMHEAP_SIZE);

    LOG_D("split: block[0x%08x] nextm[0x%08x] prevm[0x%08x] to new[0x%08x]",
          header_ptr,
          header_ptr->next,
          header_ptr->prev,
          new_ptr);

    /* 新余块标记为空闲并记录所属堆。 */
    new_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    /* free/realloc 都通过该字段反查堆对象。 */
    new_ptr->pool_ptr = heap;

#ifdef RT_USING_MEMTRACE
    rt_memset(new_ptr->owner_thread_name, ' ', sizeof(new_ptr->owner_thread_name));
#endif /* RT_USING_MEMTRACE */

    /* 将余块插入物理块链。 */
    new_ptr->prev          = header_ptr;
    new_ptr->next          = header_ptr->next;
    header_ptr->next->prev = new_ptr;
    header_ptr->next       = new_ptr;

    /* 若原右邻也空闲，立即合并，维持“不存在连续空闲块”的不变量。 */
    if (!RT_MEMHEAP_IS_USED(new_ptr->next))
    {
        struct rt_memheap_item *free_ptr;

        /* new_ptr 吞并 free_ptr 的用户区和块头。 */
        free_ptr = new_ptr->next;
        heap->available_size = heap->available_size - MEMITEM_SIZE(free_ptr);

        LOG_D("merge: right node 0x%08x, next_free 0x%08x, prev_free 0x%08x",
              header_ptr, header_ptr->next_free, header_ptr->prev_free);

        free_ptr->next->prev = new_ptr;
        new_ptr->next   = free_ptr->next;

        /* 被吞并的旧空闲块头不再有效，从空闲链摘除。 */
        free_ptr->next_free->prev_free = free_ptr->prev_free;
        free_ptr->prev_free->next_free = free_ptr->next_free;
    }

    /* 将最终余块插入空闲链。 */
    new_ptr->next_free = heap->free_list->next_free;
    new_ptr->prev_free = heap->free_list;
    heap->free_list->next_free->prev_free = new_ptr;
    heap->free_list->next_free            = new_ptr;
    LOG_D("new free ptr: next_free 0x%08x, prev_free 0x%08x",
          new_ptr->next_free,
          new_ptr->prev_free);

    /* 按合并后的余块净容量更新可用字节。 */
    heap->available_size = heap->available_size + MEMITEM_SIZE(new_ptr);

    if (heap->locked == RT_FALSE)
    {
        /* 仅释放本函数取得的内部锁。 */
        rt_sem_release(&(heap->lock));
    }

    /* 缩小始终保留原用户地址。 */
    return ptr;
}
RTM_EXPORT(rt_memheap_realloc);

/**
 * @brief 释放一个 memheap 用户块，并与左右相邻空闲块合并。
 *
 * @param ptr 由 rt_memheap_alloc()/rt_memheap_realloc() 返回的地址；RT_NULL
 *            被忽略。
 *
 * 块头 magic 和后继块 magic 用于发现重复释放、非本分配器指针及部分越界写。
 * 这些检查不能替代完整内存安全验证。释放可能获取信号量，因此普通 memheap
 * 模式下只能在线程上下文调用；由外层锁保护的 `locked` 模式遵循外层约束。
 */
void rt_memheap_free(void *ptr)
{
    rt_err_t result;
    struct rt_memheap *heap;
    struct rt_memheap_item *header_ptr, *new_ptr;
    rt_bool_t insert_header;

    /* 与标准 free 类似，空指针不执行任何操作。 */
    if (ptr == RT_NULL) return;

    /* 默认需要把当前块头插入空闲链；若向左合并则已有前驱节点代表它。 */
    insert_header = RT_TRUE;
    new_ptr       = RT_NULL;
    header_ptr    = (struct rt_memheap_item *)
                    ((rt_uint8_t *)ptr - RT_MEMHEAP_SIZE);

    LOG_D("free memory: memory[0x%08x], block[0x%08x]",
          ptr, header_ptr);

    /* 验证当前块为已用，并检查后继块头魔数是否仍完整。 */
    if (header_ptr->magic != (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED) ||
       (header_ptr->next->magic & RT_MEMHEAP_MASK) != RT_MEMHEAP_MAGIC)
    {
        LOG_D("bad magic:0x%08x @ memheap",
              header_ptr->magic);
        RT_ASSERT(header_ptr->magic == (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED));
        /* 后继魔数损坏通常意味着用户写越过了本块边界。 */
        RT_ASSERT((header_ptr->next->magic & RT_MEMHEAP_MASK) == RT_MEMHEAP_MAGIC);
    }

    /* 从块头取得真实所属堆，而不是要求调用者另外传入。 */
    heap = header_ptr->pool_ptr;

    RT_ASSERT(heap);
    RT_ASSERT(rt_object_get_type(&heap->parent) == RT_Object_Class_MemHeap);

    if (heap->locked == RT_FALSE)
    {
        /* 两条链和统计字段必须原子更新。 */
        result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            rt_set_errno(result);

            return ;
        }
    }

    /* 清除已用位，当前块开始可回收。 */
    header_ptr->magic = (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED);
    /* 先计入当前块自己的净用户容量。 */
    heap->available_size += MEMITEM_SIZE(header_ptr);

    /* 向左合并时保留前驱块头，因为它已在空闲链中。 */
    if (!RT_MEMHEAP_IS_USED(header_ptr->prev))
    {
        LOG_D("merge: left node 0x%08x",
              header_ptr->prev);

        /* 合并后中间块头也转化为可用空间。 */
        heap->available_size += RT_MEMHEAP_SIZE;

        /* 在物理链中让前驱直接连接当前后继。 */
        (header_ptr->prev)->next = header_ptr->next;
        (header_ptr->next)->prev = header_ptr->prev;

        /* 后续向右合并以合并后的前驱为基准。 */
        header_ptr = header_ptr->prev;
        /* 前驱本来就在空闲链，无需再次插入。 */
        insert_header = RT_FALSE;
    }

    /* 再检查右邻，可能形成三块一次合并。 */
    if (!RT_MEMHEAP_IS_USED(header_ptr->next))
    {
        /* 右邻块头开销在合并后也成为可用空间。 */
        heap->available_size += RT_MEMHEAP_SIZE;

        /* header_ptr 跨过右邻 new_ptr，连接到其后继。 */
        new_ptr = header_ptr->next;

        LOG_D("merge: right node 0x%08x, next_free 0x%08x, prev_free 0x%08x",
              new_ptr, new_ptr->next_free, new_ptr->prev_free);

        new_ptr->next->prev = header_ptr;
        header_ptr->next    = new_ptr->next;

        /* 右邻块头消失，必须同步从空闲链移除。 */
        new_ptr->next_free->prev_free = new_ptr->prev_free;
        new_ptr->prev_free->next_free = new_ptr->next_free;
    }

    if (insert_header)
    {
        struct rt_memheap_item *n = heap->free_list->next_free;
#if defined(RT_MEMHEAP_BEST_MODE)
        rt_size_t blk_size = MEMITEM_SIZE(header_ptr);
        for (;n != heap->free_list; n = n->next_free)
        {
            rt_size_t m = MEMITEM_SIZE(n);
            if (blk_size <= m)
            {
                break;
            }
        }
#endif
        /* 未向左合并时，当前块尚不在空闲链，需要按配置顺序插入。 */
        header_ptr->next_free = n;
        header_ptr->prev_free = n->prev_free;
        n->prev_free->next_free = header_ptr;
        n->prev_free = header_ptr;

        LOG_D("insert to free list: next_free 0x%08x, prev_free 0x%08x",
              header_ptr->next_free, header_ptr->prev_free);
    }

#ifdef RT_USING_MEMTRACE
    rt_memset(header_ptr->owner_thread_name, ' ', sizeof(header_ptr->owner_thread_name));
#endif /* RT_USING_MEMTRACE */

    if (heap->locked == RT_FALSE)
    {
        /* 链与统计恢复一致后释放内部锁。 */
        rt_sem_release(&(heap->lock));
    }
}
RTM_EXPORT(rt_memheap_free);

/**
 * @brief 读取 memheap 的容量、当前用量和历史峰值。
 *
 * @param heap 要查询的 memheap。
 * @param total 可选输出，返回对齐后的池总长度，其中包含块头开销。
 * @param used 可选输出，返回 `pool_size - available_size`，因此同样包含当前
 *             已消耗的管理开销。
 * @param max_used 可选输出，返回自初始化以来 used 的最大值。
 *
 * 为获得相互一致的快照，普通模式会获取内部信号量；任一输出指针均可为
 * RT_NULL。若信号量获取失败，函数设置 errno 并保持输出不变。
 */
void rt_memheap_info(struct rt_memheap *heap,
                     rt_size_t *total,
                     rt_size_t *used,
                     rt_size_t *max_used)
{
    rt_err_t result;

    if (heap->locked == RT_FALSE)
    {
        /* 读取多个相关统计值时也需要锁，避免观察到拆分过程的中间状态。 */
        result = rt_sem_take(&(heap->lock), RT_WAITING_FOREVER);
        if (result != RT_EOK)
        {
            rt_set_errno(result);
            return;
        }
    }

    if (total != RT_NULL)
        *total = heap->pool_size;

    if (used  != RT_NULL)
        *used = heap->pool_size - heap->available_size;

    if (max_used != RT_NULL)
        *max_used = heap->max_used_size;

    if (heap->locked == RT_FALSE)
    {
        /* 查询完成后释放本函数取得的内部锁。 */
        rt_sem_release(&(heap->lock));
    }
}

#ifdef RT_USING_MEMHEAP_AS_HEAP
/*
 * 系统堆适配层的分配入口。
 *
 * 先尝试默认 system_heap；启用 RT_USING_MEMHEAP_AUTO_BINDING 后，失败时会
 * 遍历其他全局 memheap 继续尝试。遍历对象表的并发保护由上层系统堆使用
 * 场景约束，模块私有对象不会出现在该全局表中。
 */
void *_memheap_alloc(struct rt_memheap *heap, rt_size_t size)
{
    void *ptr;

    /* 默认堆具有最高优先级。 */
    ptr = rt_memheap_alloc(heap, size);
#ifdef RT_USING_MEMHEAP_AUTO_BINDING
    if (ptr == RT_NULL)
    {
        struct rt_object *object;
        struct rt_list_node *node;
        struct rt_memheap *_heap;
        struct rt_object_information *information;

        /* 默认堆容量不足时，按对象表顺序尝试其他 memheap。 */
        information = rt_object_get_information(RT_Object_Class_MemHeap);
        RT_ASSERT(information != RT_NULL);
        for (node  = information->object_list.next;
             node != &(information->object_list);
             node  = node->next)
        {
            object = rt_list_entry(node, struct rt_object, list);
            _heap   = (struct rt_memheap *)object;

            /* 已经尝试过默认堆，避免重复调用。 */
            if (heap == _heap)
                continue;

            ptr = rt_memheap_alloc(_heap, size);
            if (ptr != RT_NULL)
                break;
        }
    }
#endif /* RT_USING_MEMHEAP_AUTO_BINDING */
    return ptr;
}

/* 系统堆适配层的释放入口；块头中的 pool_ptr 会选择真实所属 memheap。 */
void _memheap_free(void *rmem)
{
    rt_memheap_free(rmem);
}

/*
 * 系统堆适配层的 realloc 入口。
 *
 * 首先在原块所属 memheap 中调整；失败且新长度非零时，可借助自动绑定从其他
 * memheap 分配新块、复制数据并释放旧块。因此返回地址可能来自不同的堆。
 */
void *_memheap_realloc(struct rt_memheap *heap, void *rmem, rt_size_t newsize)
{
    void *new_ptr;
    struct rt_memheap_item *header_ptr;

    if (rmem == RT_NULL)
        return _memheap_alloc(heap, newsize);

    if (newsize == 0)
    {
        _memheap_free(rmem);
        return RT_NULL;
    }

    /* 从用户指针前方的块头取得原块及其所属堆。 */
    header_ptr = (struct rt_memheap_item *)
                 ((rt_uint8_t *)rmem - RT_MEMHEAP_SIZE);

    new_ptr = rt_memheap_realloc(header_ptr->pool_ptr, rmem, newsize);
    if (new_ptr == RT_NULL && newsize != 0)
    {
        /* 原堆无法满足扩大时，尝试系统适配层的跨堆分配。 */
        new_ptr = _memheap_alloc(heap, newsize);
        if (new_ptr != RT_NULL && rmem != RT_NULL)
        {
            rt_size_t oldsize;

            /* 只复制新旧长度较小者，避免读取或写入越界。 */
            oldsize = MEMITEM_SIZE(header_ptr);
            if (newsize > oldsize)
                rt_memcpy(new_ptr, rmem, oldsize);
            else
                rt_memcpy(new_ptr, rmem, newsize);

            _memheap_free(rmem);
        }
    }

    return new_ptr;
}
#endif

#ifdef RT_USING_MEMTRACE
static int memheapcheck(int argc, char *argv[])
{
    struct rt_object_information *info;
    struct rt_list_node *list;
    struct rt_memheap *heap;
    struct rt_list_node *node;
    struct rt_memheap_item *item;
    rt_bool_t has_bad = RT_FALSE;
    rt_base_t level;
    char *name;

    name = argc > 1 ? argv[1] : RT_NULL;
    level = rt_hw_interrupt_disable();
    info = rt_object_get_information(RT_Object_Class_MemHeap);
    list = &info->object_list;
    for (node = list->next; node != list; node = node->next)
    {
        heap = (struct rt_memheap *)rt_list_entry(node, struct rt_object, list);
        /* 若命令给出名称，只检查对应 memheap。 */
        if (name != RT_NULL && rt_strncmp(name, heap->parent.name, RT_NAME_MAX) != 0)
            continue;
        /* 沿物理块链逐项验证块头和双向链接不变量。 */
        for (item = heap->block_list; item->next != heap->block_list; item = item->next)
        {
            /* magic 高位必须正确，最低位只能表示空闲或已用。 */
            if (!((item->magic & (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED)) == (RT_MEMHEAP_MAGIC | RT_MEMHEAP_FREED) ||
                 (item->magic & (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED))  == (RT_MEMHEAP_MAGIC | RT_MEMHEAP_USED)))
            {
                has_bad = RT_TRUE;
                break;
            }
            /* 每个块都必须声明属于当前正在检查的堆。 */
            if (heap != item->pool_ptr)
            {
                has_bad = RT_TRUE;
                break;
            }
            /* 相邻指针必须落在池范围内并满足地址对齐。 */
            if (!((rt_uintptr_t)item->next <= (rt_uintptr_t)((rt_uintptr_t)heap->start_addr + heap->pool_size) &&
                  (rt_uintptr_t)item->prev >= (rt_uintptr_t)heap->start_addr) &&
                  (rt_uintptr_t)item->next == RT_ALIGN((rt_uintptr_t)item->next, RT_ALIGN_SIZE) &&
                  (rt_uintptr_t)item->prev == RT_ALIGN((rt_uintptr_t)item->prev, RT_ALIGN_SIZE))
            {
                has_bad = RT_TRUE;
                break;
            }
            /* 检查 next/prev 的互反关系，发现物理链断裂。 */
            if (item->next == item->next->prev)
            {
                has_bad = RT_TRUE;
                break;
            }
        }
    }
    rt_hw_interrupt_enable(level);
    if (has_bad)
    {
        rt_kprintf("Memory block wrong:\n");
        rt_kprintf("name: %s\n", heap->parent.name);
        rt_kprintf("item: 0x%p\n", item);
    }
    return 0;
}
MSH_CMD_EXPORT(memheapcheck, check memory for memheap);

static int memheaptrace(int argc, char *argv[])
{
    struct rt_object_information *info;
    struct rt_list_node *list;
    struct rt_memheap *mh;
    struct rt_list_node *node;
    char *name;

    name = argc > 1 ? argv[1] : RT_NULL;
    info = rt_object_get_information(RT_Object_Class_MemHeap);
    list = &info->object_list;
    for (node = list->next; node != list; node = node->next)
    {
        struct rt_memheap_item *header_ptr;
        long block_size;

        mh = (struct rt_memheap *)rt_list_entry(node, struct rt_object, list);
        /* 支持按对象名称过滤输出。 */
        if (name != RT_NULL && rt_strncmp(name, mh->parent.name, RT_NAME_MAX) != 0)
            continue;
        /* 输出整个堆的容量、空闲量和历史峰值。 */
        rt_kprintf("\nmemory heap address:\n");
        rt_kprintf("name    : %s\n", mh->parent.name);
        rt_kprintf("heap_ptr: 0x%p\n", mh->start_addr);
        rt_kprintf("free    : 0x%08x\n", mh->available_size);
        rt_kprintf("max_used: 0x%08x\n", mh->max_used_size);
        rt_kprintf("size    : 0x%08x\n", mh->pool_size);
        rt_kprintf("\n--memory used information --\n");
        /* 再按物理地址顺序输出每个块。 */
        for (header_ptr = mh->block_list;
             header_ptr->next != mh->block_list;
             header_ptr = header_ptr->next)
        {
            if ((header_ptr->magic & RT_MEMHEAP_MASK) != RT_MEMHEAP_MAGIC)
            {
                rt_kprintf("[0x%p - incorrect magic: 0x%08x\n",
                    header_ptr, header_ptr->magic);
                break;
            }
            /* 块长度由相邻块头地址之差计算。 */
            block_size = MEMITEM_SIZE(header_ptr);
            if (block_size < 0)
                break;

            rt_kprintf("[0x%p - ", header_ptr);
            if (block_size < 1024)
                rt_kprintf("%5d", block_size);
            else if (block_size < 1024 * 1024)
                rt_kprintf("%4dK", block_size / 1024);
            else if (block_size < 1024 * 1024 * 100)
                rt_kprintf("%2d.%dM", block_size / (1024 * 1024),  (block_size % (1024 * 1024) * 10) / (1024 * 1024));
            else
                rt_kprintf("%4dM", block_size / (1024 * 1024));
            /* 追踪模式保存固定宽度的分配线程名。 */
            rt_kprintf("] %c%c%c%c\n",
                header_ptr->owner_thread_name[0],
                header_ptr->owner_thread_name[1],
                header_ptr->owner_thread_name[2],
                header_ptr->owner_thread_name[3]);
        }
    }
    return 0;
}

#ifdef RT_USING_FINSH
#include <finsh.h>
MSH_CMD_EXPORT(memheaptrace, dump memory trace for memheap);
#endif /* RT_USING_FINSH */
#endif /* RT_USING_MEMTRACE */
#endif /* RT_USING_MEMHEAP */
