/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * 文件      : slab.c
 *
 * 修改记录：
 * 日期           作者         说明
 * 2008-07-12     Bernard      首个版本
 * 2010-07-13     Bernard      修复 kuronca 发现的 RT_ALIGN 问题
 * 2010-10-23     yi.qiu       增加模块内存分配器
 * 2010-12-18     yi.qiu       修复分区释放问题
 */

/*
 * KERN_SLABALLOC.C - Kernel SLAB memory allocator
 *
 * Copyright (c) 2003,2004 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_SLAB

#define DBG_TAG           "kernel.slab"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

/*
 * Slab 分配器实现概览
 *
 * 这套分配器把内存请求分成两条路径：
 *
 * - 小于 zone_limit 的请求：把大小向上归入 72 个规格之一，再从该规格的 zone
 *   （分区）中取一个固定大小的 chunk（块）。同一 zone 内所有 chunk 等大，所以
 *   分配和释放只需要移动索引或单链表指针；代价是向上取整形成的内部碎片。
 * - 大于等于 zone_limit 的请求：向上对齐到页，直接从底层页分配器取得连续页。
 *
 * 内存区从低地址到高地址大致为：
 *
 *   [struct rt_slab][对齐空隙][页管理区：memusage、zone、大块和空闲页……]
 *
 * 每个 zone 自身又是：
 *
 *   [rt_slab_zone 头部][为 chunk 对齐产生的空隙][chunk0][chunk1]...[chunkN]
 *
 * 初次使用的 chunk 不预建链表，而由 z_uindex 顺序取出；释放后的 chunk 才借用
 * 自身开头存放 c_next，组成 z_freechunk 单链表。memusage 则为每一页记录类型：
 * 大块首页记录页数，小块 zone 的每一页记录自己到 zone 首页的页偏移。释放任意
 * 小块时，借此可在 O(1) 时间反推出所属 zone。
 *
 * 规格分级如下（“步长”也是该范围内相邻规格之差）：
 *
 *   请求范围       向上取整步长       对应规格数
 *   1～127          8                  16
 *   128～255        16                 8
 *   256～511        32                 8
 *   512～1023       64                 8
 *   1024～2047      128                8
 *   2048～4095      256                8
 *   4096～8191      512                8
 *   8192～16383     1024               8
 *
 * 本文件的 rt_slab_alloc/free 等底层接口没有自行加锁。作为系统堆后端时，
 * kservice.c 的堆封装会按配置串行化访问；若直接调用这些接口，调用者也必须提供
 * 等价的互斥保护。它们不能在没有相应堆锁策略保证的并发环境中裸用。
 */

#define ZALLOC_SLAB_MAGIC       0x51ab51ab
#define ZALLOC_ZONE_LIMIT       (16 * 1024)     /* Slab 小块路径允许的最大上限 */
#define ZALLOC_MIN_ZONE_SIZE    (32 * 1024)     /* 单个 zone 的最小尺寸 */
#define ZALLOC_MAX_ZONE_SIZE    (128 * 1024)    /* 单个 zone 的最大尺寸 */
#define ZONE_RELEASE_THRESH     2               /* 最多缓存的全空闲 zone 数量 */

/*
 * 最小规格为 8 个字符单元；掩码用于把 zone 内第一个 chunk 的地址至少对齐到该边界。
 * 是否进入页分配路径由 zone_limit 判断，而不是仅由请求是否恰好为整页决定。
 */
#define MIN_CHUNK_SIZE      8       /* 以字符单元计 */
#define MIN_CHUNK_MASK      (MIN_CHUNK_SIZE - 1)

/*
 * memusage 表中的页面类型。FREE 表示页分配器可用；SMALL 表示属于某个 zone；
 * LARGE 表示一段直接按页分配的大块，其首页的 size 保存连续页数。
 */
#define PAGE_TYPE_FREE      0x00
#define PAGE_TYPE_SMALL     0x01
#define PAGE_TYPE_LARGE     0x02

#define btokup(addr)    \
    (&slab->memusage[((rt_uintptr_t)(addr) - slab->heap_start) >> RT_MM_PAGE_BITS])

/**
 * Slab 内存对象使用的内部数据结构。
 */

/*
 * zone 头部直接放在 zone 所占内存的最前面，不需要额外为元数据分配内存。
 */
struct rt_slab_zone
{
    rt_uint32_t  z_magic;                    /**< 魔数，用于发现错误指针或已回收 zone */
    rt_uint32_t  z_nfree;                    /**< 尚未用过与已经回收的 chunk 总数 */
    rt_uint32_t  z_nmax;                     /**< 本 zone 可容纳的 chunk 总数 */
    struct rt_slab_zone *z_next;            /**< 挂入同规格可分配链表或全空闲链表的指针 */
    rt_uint8_t  *z_baseptr;                 /**< 第 0 个 chunk 的起始地址 */

    rt_uint32_t  z_uindex;                   /**< 顺序分配区中最后一个已取出的 chunk 下标 */
    rt_uint32_t  z_chunksize;                /**< 此 zone 的固定 chunk 尺寸 */

    rt_uint32_t  z_zoneindex;                /**< 所属规格在 zone_array[] 中的下标 */
    struct rt_slab_chunk  *z_freechunk;     /**< 已释放 chunk 组成的单链表表头 */
};

/*
 * 空闲 chunk 的链表节点。节点不另占空间：释放后，chunk 开头暂时解释成此结构；
 * 下次分配出去后，这个字段即可重新由用户数据覆盖。
 */
struct rt_slab_chunk
{
    struct rt_slab_chunk *c_next;           /**< 下一个已释放 chunk */
};

/** 每页一项的反向索引；两个位域合计 32 位。 */
struct rt_slab_memusage
{
    rt_uint32_t     type: 2 ;               /**< PAGE_TYPE_* 页面类型 */
    rt_uint32_t     size: 30;               /**< 大块页数，或当前页距 zone 首页的页偏移 */
};

/*
 * 页分配器空闲链表中的节点。节点直接放在每段空闲连续页的第一页内。
 */
struct rt_slab_page
{
    struct rt_slab_page *next;      /**< 下一段按地址递增排列的空闲连续页 */
    rt_size_t page;                 /**< 本节点代表的连续页数 */

    /* 把结构补到整整一页，使 b + npages 能按“页数”进行指针运算。 */
    char dummy[RT_MM_PAGE_SIZE - (sizeof(struct rt_slab_page *) + sizeof(rt_size_t))];
};

#define RT_SLAB_NZONES                  72              /* 小块大小规格总数 */

/*
 * 一个完整 Slab 堆的控制块。parent 提供统一 rt_memory 接口，其余字段只供本算法使用。
 */
struct rt_slab
{
    struct rt_memory            parent;                         /**< 统一内存对象基类，必须位于首字段 */
    rt_uintptr_t                heap_start;                     /**< 页对齐后的可管理区起点 */
    rt_uintptr_t                heap_end;                       /**< 页对齐后的可管理区末尾（不含） */
    struct rt_slab_memusage    *memusage;                       /**< 按页索引的类型和归属信息表 */
    struct rt_slab_zone        *zone_array[RT_SLAB_NZONES];     /* 每种规格仍有空闲块的 zone 链表 */
    struct rt_slab_zone        *zone_free;                      /* 已完全空闲、暂留复用的 zone 链表 */
    rt_uint32_t                 zone_free_cnt;                  /**< zone_free 链表节点数 */
    rt_uint32_t                 zone_size;                      /**< 一个 zone 占用的总字符单元数 */
    rt_uint32_t                 zone_limit;                     /**< 小于此值走 zone，否则直接按页 */
    rt_uint32_t                 zone_page_cnt;                  /**< 一个 zone 占用的页数 */
    struct rt_slab_page        *page_list;                      /**< 按地址排序的空闲连续页链表 */
};

/**
 * @brief 从 Slab 的底层空闲页链表分配连续页。
 *
 * @param m Slab 内存对象。
 *
 * @param npages 所需连续页数；为 0 时直接失败。
 *
 * @return 成功时返回第一页地址；没有足够大的连续空闲段时返回 RT_NULL。
 *
 * @details 链表按地址保存空闲区，本函数采用首次适配：遇到更大的节点便从其低地址
 *          一端切出 npages 页，并把剩余部分的页头移到新起点；大小刚好则摘掉节点。
 * @warning 本函数不加锁，也不写 memusage；上层负责同步并标记页面用途。
 */
void *rt_slab_page_alloc(rt_slab_t m, rt_size_t npages)
{
    struct rt_slab_page *b, *n;
    struct rt_slab_page **prev;
    struct rt_slab *slab = (struct rt_slab *)m;

    if (npages == 0)
        return RT_NULL;

    for (prev = &slab->page_list; (b = *prev) != RT_NULL; prev = &(b->next))
    {
        if (b->page > npages)
        {
            /* 空闲段更大：低地址部分返回，高地址余段原地形成新节点。 */
            n       = b + npages;
            n->next = b->next;
            n->page = b->page - npages;
            *prev   = n;
            break;
        }

        if (b->page == npages)
        {
            /* 大小恰好匹配：整段返回，并从空闲链表中摘除。 */
            *prev = b->next;
            break;
        }
    }

    return b;
}

/**
 * @brief 把一段连续页归还到底层空闲页链表。
 *
 * @param m Slab 内存对象。
 *
 * @param addr 第一页的页对齐地址。
 *
 * @param npages 连续页数，必须大于 0。
 *
 * @details 在按地址排序的位置插入空闲段，并尽量与紧邻的前段、后段合并。这种
 *          相邻合并可恢复较大的连续区域，缓解外部碎片。断言用于捕获重叠或重复释放。
 * @warning 本函数不清空页面内容、不更新 memusage，也不自行加锁。
 */
void rt_slab_page_free(rt_slab_t m, void *addr, rt_size_t npages)
{
    struct rt_slab_page *b, *n;
    struct rt_slab_page **prev;
    struct rt_slab *slab = (struct rt_slab *)m;

    RT_ASSERT(addr != RT_NULL);
    RT_ASSERT((rt_uintptr_t)addr % RT_MM_PAGE_SIZE == 0);
    RT_ASSERT(npages != 0);

    n = (struct rt_slab_page *)addr;

    for (prev = &slab->page_list; (b = *prev) != RT_NULL; prev = &(b->next))
    {
        RT_ASSERT(b->page > 0);
        RT_ASSERT(b > n || b + b->page <= n);

        if (b + b->page == n)
        {
            if (b + (b->page += npages) == b->next)
            {
                b->page += b->next->page;
                b->next  = b->next->next;
            }
            return;
        }

        if (b == n + npages)
        {
            n->page = b->page + npages;
            n->next = b->next;
            *prev   = n;
            return;
        }

        if (b > n + npages)
            break;
    }

    n->page = npages;
    n->next = b;
    *prev   = n;
}

/*
 * 初始化页分配器：先置空链表，再把全部可用页作为一整段“释放”进去。
 */
static void rt_slab_page_init(struct rt_slab *slab, void *addr, rt_size_t npages)
{
    RT_ASSERT(addr != RT_NULL);
    RT_ASSERT(npages != 0);

    slab->page_list = RT_NULL;
    rt_slab_page_free((rt_slab_t)(&slab->parent), addr, npages);
}

/**
 * @brief 在调用者提供的内存区上初始化一个 Slab 内存对象。
 *
 * @param name 注册到内核对象系统中的名称。
 *
 * @param begin_addr 原始内存区起始地址；控制块也放在这段内存的前部。
 *
 * @param size 原始内存区总长度。
 *
 * @return 成功时返回统一 rt_memory 基类指针；对齐后没有完整页面时返回 RT_NULL。
 *
 * @details 初始化步骤为：放置并清零控制块；裁掉两端不足一页的部分；初始化对象与
 *          空闲页链表；根据堆总量在 32～128 KiB 之间选择 zone 尺寸；最后从页池
 *          分配 memusage 表。memusage 本身占用的页不再出现在空闲页链表中。
 * @note zone_size 会随总堆大小调节，小堆使用较小 zone，避免一个规格占用过多页。
 * @warning 本函数当前没有检查 memusage 表分配失败；调用者应提供足够大的内存区。
 */
rt_slab_t rt_slab_init(const char *name, void *begin_addr, rt_size_t size)
{
    rt_uint32_t limsize, npages;
    rt_uintptr_t start_addr, begin_align, end_align;
    struct rt_slab *slab;

    slab = (struct rt_slab *)RT_ALIGN((rt_uintptr_t)begin_addr, RT_ALIGN_SIZE);
    start_addr = (rt_uintptr_t)slab + sizeof(*slab);
    /* 控制块之后的起点向上按页对齐，原始末尾向下按页对齐。 */
    begin_align = RT_ALIGN((rt_uintptr_t)start_addr, RT_MM_PAGE_SIZE);
    end_align   = RT_ALIGN_DOWN((rt_uintptr_t)begin_addr + size, RT_MM_PAGE_SIZE);
    if (begin_align >= end_align)
    {
        rt_kprintf("slab init errr. wrong address[0x%x - 0x%x]\n",
                   (rt_uintptr_t)begin_addr, (rt_uintptr_t)begin_addr + size);
        return RT_NULL;
    }

    limsize = end_align - begin_align;
    npages  = limsize / RT_MM_PAGE_SIZE;
    LOG_D("heap[0x%x - 0x%x], size 0x%x, 0x%x pages",
          begin_align, end_align, limsize, npages);

    rt_memset(slab, 0, sizeof(*slab));
    /* 初始化统一内存对象基类及统计字段。 */
    rt_object_init(&(slab->parent.parent), RT_Object_Class_Memory, name);
    slab->parent.algorithm = "slab";
    slab->parent.address = begin_align;
    slab->parent.total = limsize;
    slab->parent.used = 0;
    slab->parent.max = 0;
    slab->heap_start = begin_align;
    slab->heap_end = end_align;

    /* 初始时，整个页对齐区都是一段连续空闲页。 */
    rt_slab_page_init(slab, (void *)slab->heap_start, npages);

    /* 依据堆容量计算 zone 尺寸，并由此得到小块/大块分界。 */
    slab->zone_size = ZALLOC_MIN_ZONE_SIZE;
    while (slab->zone_size < ZALLOC_MAX_ZONE_SIZE && (slab->zone_size << 1) < (limsize / 1024))
        slab->zone_size <<= 1;

    slab->zone_limit = slab->zone_size / 4;
    if (slab->zone_limit > ZALLOC_ZONE_LIMIT)
        slab->zone_limit = ZALLOC_ZONE_LIMIT;

    slab->zone_page_cnt = slab->zone_size / RT_MM_PAGE_SIZE;

    LOG_D("zone size 0x%x, zone page count 0x%x",
          slab->zone_size, slab->zone_page_cnt);

    /* 为每个页面分配一项 memusage，并把表长度向上补齐到整页。 */
    limsize  = npages * sizeof(struct rt_slab_memusage);
    limsize  = RT_ALIGN(limsize, RT_MM_PAGE_SIZE);
    slab->memusage = rt_slab_page_alloc((rt_slab_t)(&slab->parent), limsize / RT_MM_PAGE_SIZE);

    LOG_D("slab->memusage 0x%x, size 0x%x",
          (rt_uintptr_t)slab->memusage, limsize);
    return &slab->parent;
}
RTM_EXPORT(rt_slab_init);

/**
 * @brief 从内核对象系统中分离一个静态 Slab 内存对象。
 *
 * @param m 由 rt_slab_init() 初始化的 Slab 内存对象。
 *
 * @return 始终返回 RT_EOK；非法对象会触发断言。
 *
 * @warning 该操作只注销对象，不遍历或释放尚存分配，也不会释放调用者提供的原始
 *          内存区。分离后，调用者必须确保没有线程继续使用此堆。
 */
rt_err_t rt_slab_detach(rt_slab_t m)
{
    struct rt_slab *slab = (struct rt_slab *)m;

    RT_ASSERT(slab != RT_NULL);
    RT_ASSERT(rt_object_get_type(&slab->parent.parent) == RT_Object_Class_Memory);
    RT_ASSERT(rt_object_is_systemobject(&slab->parent.parent));

    rt_object_detach(&(slab->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_slab_detach);

/*
 * 根据请求尺寸计算规格下标，同时把 *bytes 向上改写为该规格的真实 chunk 尺寸。
 * 分段公式把 8～16384 映射到连续的 0～71。调用者必须先排除 0 和大块请求。
 */
rt_inline int zoneindex(rt_size_t *bytes)
{
    /* 使用无符号整数，便于编译器优化位运算和除以 2 的幂。 */
    rt_uintptr_t n = (rt_uintptr_t)(*bytes);

    if (n < 128)
    {
        *bytes = n = (n + 7) & ~7;

        /* 8 为步长，共 16 个规格，对应下标 0～15。 */
        return (n / 8 - 1);
    }
    if (n < 256)
    {
        *bytes = n = (n + 15) & ~15;

        return (n / 16 + 7);
    }
    if (n < 8192)
    {
        if (n < 512)
        {
            *bytes = n = (n + 31) & ~31;

            return (n / 32 + 15);
        }
        if (n < 1024)
        {
            *bytes = n = (n + 63) & ~63;

            return (n / 64 + 23);
        }
        if (n < 2048)
        {
            *bytes = n = (n + 127) & ~127;

            return (n / 128 + 31);
        }
        if (n < 4096)
        {
            *bytes = n = (n + 255) & ~255;

            return (n / 256 + 39);
        }
        *bytes = n = (n + 511) & ~511;

        return (n / 512 + 47);
    }
    if (n < 16384)
    {
        *bytes = n = (n + 1023) & ~1023;

        return (n / 1024 + 55);
    }

    rt_kprintf("Unexpected byte count %d", n);

    return 0;
}

/**
 * @addtogroup group_memory_management
 */

/**@{*/

/**
 * @brief 从 Slab 内存对象分配一块内存。
 *
 * @note size 为 0、没有足够连续页或没有可用 zone 时返回 RT_NULL。rt_size_t 是
 *       无符号类型，因此不存在“负长度”这一合法输入。
 *
 * @param m 已初始化的 Slab 内存对象。
 *
 * @param size 所需长度；小块请求会被向上取整到对应规格。
 *
 * @return 成功时返回满足对齐要求的内存；失败时返回 RT_NULL。
 *
 * @details 大块直接取连续页并在 memusage 中记录页数。小块先看相应规格链表：
 *          优先使用从未分配过的顺序区域，耗尽后从释放链表取块；没有现成 zone 时，
 *          先复用全空闲 zone，否则向页分配器申请新 zone。
 * @warning 返回内容未初始化。接口本身不加锁；地址必须最终交回同一个 @p m。
 */
void *rt_slab_alloc(rt_slab_t m, rt_size_t size)
{
    struct rt_slab_zone *z;
    rt_int32_t zi;
    struct rt_slab_chunk *chunk;
    struct rt_slab_memusage *kup;
    struct rt_slab *slab = (struct rt_slab *)m;

    /* 本实现明确把零长度请求视为失败。 */
    if (size == 0)
        return RT_NULL;

    /*
     * 大块不再细分为 chunk，直接按整页申请。此类操作预期较少，首先保证能够分配
     * 任意较大尺寸，而不追求小块路径那样的常数时间性能。
     */
    if (size >= slab->zone_limit)
    {
        size = RT_ALIGN(size, RT_MM_PAGE_SIZE);

        chunk = rt_slab_page_alloc(m, size >> RT_MM_PAGE_BITS);
        if (chunk == RT_NULL)
            return RT_NULL;

        /* 大块首页记录 PAGE_TYPE_LARGE 和总页数，供 realloc/free 反查。 */
        kup = btokup(chunk);
        kup->type = PAGE_TYPE_LARGE;
        kup->size = size >> RT_MM_PAGE_BITS;

        LOG_D("alloc a large memory 0x%x, page cnt %d, kup %d",
              size,
              size >> RT_MM_PAGE_BITS,
              ((rt_uintptr_t)chunk - slab->heap_start) >> RT_MM_PAGE_BITS);
        /* 统计按实际占用的整页尺寸计算，并更新历史峰值。 */
        slab->parent.used += size;
        if (slab->parent.used > slab->parent.max)
            slab->parent.max = slab->parent.used;
        return chunk;
    }

    /*
     * 小块先把请求归入固定规格。zone_array[zi] 只保存 z_nfree > 0 的 zone；链表
     * 首节点即可满足请求，因而无需扫描。当前实现先消耗从未用过的顺序区域，随后
     * 才从 z_freechunk 取回收块。
     */
    zi = zoneindex(&size);
    RT_ASSERT(zi < RT_SLAB_NZONES);

    LOG_D("try to alloc 0x%x on zone: %d", size, zi);

    if ((z = slab->zone_array[zi]) != RT_NULL)
    {
        RT_ASSERT(z->z_nfree > 0);

        /* 预先扣减空闲数；降到 0 时从可分配链表摘除，避免下次再次选中满 zone。 */
        if (--z->z_nfree == 0)
        {
            slab->zone_array[zi] = z->z_next;
            z->z_next = RT_NULL;
        }

        /*
         * z_uindex 尚未到最后下标时，还有从未交给用户的连续 chunk，直接递增下标
         * 即可取得；全部顺序 chunk 都用过后，剩余空闲数一定来自 z_freechunk。
         */
        if (z->z_uindex + 1 != z->z_nmax)
        {
            z->z_uindex = z->z_uindex + 1;
            chunk = (struct rt_slab_chunk *)(z->z_baseptr + z->z_uindex * size);
        }
        else
        {
            /* 顺序区域耗尽，从已释放 chunk 链表头取一块。 */
            chunk = z->z_freechunk;

            /* 链表头后移；chunk 内的 c_next 随即重新成为用户可用内容。 */
            z->z_freechunk = z->z_freechunk->c_next;
        }
        /* 小块统计按所属规格的真实 chunk 尺寸计算。 */
        slab->parent.used += z->z_chunksize;
        if (slab->parent.used > slab->parent.max)
            slab->parent.max = slab->parent.used;

        return chunk;
    }

    /*
     * 当前规格没有可用 zone：优先复用全空闲 zone；若缓存为空，再从底层页池取得
     * zone_size 大小的连续页。对 2 的幂大小，首 chunk 也按该大小对齐；其他规格
     * 至少按 MIN_CHUNK_SIZE 对齐。
     */
    {
        rt_uint32_t off;

        if ((z = slab->zone_free) != RT_NULL)
        {
            /* 从全空闲 zone 缓存表头取一个，避免重新操作页链表。 */
            slab->zone_free = z->z_next;
            -- slab->zone_free_cnt;
        }
        else
        {
            /* 缓存不足，向页分配器申请一个完整 zone。 */
            z = rt_slab_page_alloc(m, slab->zone_size / RT_MM_PAGE_SIZE);
            if (z == RT_NULL)
            {
                return RT_NULL;
            }

            LOG_D("alloc a new zone: 0x%x",
                  (rt_uintptr_t)z);

            /* 每页记录到 zone 首页的偏移，释放任意 chunk 时即可反推 zone。 */
            for (off = 0, kup = btokup(z); off < slab->zone_page_cnt; off ++)
            {
                kup->type = PAGE_TYPE_SMALL;
                kup->size = off;

                kup ++;
            }
        }

        /* 清空旧头部，防止复用 zone 时继承原链表和计数。 */
        rt_memset(z, 0, sizeof(struct rt_slab_zone));

        /* chunk 数组从 zone 头部之后开始，下面还会调整对齐。 */
        off = sizeof(struct rt_slab_zone);

        /*
         * 若 chunk 尺寸为 2 的幂，则让首 chunk 按同样尺寸对齐；否则至少按 8 对齐。
         */
        if ((size | (size - 1)) + 1 == (size << 1))
            off = (off + size - 1) & ~(size - 1);
        else
            off = (off + MIN_CHUNK_MASK) & ~MIN_CHUNK_MASK;

        z->z_magic     = ZALLOC_SLAB_MAGIC;
        z->z_zoneindex = zi;
        z->z_nmax      = (slab->zone_size - off) / size;
        z->z_nfree     = z->z_nmax - 1;
        z->z_baseptr   = (rt_uint8_t *)z + off;
        z->z_uindex    = 0;
        z->z_chunksize = size;

        chunk = (struct rt_slab_chunk *)(z->z_baseptr + z->z_uindex * size);

        /* 新 zone 尚有空闲 chunk，挂到相应规格链表头。 */
        z->z_next = slab->zone_array[zi];
        slab->zone_array[zi] = z;
        /* 第 0 个 chunk 已直接返回，所以立刻计入已用量。 */
        slab->parent.used += z->z_chunksize;
        if (slab->parent.used > slab->parent.max)
            slab->parent.max = slab->parent.used;
    }

    return chunk;
}
RTM_EXPORT(rt_slab_alloc);

/**
 * @brief 调整先前由同一 Slab 对象分配的内存块大小。
 *
 * @param m 原分配所属的 Slab 内存对象。
 *
 * @param ptr 原内存块；RT_NULL 等价于新分配。
 *
 * @param size 新请求长度；为 0 时释放原块并返回 RT_NULL。
 *
 * @return 成功时返回可容纳新尺寸的地址；失败时返回 RT_NULL，原块仍保持有效。
 *
 * @details 若原块是小块且新尺寸取整后仍属于同一规格，直接返回原指针。其他情况
 *          采用“先分配、复制较小长度、再释放旧块”，所以返回地址可能变化。
 * @warning @p ptr 必须是本 Slab 当前有效分配的起始地址，不能传块中间、已释放地址
 *          或其他堆的指针。本函数同样要求外部串行化。
 */
void *rt_slab_realloc(rt_slab_t m, void *ptr, rt_size_t size)
{
    void *nptr;
    struct rt_slab_zone *z;
    struct rt_slab_memusage *kup;
    struct rt_slab *slab = (struct rt_slab *)m;

    if (ptr == RT_NULL)
        return rt_slab_alloc(m, size);

    if (size == 0)
    {
        rt_slab_free(m, ptr);
        return RT_NULL;
    }

    /*
     * 先用 ptr 所在页查询 memusage。大块的 size 是页数；小块的 size 是从当前页
     * 回退到 zone 首页所需的页数。
     */
    kup = btokup((rt_uintptr_t)ptr & ~RT_MM_PAGE_MASK);
    if (kup->type == PAGE_TYPE_LARGE)
    {
        rt_size_t osize;

        osize = kup->size << RT_MM_PAGE_BITS;
        if ((nptr = rt_slab_alloc(m, size)) == RT_NULL)
            return RT_NULL;
        rt_memcpy(nptr, ptr, size > osize ? osize : size);
        rt_slab_free(m, ptr);

        return nptr;
    }
    else if (kup->type == PAGE_TYPE_SMALL)
    {
        z = (struct rt_slab_zone *)(((rt_uintptr_t)ptr & ~RT_MM_PAGE_MASK) -
                          kup->size * RT_MM_PAGE_SIZE);
        RT_ASSERT(z->z_magic == ZALLOC_SLAB_MAGIC);

        zoneindex(&size);
        if (z->z_chunksize == size)
        return (ptr); /* 取整后规格不变，原 chunk 已足够容纳。 */

        /*
         * zoneindex() 已把 size 改成新规格的真实尺寸。先申请新块，再只复制新旧容量
         * 中较小者；申请失败时不释放原块，符合 realloc 的常见失败语义。
         */
        if ((nptr = rt_slab_alloc(m, size)) == RT_NULL)
            return RT_NULL;

        rt_memcpy(nptr, ptr, size > z->z_chunksize ? z->z_chunksize : size);
        rt_slab_free(m, ptr);

        return nptr;
    }

    return RT_NULL;
}
RTM_EXPORT(rt_slab_realloc);

/**
 * @brief 释放先前由 rt_slab_alloc() 分配的内存块。
 *
 * @note 大块立即归还页分配器；小块先回所属 zone。全空闲 zone 会进入缓存，缓存
 *       超过阈值后才有一个 zone 真正拆回空闲页，因此释放后页数未必立刻增加。
 *
 * @param m 分配该内存的 Slab 内存对象。
 * @param ptr 待释放块的起始地址；RT_NULL 被安静忽略。
 *
 * @warning 不检查重复释放、块内地址或跨堆指针；这些错误可能破坏链表。释放后不得
 *          再读写原内存。本函数不清除用户数据，也不自行加锁。
 */
void rt_slab_free(rt_slab_t m, void *ptr)
{
    struct rt_slab_zone *z;
    struct rt_slab_chunk *chunk;
    struct rt_slab_memusage *kup;
    struct rt_slab *slab = (struct rt_slab *)m;

    /* 与常见 free 语义一致，释放空指针不做任何操作。 */
    if (ptr == RT_NULL)
        return ;

    /* 调试日志展示 ptr 所在页以及对应 memusage 下标。 */
#if (DBG_LVL == DBG_LOG)
    {
        rt_uintptr_t addr = ((rt_uintptr_t)ptr & ~RT_MM_PAGE_MASK);
        LOG_D("free a memory 0x%x and align to 0x%x, kup index %d",
              (rt_uintptr_t)ptr,
              (rt_uintptr_t)addr,
              ((rt_uintptr_t)(addr) - slab->heap_start) >> RT_MM_PAGE_BITS);
    }
#endif /* DBG_LVL == DBG_LOG */

    kup = btokup((rt_uintptr_t)ptr & ~RT_MM_PAGE_MASK);
    /* 大块：首页的 memusage.size 给出连续页数。 */
    if (kup->type == PAGE_TYPE_LARGE)
    {
        rt_uintptr_t size;

        /* 先清除页数标记，随后更新统计并把整段页归还。 */
        size = kup->size;
        kup->size = 0;
        /* 大块已用量按页对齐后的实际尺寸扣减。 */
        slab->parent.used -= size * RT_MM_PAGE_SIZE;

        LOG_D("free large memory block 0x%x, page count %d",
              (rt_uintptr_t)ptr, size);

        /* 插回有序空闲页链表，并尝试与相邻段合并。 */
        rt_slab_page_free(m, ptr, size);

        return;
    }

    /* 小块：利用当前页记录的偏移回退到所属 zone 首页。 */
    z = (struct rt_slab_zone *)(((rt_uintptr_t)ptr & ~RT_MM_PAGE_MASK) -
                      kup->size * RT_MM_PAGE_SIZE);
    RT_ASSERT(z->z_magic == ZALLOC_SLAB_MAGIC);

    chunk          = (struct rt_slab_chunk *)ptr;
    chunk->c_next  = z->z_freechunk;
    z->z_freechunk = chunk;
    /* chunk 已接到回收链表头，从已用量中扣除其规格尺寸。 */
    slab->parent.used -= z->z_chunksize;

    /*
     * 增加空闲块数。若旧值为 0，说明 zone 先前已满、不在 zone_array 中；现在有了
     * 第一个空闲块，必须重新挂回相应规格链表。
     */
    if (z->z_nfree++ == 0)
    {
        z->z_next = slab->zone_array[z->z_zoneindex];
        slab->zone_array[z->z_zoneindex] = z;
    }

    /*
     * 若本 zone 已全空，并且同规格链表里还有其他 zone 可供分配，就把它从规格链表
     * 移入全空闲缓存。保留至少一个本规格 zone，避免刚释放完又立即申请页造成抖动。
     */
    if (z->z_nfree == z->z_nmax &&
        (z->z_next || slab->zone_array[z->z_zoneindex] != z))
    {
        struct rt_slab_zone **pz;

        LOG_D("free zone %#x, zoneindex %d",
              (rt_uintptr_t)z, z->z_zoneindex);

        /* 在单链表中找到并摘除这个全空闲 zone。 */
        for (pz = &slab->zone_array[z->z_zoneindex]; z != *pz; pz = &(*pz)->z_next)
            ;
        *pz = z->z_next;

        /* 毁掉魔数，使意外继续按活动 zone 使用时更容易被断言发现。 */
        z->z_magic = RT_UINT32_MAX;

        /* 放入可被任意规格复用的全空闲 zone 缓存。 */
        z->z_next = slab->zone_free;
        slab->zone_free = z;

        ++ slab->zone_free_cnt;

        /* 缓存超过阈值时，取一个 zone 真正归还底层页分配器。 */
        if (slab->zone_free_cnt > ZONE_RELEASE_THRESH)
        {
            register rt_uint32_t i;

            z         = slab->zone_free;
            slab->zone_free = z->z_next;
            -- slab->zone_free_cnt;

            /* 页面不再属于 zone，清除每一页的类型和偏移记录。 */
            for (i = 0, kup = btokup(z); i < slab->zone_page_cnt; i ++)
            {
                kup->type = PAGE_TYPE_FREE;
                kup->size = 0;
                kup ++;
            }

            /* 把整段连续页插回并合并到空闲页链表。 */
            rt_slab_page_free(m, z, slab->zone_size / RT_MM_PAGE_SIZE);

            return;
        }
    }
}
RTM_EXPORT(rt_slab_free);

#endif /* RT_USING_SLAB */
