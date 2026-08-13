/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2008-7-12      Bernard      首个版本
 * 2010-06-09     Bernard      修复堆尾哨兵，并修正 rt_realloc 的内存检查
 * 2010-07-13     Bernard      修复 kuronca 发现的 RT_ALIGN 问题
 * 2010-10-14     Bernard      修复使用空指针调用 rt_realloc 的问题
 * 2017-07-14     armink       修复新长度为 0 时的 rt_realloc 行为
 * 2018-10-02     Bernard      增加 64 位支持
 */

/*
 * Copyright (c) 2001-2004 Swedish Institute of Computer Science.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Adam Dunkels <adam@sics.se>
 *         Simon Goldschmidt
 *
 */

/**
 * @file mem.c
 * @brief 面向较小连续内存区的顺序首次适配分配器。
 *
 * 本文件实现 RT-Thread 的 small-memory 分配算法。初始化时，调用者给出一段
 * 连续内存；分配器在这段内存的开头放置 `struct rt_small_mem` 管理对象，随后
 * 把剩余空间组织成一条“按地址递增”的物理块链。每个块前面都有
 * `struct rt_small_mem_item` 块头，`next` 和 `prev` 保存相对于 `heap_ptr` 的
 * 偏移，因此不需要为链表指针另行分配内存。
 *
 * 初学者可以把内存布局理解为：
 *
 * @code
 * [管理对象][块头 A][A 的用户区][块头 B][B 的用户区]...[结尾哨兵]
 * @endcode
 *
 * 分配采用从 `lfree` 开始的首次适配搜索：找到第一个足够大的空闲块，能
 * 安全留下“块头 + 最小用户区”时便拆分，否则整块交给调用者。释放时通过
 * `plug_holes()` 与相邻空闲块合并，以抑制外部碎片。`lfree` 始终指向地址
 * 最低的空闲块，因此后续搜索可以跳过其前方已知全部占用的区域。
 *
 * 本分配器自身不加锁；当它被选作系统堆时，`kservice.c` 的系统堆包装层
 * 负责互斥。直接调用 rt_smem_* API 的使用者也必须自行串行化并发访问。
 */

#include <rthw.h>
#include <rtthread.h>

#if defined (RT_USING_SMALL_MEM)

#define DBG_TAG           "kernel.mem"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

struct rt_small_mem_item
{
    rt_uintptr_t            pool_ptr;         /**< 所属分配器地址；最低位同时编码“已使用”状态。 */
    rt_size_t               next;             /**< 后一物理块块头相对 heap_ptr 的字节偏移。 */
    rt_size_t               prev;             /**< 前一物理块块头相对 heap_ptr 的字节偏移。 */
#ifdef RT_USING_MEMTRACE
#ifdef ARCH_CPU_64BIT
    rt_uint8_t              thread[8];       /**< 截断保存的分配线程名，仅用于内存追踪。 */
#else
    rt_uint8_t              thread[4];       /**< 截断保存的分配线程名，仅用于内存追踪。 */
#endif /* ARCH_CPU_64BIT */
#endif /* RT_USING_MEMTRACE */
};

/**
 * @brief small-memory 分配器的运行时管理对象。
 *
 * 该对象本身放在调用者提供区域的最前端。`parent` 提供统一的内存统计与
 * 内核对象头；其余字段描述真正可分配的块区域以及首次适配搜索起点。
 */
struct rt_small_mem
{
    struct rt_memory            parent;                 /**< 通用内存对象与 total/used/max 统计。 */
    rt_uint8_t                 *heap_ptr;               /**< 第一个块头的地址，也是偏移量计算基准。 */
    struct rt_small_mem_item   *heap_end;               /**< 永久标记为已用的结尾哨兵，阻止越界合并。 */
    struct rt_small_mem_item   *lfree;                  /**< 当前地址最低的空闲块；无空闲块时指向哨兵。 */
    rt_size_t                   mem_size_aligned;       /**< 对齐后可供用户块使用的总字节数。 */
};

/* 一个最小空闲块必须能容纳这些基础元数据等价的用户空间。 */
#define MIN_SIZE (sizeof(rt_uintptr_t) + sizeof(rt_size_t) + sizeof(rt_size_t))

/* 清除最低状态位；所有对象地址都满足至少 2 字节对齐。 */
#define MEM_MASK ((~(rt_size_t)0) - 1)

/*
 * `pool_ptr` 的最低位保存使用状态，其余位保存 `rt_small_mem` 地址。
 * 这种“带标签指针”节省了单独的状态字段，但要求分配器对象地址已对齐。
 */
#define MEM_USED(_mem)       ((((rt_uintptr_t)(_mem)) & MEM_MASK) | 0x1)
#define MEM_FREED(_mem)      ((((rt_uintptr_t)(_mem)) & MEM_MASK) | 0x0)
#define MEM_ISUSED(_mem)   \
                      (((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (~MEM_MASK))
#define MEM_POOL(_mem)     \
    ((struct rt_small_mem *)(((rt_uintptr_t)(((struct rt_small_mem_item *)(_mem))->pool_ptr)) & (MEM_MASK)))
#define MEM_SIZE(_heap, _mem)      \
    (((struct rt_small_mem_item *)(_mem))->next - ((rt_uintptr_t)(_mem) - \
    (rt_uintptr_t)((_heap)->heap_ptr)) - RT_ALIGN(sizeof(struct rt_small_mem_item), RT_ALIGN_SIZE))

#define MIN_SIZE_ALIGNED     RT_ALIGN(MIN_SIZE, RT_ALIGN_SIZE)
#define SIZEOF_STRUCT_MEM    RT_ALIGN(sizeof(struct rt_small_mem_item), RT_ALIGN_SIZE)

#ifdef RT_USING_MEMTRACE
/**
 * @brief 把分配者名称写入块头中的定长追踪字段。
 *
 * 名称过长时截断，过短时用空格补齐，因而该字段不保证以 NUL 结尾，只能按
 * 固定宽度显示。调用者必须已经独占分配器元数据。
 */
rt_inline void rt_smem_setname(struct rt_small_mem_item *mem, const char *name)
{
    int index;
    if (name == RT_NULL)
    {
        name = "";
    }

    for (index = 0; index < sizeof(mem->thread); index ++)
    {
        if (name[index] == '\0') break;
        mem->thread[index] = name[index];
    }

    for (; index < sizeof(mem->thread); index ++)
    {
        mem->thread[index] = ' ';
    }
}
#endif /* RT_USING_MEMTRACE */

/**
 * @brief 将刚释放的块与左右相邻空闲块合并。
 *
 * 物理块链按地址排列，所以只检查直接后继和直接前驱就能完成所有可能的
 * 合并。函数先向高地址合并，再向低地址合并，并同步维护 `lfree`。被吞并
 * 块的 `pool_ptr` 被清零，便于调试时识别过期块头。
 *
 * @param m   拥有该块的 small-memory 分配器。
 * @param mem 已标记为空闲、且位于有效块区中的块头。
 *
 * @note 本函数不加锁，调用者必须保证分配器元数据不会被并发修改。
 */
static void plug_holes(struct rt_small_mem *m, struct rt_small_mem_item *mem)
{
    struct rt_small_mem_item *nmem;
    struct rt_small_mem_item *pmem;

    RT_ASSERT((rt_uint8_t *)mem >= m->heap_ptr);
    RT_ASSERT((rt_uint8_t *)mem < (rt_uint8_t *)m->heap_end);

    /* 先检查高地址一侧，避免保留两个连续空闲块。 */
    nmem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next];
    if (mem != nmem && !MEM_ISUSED(nmem) &&
        (rt_uint8_t *)nmem != (rt_uint8_t *)m->heap_end)
    {
        /* 后继为空闲且不是结尾哨兵：让 mem 跨过 nmem，合并为一个大块。
         */
        if (m->lfree == nmem)
        {
            m->lfree = mem;
        }
        nmem->pool_ptr = 0;
        mem->next = nmem->next;
        ((struct rt_small_mem_item *)&m->heap_ptr[nmem->next])->prev = (rt_uint8_t *)mem - m->heap_ptr;
    }

    /* 再检查低地址一侧；若可合并，最终保留前驱 pmem 的块头。 */
    pmem = (struct rt_small_mem_item *)&m->heap_ptr[mem->prev];
    if (pmem != mem && !MEM_ISUSED(pmem))
    {
        /* 前驱为空闲：让 pmem 跨过 mem，并按需把 lfree 移到 pmem。 */
        if (m->lfree == mem)
        {
            m->lfree = pmem;
        }
        mem->pool_ptr = 0;
        pmem->next = mem->next;
        ((struct rt_small_mem_item *)&m->heap_ptr[mem->next])->prev = (rt_uint8_t *)pmem - m->heap_ptr;
    }
}

/**
 * @brief 在调用者提供的连续内存中建立 small-memory 分配器。
 *
 * @param name 内核对象名称，用于诊断和 FinSH 查询。
 *
 * @param begin_addr 原始内存区起始地址；函数会向上对齐实际管理对象和块区。
 *
 * @param size 从 @p begin_addr 开始的总字节数，包含管理对象和块头开销。
 *
 * @return 成功时返回嵌入的 `rt_memory` 对象；空间不足以放置管理对象、首块和
 *         结尾哨兵时返回 RT_NULL。
 *
 * 初始化后的块链只有一个大空闲块和一个已用结尾哨兵。对象由
 * rt_object_init() 注册，因此销毁时应调用 rt_smem_detach()，而不是释放
 * 这段调用者提供的内存。
 */
rt_smem_t rt_smem_init(const char    *name,
                     void          *begin_addr,
                     rt_size_t      size)
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *small_mem;
    rt_uintptr_t start_addr, begin_align, end_align, mem_size;

    small_mem = (struct rt_small_mem *)RT_ALIGN((rt_uintptr_t)begin_addr, RT_ALIGN_SIZE);
    start_addr = (rt_uintptr_t)small_mem + sizeof(*small_mem);
    begin_align = RT_ALIGN((rt_uintptr_t)start_addr, RT_ALIGN_SIZE);
    end_align   = RT_ALIGN_DOWN((rt_uintptr_t)begin_addr + size, RT_ALIGN_SIZE);

    /* 对齐边界，并确认至少能够放下两个块头（首块与结尾哨兵）。 */
    if ((end_align > (2 * SIZEOF_STRUCT_MEM)) &&
        ((end_align - 2 * SIZEOF_STRUCT_MEM) >= start_addr))
    {
        /* 扣除结尾所需块头后，计算对齐的用户块区域长度。 */
        mem_size = end_align - begin_align - 2 * SIZEOF_STRUCT_MEM;
    }
    else
    {
        rt_kprintf("mem init, error begin address 0x%x, and end address 0x%x\n",
                   (rt_uintptr_t)begin_addr, (rt_uintptr_t)begin_addr + size);

        return RT_NULL;
    }

    rt_memset(small_mem, 0, sizeof(*small_mem));
    /* 初始化通用对象头和统计信息；此时 used/max 均由清零得到。 */
    rt_object_init(&(small_mem->parent.parent), RT_Object_Class_Memory, name);
    small_mem->parent.algorithm = "small";
    small_mem->parent.address = begin_align;
    small_mem->parent.total = mem_size;
    small_mem->mem_size_aligned = mem_size;

    /* heap_ptr 是所有 next/prev 偏移的统一基准。 */
    small_mem->heap_ptr = (rt_uint8_t *)begin_align;

    LOG_D("mem init, heap begin address 0x%x, size %d",
            (rt_uintptr_t)small_mem->heap_ptr, small_mem->mem_size_aligned);

    /* 建立覆盖全部可用空间的第一个空闲块。 */
    mem        = (struct rt_small_mem_item *)small_mem->heap_ptr;
    mem->pool_ptr = MEM_FREED(small_mem);
    mem->next  = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
    mem->prev  = 0;
#ifdef RT_USING_MEMTRACE
    rt_smem_setname(mem, "INIT");
#endif /* RT_USING_MEMTRACE */

    /* 建立零长度、永久“已使用”的尾哨兵，阻止合并越过内存区末端。 */
    small_mem->heap_end        = (struct rt_small_mem_item *)&small_mem->heap_ptr[mem->next];
    small_mem->heap_end->pool_ptr = MEM_USED(small_mem);
    small_mem->heap_end->next  = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
    small_mem->heap_end->prev  = small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM;
#ifdef RT_USING_MEMTRACE
    rt_smem_setname(small_mem->heap_end, "INIT");
#endif /* RT_USING_MEMTRACE */

    /* 此时唯一的空闲块也自然是最低地址空闲块。 */
    small_mem->lfree = (struct rt_small_mem_item *)small_mem->heap_ptr;

    return &small_mem->parent;
}
RTM_EXPORT(rt_smem_init);

/**
 * @brief 从内核对象系统中注销一个静态 small-memory 分配器。
 *
 * @param m 由 rt_smem_init() 返回的对象。
 *
 * @return 始终返回 RT_EOK；参数和对象类型错误通过断言报告。
 *
 * @warning 本函数不会检查仍未释放的用户块，也不会释放调用者提供的内存；
 *          调用者必须先停止所有访问并自行管理底层区域生命周期。
 */
rt_err_t rt_smem_detach(rt_smem_t m)
{
    RT_ASSERT(m != RT_NULL);
    RT_ASSERT(rt_object_get_type(&m->parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&m->parent));

    rt_object_detach(&(m->parent));

    return RT_EOK;
}
RTM_EXPORT(rt_smem_detach);

/**
 * @addtogroup group_memory_management
 */

/**@{*/

/**
 * @brief 从指定 small-memory 分配器中分配至少 @p size 字节。
 *
 * @param m small-memory 分配器对象。
 *
 * @param size 请求的最小用户区字节数；0 直接返回 RT_NULL。
 *
 * @return 成功时返回对齐后的用户区首地址；找不到足够大的连续块时返回 RT_NULL。
 *
 * 搜索从 `lfree` 开始沿物理块链向高地址进行。若剩余空间足以容纳一个新块
 * 头和最小用户区，则拆出空闲余块；否则把整个候选块分配出去，从而避免制造
 * 永远无法使用的微小碎片。返回指针之后，调用者看不到其前方块头。
 */
void *rt_smem_alloc(rt_smem_t m, rt_size_t size)
{
    rt_size_t ptr, ptr2;
    struct rt_small_mem_item *mem, *mem2;
    struct rt_small_mem *small_mem;

    if (size == 0)
        return RT_NULL;

    RT_ASSERT(m != RT_NULL);
    RT_ASSERT(rt_object_get_type(&m->parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&m->parent));

    small_mem = (struct rt_small_mem *)m;
    /* 所有用户指针和后继块头都必须满足 RT_ALIGN_SIZE。 */
    size = RT_ALIGN(size, RT_ALIGN_SIZE);

    /* 即使请求很小，也扩大到可以在未来作为有效空闲块管理的最小长度。 */
    if (size < MIN_SIZE_ALIGNED)
        size = MIN_SIZE_ALIGNED;

    if (size > small_mem->mem_size_aligned)
    {
        LOG_D("no memory");

        return RT_NULL;
    }

    for (ptr = (rt_uint8_t *)small_mem->lfree - small_mem->heap_ptr;
         ptr <= small_mem->mem_size_aligned - size;
         ptr = ((struct rt_small_mem_item *)&small_mem->heap_ptr[ptr])->next)
    {
        mem = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr];

        if ((!MEM_ISUSED(mem)) && (mem->next - (ptr + SIZEOF_STRUCT_MEM)) >= size)
        {
            /* 当前块空闲且用户区足够大；表达式计算的是不含块头的净容量。 */

            if (mem->next - (ptr + SIZEOF_STRUCT_MEM) >=
                (size + SIZEOF_STRUCT_MEM + MIN_SIZE_ALIGNED))
            {
                /*
                 * 余量可以同时容纳新块头和最小用户区，因此拆分：前半块满足本次
                 * 请求，后半块 mem2 保持空闲。若只够放块头却没有可用数据，创建
                 * mem2 只会产生不可分配碎片，所以这里要求额外的 MIN_SIZE_ALIGNED。
                 */
                ptr2 = ptr + SIZEOF_STRUCT_MEM + size;

                /* 在已分配用户区之后原地构造余块块头。 */
                mem2       = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr2];
                mem2->pool_ptr = MEM_FREED(small_mem);
                mem2->next = mem->next;
                mem2->prev = ptr;
#ifdef RT_USING_MEMTRACE
                rt_smem_setname(mem2, "    ");
#endif /* RT_USING_MEMTRACE */

                /* 把余块插入物理块链，并修正原后继的反向偏移。 */
                mem->next = ptr2;

                if (mem2->next != small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM)
                {
                    ((struct rt_small_mem_item *)&small_mem->heap_ptr[mem2->next])->prev = ptr2;
                }
                small_mem->parent.used += (size + SIZEOF_STRUCT_MEM);
                if (small_mem->parent.max < small_mem->parent.used)
                    small_mem->parent.max = small_mem->parent.used;
            }
            else
            {
                /*
                 * 近似匹配或完全匹配：余量不足以形成有效新块，因而整块分配。
                 * 当前块后继必为已用块，否则释放路径的 plug_holes() 本应早已把
                 * 两个连续空闲块合并。
                 */
                small_mem->parent.used += mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr);
                if (small_mem->parent.max < small_mem->parent.used)
                    small_mem->parent.max = small_mem->parent.used;
            }
            /* 标记块为已用；同一个字段仍保留所属分配器地址。 */
            mem->pool_ptr = MEM_USED(small_mem);
#ifdef RT_USING_MEMTRACE
            if (rt_thread_self())
                rt_smem_setname(mem, rt_thread_self()->parent.name);
            else
                rt_smem_setname(mem, "NONE");
#endif /* RT_USING_MEMTRACE */

            if (mem == small_mem->lfree)
            {
                /* 原最低空闲块已被使用，向后寻找新的最低空闲块。 */
                while (MEM_ISUSED(small_mem->lfree) && small_mem->lfree != small_mem->heap_end)
                    small_mem->lfree = (struct rt_small_mem_item *)&small_mem->heap_ptr[small_mem->lfree->next];

                RT_ASSERT(((small_mem->lfree == small_mem->heap_end) || (!MEM_ISUSED(small_mem->lfree))));
            }
            RT_ASSERT((rt_uintptr_t)mem + SIZEOF_STRUCT_MEM + size <= (rt_uintptr_t)small_mem->heap_end);
            RT_ASSERT((rt_uintptr_t)((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM) % RT_ALIGN_SIZE == 0);
            RT_ASSERT((((rt_uintptr_t)mem) & (RT_ALIGN_SIZE - 1)) == 0);

            LOG_D("allocate memory at 0x%x, size: %d",
                    (rt_uintptr_t)((rt_uint8_t *)mem + SIZEOF_STRUCT_MEM),
                    (rt_uintptr_t)(mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr)));

            /* 跳过内部块头，只把用户区交给调用者。 */
            return (rt_uint8_t *)mem + SIZEOF_STRUCT_MEM;
        }
    }

    return RT_NULL;
}
RTM_EXPORT(rt_smem_alloc);

/**
 * @brief 调整已分配块的大小，并尽量保留原地址。
 *
 * @param m small-memory 分配器对象。
 *
 * @param rmem 由同一分配器返回的用户指针；RT_NULL 等价于新分配。
 *
 * @param newsize 期望的新用户区长度；对齐后为 0 时释放原块。
 *
 * @return 成功时返回调整后的地址；扩大且无法获得新块时返回 RT_NULL，此时
 *         原块仍保持有效、内容不变。
 *
 * 缩小时若尾部足够形成一个合法空闲块，函数会原地拆分并立即与邻块合并；
 * 扩大时当前实现不会直接吞并右侧空闲块，而是执行“另行分配—复制—释放”。
 */
void *rt_smem_realloc(rt_smem_t m, void *rmem, rt_size_t newsize)
{
    rt_size_t size;
    rt_size_t ptr, ptr2;
    struct rt_small_mem_item *mem, *mem2;
    struct rt_small_mem *small_mem;
    void *nmem;

    RT_ASSERT(m != RT_NULL);
    RT_ASSERT(rt_object_get_type(&m->parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&m->parent));

    small_mem = (struct rt_small_mem *)m;
    /* 与普通分配保持相同的地址和块长对齐规则。 */
    newsize = RT_ALIGN(newsize, RT_ALIGN_SIZE);
    if (newsize > small_mem->mem_size_aligned)
    {
        LOG_D("realloc: out of memory");

        return RT_NULL;
    }
    else if (newsize == 0)
    {
        rt_smem_free(rmem);
        return RT_NULL;
    }

    /* C realloc 兼容语义：空旧指针等价于 alloc。 */
    if (rmem == RT_NULL)
        return rt_smem_alloc(&small_mem->parent, newsize);

    RT_ASSERT((((rt_uintptr_t)rmem) & (RT_ALIGN_SIZE - 1)) == 0);
    RT_ASSERT((rt_uint8_t *)rmem >= (rt_uint8_t *)small_mem->heap_ptr);
    RT_ASSERT((rt_uint8_t *)rmem < (rt_uint8_t *)small_mem->heap_end);

    mem = (struct rt_small_mem_item *)((rt_uint8_t *)rmem - SIZEOF_STRUCT_MEM);

    /* 通过当前块头和后继偏移还原旧用户区长度。 */
    ptr = (rt_uint8_t *)mem - small_mem->heap_ptr;
    size = mem->next - ptr - SIZEOF_STRUCT_MEM;
    if (size == newsize)
    {
        /* 长度完全相同，不移动也不复制。 */
        return rmem;
    }

    if (newsize + SIZEOF_STRUCT_MEM + MIN_SIZE < size)
    {
        /* 原地缩小：从尾部拆出一个新的空闲块。 */
        small_mem->parent.used -= (size - newsize);

        ptr2 = ptr + SIZEOF_STRUCT_MEM + newsize;
        mem2 = (struct rt_small_mem_item *)&small_mem->heap_ptr[ptr2];
        mem2->pool_ptr = MEM_FREED(small_mem);
        mem2->next = mem->next;
        mem2->prev = ptr;
#ifdef RT_USING_MEMTRACE
        rt_smem_setname(mem2, "    ");
#endif /* RT_USING_MEMTRACE */
        mem->next = ptr2;
        if (mem2->next != small_mem->mem_size_aligned + SIZEOF_STRUCT_MEM)
        {
            ((struct rt_small_mem_item *)&small_mem->heap_ptr[mem2->next])->prev = ptr2;
        }

        if (mem2 < small_mem->lfree)
        {
            /* 新余块比原 lfree 更靠前，更新搜索起点。 */
            small_mem->lfree = mem2;
        }

        plug_holes(small_mem, mem2);

        return rmem;
    }

    /* 扩大或无法有效拆分时，申请新块并复制旧内容。 */
    nmem = rt_smem_alloc(&small_mem->parent, newsize);
    if (nmem != RT_NULL) /* 只有新分配成功后才释放旧块，保证失败安全。 */
    {
        rt_memcpy(nmem, rmem, size < newsize ? size : newsize);
        rt_smem_free(rmem);
    }

    return nmem;
}
RTM_EXPORT(rt_smem_realloc);

/**
 * @brief 释放一个 small-memory 用户块并合并相邻空闲空间。
 *
 * @param rmem 先前由 rt_smem_alloc()/rt_smem_realloc() 返回的地址；RT_NULL
 *             被静默忽略。
 *
 * 块头中的带标签所属指针让函数无需额外传入分配器对象。重复释放、跨分配器
 * 指针和越界指针均属于调用错误；调试构建会尽可能通过断言发现它们。
 */
void rt_smem_free(void *rmem)
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *small_mem;

    if (rmem == RT_NULL)
        return;

    RT_ASSERT((((rt_uintptr_t)rmem) & (RT_ALIGN_SIZE - 1)) == 0);

    /* 用户地址前方固定放置块头，先退回块头位置。 */
    mem = (struct rt_small_mem_item *)((rt_uint8_t *)rmem - SIZEOF_STRUCT_MEM);
    /* 从带标签指针恢复所属分配器，并验证该块当前确实为已用状态。 */
    small_mem = MEM_POOL(mem);
    RT_ASSERT(small_mem != RT_NULL);
    RT_ASSERT(MEM_ISUSED(mem));
    RT_ASSERT(rt_object_get_type(&small_mem->parent.parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&small_mem->parent.parent));
    RT_ASSERT((rt_uint8_t *)rmem >= (rt_uint8_t *)small_mem->heap_ptr &&
              (rt_uint8_t *)rmem < (rt_uint8_t *)small_mem->heap_end);
    RT_ASSERT(MEM_POOL(&small_mem->heap_ptr[mem->next]) == small_mem);

    LOG_D("release memory 0x%x, size: %d",
            (rt_uintptr_t)rmem,
            (rt_uintptr_t)(mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr)));

    /* 清除最低状态位，把该块转换为空闲状态。 */
    mem->pool_ptr = MEM_FREED(small_mem);
#ifdef RT_USING_MEMTRACE
    rt_smem_setname(mem, "    ");
#endif /* RT_USING_MEMTRACE */

    if (mem < small_mem->lfree)
    {
        /* 新释放块地址更低，因此成为下一次首次适配搜索起点。 */
        small_mem->lfree = mem;
    }

    small_mem->parent.used -= (mem->next - ((rt_uint8_t *)mem - small_mem->heap_ptr));

    /* 最后与可合并的直接邻块合并，恢复“不相邻空闲块”的不变量。 */
    plug_holes(small_mem, mem);
}
RTM_EXPORT(rt_smem_free);

#ifdef RT_USING_FINSH
#include <finsh.h>

#ifdef RT_USING_MEMTRACE
static int memcheck(int argc, char *argv[])
{
    int position;
    rt_base_t level;
    struct rt_small_mem_item *mem;
    struct rt_small_mem *m;
    struct rt_object_information *information;
    struct rt_list_node *node;
    struct rt_object *object;
    char *name;

    name = argc > 1 ? argv[1] : RT_NULL;
    level = rt_hw_interrupt_disable();
    /* 关闭本地中断，避免检查期间系统堆元数据发生变化。 */
    information = rt_object_get_information(RT_Object_Class_Memory);
    for (node = information->object_list.next;
         node != &(information->object_list);
         node  = node->next)
    {
        object = rt_list_entry(node, struct rt_object, list);
        /* 如果给出了名称，只检查匹配的内存对象。 */
        if (name != RT_NULL && rt_strncmp(name, object->name, RT_NAME_MAX) != 0)
        {
            continue;
        }
        /* 同一对象类还可能包含其他算法，只处理 algorithm == "small"。 */
        m = (struct rt_small_mem *)object;
        if(rt_strncmp(m->parent.algorithm, "small", RT_NAME_MAX) != 0)
        {
            continue;
        }

        /* 沿物理块链检查偏移范围和每个块记录的所属分配器。 */
        for (mem = (struct rt_small_mem_item *)m->heap_ptr; mem != m->heap_end; mem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next])
        {
            position = (rt_uintptr_t)mem - (rt_uintptr_t)m->heap_ptr;
            if (position < 0) goto __exit;
            if (position > (int)m->mem_size_aligned) goto __exit;
            if (MEM_POOL(mem) != m) goto __exit;
        }
    }
    rt_hw_interrupt_enable(level);

    return 0;
__exit:
    rt_kprintf("Memory block wrong:\n");
    rt_kprintf("   name: %s\n", m->parent.parent.name);
    rt_kprintf("address: 0x%08x\n", mem);
    rt_kprintf("   pool: 0x%04x\n", mem->pool_ptr);
    rt_kprintf("   size: %d\n", mem->next - position - SIZEOF_STRUCT_MEM);
    rt_hw_interrupt_enable(level);

    return 0;
}
MSH_CMD_EXPORT(memcheck, check memory data);

static int memtrace(int argc, char **argv)
{
    struct rt_small_mem_item *mem;
    struct rt_small_mem *m;
    struct rt_object_information *information;
    struct rt_list_node *node;
    struct rt_object *object;
    char *name;

    name = argc > 1 ? argv[1] : RT_NULL;
    /* 遍历全局 Memory 对象表，可通过可选名称筛选。 */
    information = rt_object_get_information(RT_Object_Class_Memory);
    for (node = information->object_list.next;
         node != &(information->object_list);
         node  = node->next)
    {
        object = rt_list_entry(node, struct rt_object, list);
        /* 跳过名称不匹配的对象。 */
        if (name != RT_NULL && rt_strncmp(name, object->name, RT_NAME_MAX) != 0)
        {
            continue;
        }
        /* 跳过不是 small-memory 算法的内存对象。 */
        m = (struct rt_small_mem *)object;
        if(rt_strncmp(m->parent.algorithm, "small", RT_NAME_MAX) != 0)
        {
            continue;
        }
        /* 先打印总体统计和关键指针，再逐块显示长度及分配者缩写。 */
        rt_kprintf("\nmemory heap address:\n");
        rt_kprintf("name    : %s\n", m->parent.parent.name);
        rt_kprintf("total   : %d\n", m->parent.total);
        rt_kprintf("used    : %d\n", m->parent.used);
        rt_kprintf("max_used: %d\n", m->parent.max);
        rt_kprintf("heap_ptr: 0x%08x\n", m->heap_ptr);
        rt_kprintf("lfree   : 0x%08x\n", m->lfree);
        rt_kprintf("heap_end: 0x%08x\n", m->heap_end);
        rt_kprintf("\n--memory item information --\n");
        for (mem = (struct rt_small_mem_item *)m->heap_ptr; mem != m->heap_end; mem = (struct rt_small_mem_item *)&m->heap_ptr[mem->next])
        {
            int size = MEM_SIZE(m, mem);

            rt_kprintf("[0x%08x - ", mem);
            if (size < 1024)
                rt_kprintf("%5d", size);
            else if (size < 1024 * 1024)
                rt_kprintf("%4dK", size / 1024);
            else
                rt_kprintf("%4dM", size / (1024 * 1024));

            rt_kprintf("] %c%c%c%c", mem->thread[0], mem->thread[1], mem->thread[2], mem->thread[3]);
            if (MEM_POOL(mem) != m)
                rt_kprintf(": ***\n");
            else
                rt_kprintf("\n");
        }
    }
    return 0;
}
MSH_CMD_EXPORT(memtrace, dump memory trace information);
#endif /* RT_USING_MEMTRACE */
#endif /* RT_USING_FINSH */

#endif /* defined (RT_USING_SMALL_MEM) */

/**@}*/
