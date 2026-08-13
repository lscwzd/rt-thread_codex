/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2006-05-27     Bernard      实现内存池
 * 2006-06-03     Bernard      修复线程定时器初始化问题
 * 2006-06-30     Bernard      修复内存块分配/释放问题
 * 2006-08-04     Bernard      增加钩子支持
 * 2006-08-10     Bernard      修复 rt_mp_alloc 的中断上下文问题
 * 2010-07-13     Bernard      修复 kuronca 发现的 RT_ALIGN 问题
 * 2010-10-26     yi.qiu       为 rt_mp_delete 增加模块支持
 * 2011-01-24     Bernard      增加对象分配结果检查
 * 2012-03-22     Bernard      修复 rt_mp_init 和 rt_mp_create 的对齐问题
 * 2022-01-07     Gabriel      将 __on_rt_xxxxx_hook 移入本文件
 * 2023-09-15     xqyjlj       优化 rt_hw_interrupt_disable/enable
 * 2023-12-10     xqyjlj       修复自旋锁断言
 */

/**
 * @file mempool.c
 * @brief 固定大小内存块池，以及等待空闲块的线程同步机制。
 *
 * 内存池适合频繁申请、释放同一种大小对象的场景。与通用堆不同，它不需要
 * 搜索可变长度空闲块，也不会产生外部碎片。每个物理槽位由两部分组成：
 *
 * @code
 * [一个指针宽度的内部字段][block_size 字节的用户区]
 * @endcode
 *
 * 块空闲时，内部字段保存下一空闲块地址，从而构成单链表；块被占用时，该
 * 字段改存所属 `rt_mempool` 地址，因此 rt_mp_free() 只凭用户指针就能找到
 * 原内存池。空闲链和计数由 `spinlock` 保护，可从中断上下文执行不等待的
 * 分配及释放；只有“无块可用且允许等待”的路径必须处在线程上下文。
 *
 * 当池为空时，线程挂入 `suspend_thread`。释放者先把块放回空闲链，再唤醒
 * 一个等待者；被唤醒线程重新取得锁并在 while 条件中复查资源，因而可以
 * 正确处理竞争、超时和伪唤醒式的状态变化。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_MEMPOOL

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_mp_alloc_hook)(struct rt_mempool *mp, void *block);
static void (*rt_mp_free_hook)(struct rt_mempool *mp, void *block);

/**
 * @addtogroup group_hook
 */

/**@{*/

/**
 * @brief 安装固定块分配成功后的单监听者钩子。
 *
 * @param hook 新钩子；传入 RT_NULL 可禁用。
 *
 * 钩子在内存池自旋锁已经释放后同步调用，接收池对象和用户区地址。此时
 * 分配已成功，钩子可以做追踪，但不得释放该块、递归分配同一池，或保留超过
 * 内存池生命周期的指针。设置函数本身没有并发保护，通常在初始化阶段调用。
 */
void rt_mp_alloc_sethook(void (*hook)(struct rt_mempool *mp, void *block))
{
    rt_mp_alloc_hook = hook;
}

/**
 * @brief 安装固定块释放入口处的单监听者钩子。
 *
 * @param hook 新钩子；传入 RT_NULL 可禁用。
 *
 * rt_mp_free() 在取得内存池自旋锁、修改空闲链之前调用该钩子。因此它是释放
 * 请求的追踪点，而不是“块已经可再次分配”的完成通知；钩子继承调用者上下文，
 * 可能处于中断中，必须短小、不可阻塞，也不能递归操作同一块。
 */
void rt_mp_free_sethook(void (*hook)(struct rt_mempool *mp, void *block))
{
    rt_mp_free_hook = hook;
}

/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_memory_management
 */

/**@{*/

/**
 * @brief 使用调用者提供的控制块和存储区初始化静态内存池。
 *
 * @param mp 调用者持有的内存池控制块，其生命周期必须覆盖所有块的使用期。
 *
 * @param name 内核对象名称。
 *
 * @param start 用于切分固定块的连续存储区起点。
 *
 * @param size 存储区总字节数；尾部不足一个完整槽位的字节不会使用。
 *
 * @param block_size 每个用户区期望长度，函数会向上对齐。
 *
 * @return 初始化完成后返回 RT_EOK；非法参数由断言报告。
 *
 * 每个槽位额外消耗一个指针宽度。初始化循环把这些内部字段串成单向空闲链，
 * 最后一项指向 RT_NULL。该 API 使用 rt_object_init() 注册对象，后续应调用
 * rt_mp_detach()；底层 @p start 仍归调用者所有。
 */
rt_err_t rt_mp_init(struct rt_mempool *mp,
                    const char        *name,
                    void              *start,
                    rt_size_t          size,
                    rt_size_t          block_size)
{
    rt_uint8_t *block_ptr;
    rt_size_t offset;

    /* 这些参数决定后续的块地址计算，任何一个无效都可能破坏整条空闲链。 */
    RT_ASSERT(mp != RT_NULL);
    RT_ASSERT(name != RT_NULL);
    RT_ASSERT(start != RT_NULL);
    RT_ASSERT(size > 0 && block_size > 0);

    /* 以“静态初始化路径”注册内存池对象。 */
    rt_object_init(&(mp->parent), RT_Object_Class_MemPool, name);

    /* 丢弃末端未对齐字节，确保每个内部指针都满足访问对齐。 */
    mp->start_address = start;
    mp->size = RT_ALIGN_DOWN(size, RT_ALIGN_SIZE);

    /* 用户块长度也向上对齐，使相邻槽位的内部字段自然对齐。 */
    block_size = RT_ALIGN(block_size, RT_ALIGN_SIZE);
    mp->block_size = block_size;

    /* 一个槽位 = 用户区 + 一个内部指针；整数除法自动忽略尾部残余。 */
    mp->block_total_count = mp->size / (mp->block_size + sizeof(rt_uint8_t *));
    mp->block_free_count  = mp->block_total_count;

    /* 该列表保存因池空而阻塞的线程。 */
    rt_list_init(&(mp->suspend_thread));

    /* 在每个槽位首部写入下一槽位地址，原地构造空闲单链。 */
    block_ptr = (rt_uint8_t *)mp->start_address;
    for (offset = 0; offset < mp->block_total_count; offset ++)
    {
        *(rt_uint8_t **)(block_ptr + offset * (block_size + sizeof(rt_uint8_t *))) =
            (rt_uint8_t *)(block_ptr + (offset + 1) * (block_size + sizeof(rt_uint8_t *)));
    }

    *(rt_uint8_t **)(block_ptr + (offset - 1) * (block_size + sizeof(rt_uint8_t *))) =
        RT_NULL;

    mp->block_list = block_ptr;
    rt_spin_lock_init(&(mp->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mp_init);

/**
 * @brief 注销一个由 rt_mp_init() 初始化的内存池。
 *
 * @param mp 静态内存池对象。
 *
 * @return 返回 RT_EOK；类型或生命周期路径错误由断言报告。
 *
 * 所有等待者会以 RT_ERROR 被唤醒。函数不检查在外块，也不释放控制块或存储区；
 * 调用者必须先阻止新的访问，并保证所有已经取出的块不再使用。
 */
rt_err_t rt_mp_detach(struct rt_mempool *mp)
{
    rt_base_t level;

    /* 检查对象类型，避免把其他内核对象误当成内存池处理。 */
    RT_ASSERT(mp != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mp->parent) == RT_Object_Class_MemPool);
    RT_ASSERT(rt_object_is_systemobject(&mp->parent));

    level = rt_spin_lock_irqsave(&(mp->spinlock));
    /* 在同一把锁下改变等待队列，避免释放路径同时唤醒其中节点。 */
    rt_susp_list_resume_all(&mp->suspend_thread, RT_ERROR);

    /* 从对象表移除，但不回收调用者提供的任何内存。 */
    rt_object_detach(&(mp->parent));
    rt_spin_unlock_irqrestore(&(mp->spinlock), level);

    return RT_EOK;
}
RTM_EXPORT(rt_mp_detach);

#ifdef RT_USING_HEAP
/**
 * @brief 从系统堆动态创建控制块和固定块存储区。
 *
 * @param name 内核对象名称。
 *
 * @param block_count 固定块数量，必须大于 0。
 *
 * @param block_size 每块用户区大小，函数会向上对齐。
 *
 * @return 成功时返回动态内存池；任一堆分配失败时返回 RT_NULL，并回滚已创建对象。
 *
 * 该函数会访问系统堆，明确禁止中断上下文。返回对象必须由 rt_mp_delete()
 * 删除，不能使用 rt_mp_detach()。
 */
rt_mp_t rt_mp_create(const char *name,
                     rt_size_t   block_count,
                     rt_size_t   block_size)
{
    rt_uint8_t *block_ptr;
    struct rt_mempool *mp;
    rt_size_t offset;

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 动态创建和堆分配均要求线程上下文。 */
    RT_ASSERT(name != RT_NULL);
    RT_ASSERT(block_count > 0 && block_size > 0);

    /* 先从对象管理器分配控制块并注册为动态对象。 */
    mp = (struct rt_mempool *)rt_object_allocate(RT_Object_Class_MemPool, name);
    /* 控制块分配失败时没有其他资源需要回滚。 */
    if (mp == RT_NULL)
        return RT_NULL;

    /* 计算包括每块内部指针在内的完整存储区大小。 */
    block_size     = RT_ALIGN(block_size, RT_ALIGN_SIZE);
    mp->block_size = block_size;
    mp->size       = (block_size + sizeof(rt_uint8_t *)) * block_count;

    /* 再为所有固定槽位申请一整段连续存储。 */
    mp->start_address = rt_malloc((block_size + sizeof(rt_uint8_t *)) *
                                  block_count);
    if (mp->start_address == RT_NULL)
    {
        /* 第二步失败，注销并释放第一步创建的动态控制块。 */
        rt_object_delete(&(mp->parent));

        return RT_NULL;
    }

    mp->block_total_count = block_count;
    mp->block_free_count  = mp->block_total_count;

    /* 新池没有等待者。 */
    rt_list_init(&(mp->suspend_thread));

    /* 与静态初始化相同，在槽位首部原地串起空闲链。 */
    block_ptr = (rt_uint8_t *)mp->start_address;
    for (offset = 0; offset < mp->block_total_count; offset ++)
    {
        *(rt_uint8_t **)(block_ptr + offset * (block_size + sizeof(rt_uint8_t *)))
            = block_ptr + (offset + 1) * (block_size + sizeof(rt_uint8_t *));
    }

    *(rt_uint8_t **)(block_ptr + (offset - 1) * (block_size + sizeof(rt_uint8_t *)))
        = RT_NULL;

    mp->block_list = block_ptr;
    rt_spin_lock_init(&(mp->spinlock));

    return mp;
}
RTM_EXPORT(rt_mp_create);

/**
 * @brief 删除 rt_mp_create() 创建的动态内存池及其存储区。
 *
 * @param mp 动态内存池对象。
 *
 * @return 删除完成返回 RT_EOK。
 *
 * 等待线程先以 RT_ERROR 被全部恢复，然后释放块存储区和控制块。函数不验证
 * 是否仍有外借块，因此调用者必须先协调所有使用者；否则现存用户指针会变成
 * 悬空指针。堆释放要求线程上下文。
 */
rt_err_t rt_mp_delete(rt_mp_t mp)
{
    rt_base_t level;

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 动态内存池必须由 rt_mp_create() 创建，删除后其控制块也会被释放。 */
    RT_ASSERT(mp != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mp->parent) == RT_Object_Class_MemPool);
    RT_ASSERT(rt_object_is_systemobject(&mp->parent) == RT_FALSE);

    level = rt_spin_lock_irqsave(&(mp->spinlock));
    /* 先清空等待队列，避免线程永久睡在即将消失的对象上。 */
    rt_susp_list_resume_all(&mp->suspend_thread, RT_ERROR);

    rt_spin_unlock_irqrestore(&(mp->spinlock), level);

    /* 等待队列解锁后释放动态块存储区。 */
    rt_free(mp->start_address);

    /* 最后从对象表移除并释放动态控制块。 */
    rt_object_delete(&(mp->parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mp_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief 从内存池取得一个固定大小块，必要时等待释放者归还块。
 *
 * @param mp 目标内存池。
 *
 * @param time 最长等待 tick 数：0 表示不等待，负数表示无限等待，正数表示
 *             总超时预算。阻塞等待使用不可中断策略。
 *
 * @return 成功时返回用户区；立即失败、超时或对象删除唤醒时返回 RT_NULL，
 *         具体原因写入当前线程 error/errno 路径。
 *
 * while 循环非常重要：线程被唤醒不代表块已经为它保留，另一个执行者可能先
 * 取得刚释放的块，因此必须在重新加锁后检查 `block_free_count`。正超时会在
 * 每轮睡眠后扣除实际经过的 tick，避免多次竞争导致总等待时间无限延长。
 */
void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time)
{
    rt_uint8_t *block_ptr;
    rt_base_t level;
    struct rt_thread *thread;
    rt_uint32_t before_sleep = 0;

    /* 对象必须在整个调用期间保持有效。 */
    RT_ASSERT(mp != RT_NULL);

    /* 非阻塞成功路径可在中断中运行；只有真正等待时才会使用当前线程。 */
    thread = rt_thread_self();

    level = rt_spin_lock_irqsave(&(mp->spinlock));

    while (mp->block_free_count == 0)
    {
        /* 池为空：根据 time 决定立即失败还是把当前线程加入等待队列。 */
        if (time == 0)
        {
            rt_spin_unlock_irqrestore(&(mp->spinlock), level);

            rt_set_errno(-RT_ETIMEOUT);

            return RT_NULL;
        }

        RT_DEBUG_NOT_IN_INTERRUPT;

        thread->error = RT_EOK;

        /* 在持有池锁时原子地改变线程状态并插入 FIFO 等待队列。 */
        rt_thread_suspend_to_list(thread, &mp->suspend_thread, RT_IPC_FLAG_FIFO, RT_UNINTERRUPTIBLE);

        if (time > 0)
        {
            rt_tick_t time_tick = time;
            /* 记录入睡时刻，用于醒来后维护剩余总预算。 */
            before_sleep = rt_tick_get();

            /* 复用线程内嵌单次定时器；到期路径会把线程从等待队列移出。 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &time_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        /* 调度前必须释放池锁并恢复本地中断，否则其他线程无法归还块。 */
        rt_spin_unlock_irqrestore(&(mp->spinlock), level);

        /* 当前线程已经是挂起态，调度器会切换到其他可运行线程。 */
        rt_schedule();

        if (thread->error != RT_EOK)
            return RT_NULL;

        if (time > 0)
        {
            time -= rt_tick_get() - before_sleep;
            if (time < 0)
                time = 0;
        }
        level = rt_spin_lock_irqsave(&(mp->spinlock));
    }

    /* 在锁内同时更新计数与链头，使二者始终保持一致。 */
    mp->block_free_count--;

    /* 从空闲链头弹出一个槽位。 */
    block_ptr = mp->block_list;
    RT_ASSERT(block_ptr != RT_NULL);

    /* 空闲时内部字段保存 next；读取后推进池的链头。 */
    mp->block_list = *(rt_uint8_t **)block_ptr;

    /* 占用时把同一内部字段改写为所属池，供 rt_mp_free() 反向定位。 */
    *(rt_uint8_t **)block_ptr = (rt_uint8_t *)mp;

    rt_spin_unlock_irqrestore(&(mp->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_mp_alloc_hook,
                        (mp, (rt_uint8_t *)(block_ptr + sizeof(rt_uint8_t *))));

    return (rt_uint8_t *)(block_ptr + sizeof(rt_uint8_t *));
}
RTM_EXPORT(rt_mp_alloc);

/**
 * @brief 把一个固定块归还原内存池，并唤醒至多一个等待线程。
 *
 * @param block rt_mp_alloc() 返回的用户区地址；RT_NULL 被忽略。
 *
 * @warning 调用者必须保证该地址确实来自尚存活的内存池且只释放一次。当前
 *          实现没有范围或重复释放检查，错误指针会把任意内存解释为池地址。
 *
 * 释放操作本身不等待，可在中断上下文执行。若唤醒了更高优先级线程，函数在
 * 释放自旋锁后调用调度器，让抢占按正常规则发生。
 */
void rt_mp_free(void *block)
{
    rt_uint8_t **block_ptr;
    struct rt_mempool *mp;
    rt_base_t level;

    /* 允许按 C free 风格传入空指针。 */
    if (block == RT_NULL) return;

    /* 用户区前一个指针槽在占用期间保存所属内存池。 */
    block_ptr = (rt_uint8_t **)((rt_uint8_t *)block - sizeof(rt_uint8_t *));
    mp        = (struct rt_mempool *)*block_ptr;

    RT_OBJECT_HOOK_CALL(rt_mp_free_hook, (mp, block));

    level = rt_spin_lock_irqsave(&(mp->spinlock));

    /* 计数和空闲链修改都受同一自旋锁保护。 */
    mp->block_free_count ++;

    /* 头插回空闲链，O(1) 完成归还。 */
    *block_ptr = mp->block_list;
    mp->block_list = (rt_uint8_t *)block_ptr;

    if (rt_susp_list_dequeue(&mp->suspend_thread, RT_EOK))
    {
        rt_spin_unlock_irqrestore(&(mp->spinlock), level);

        /* 已有等待者转为就绪；解锁后给调度器一次抢占机会。 */
        rt_schedule();

        return;
    }
    rt_spin_unlock_irqrestore(&(mp->spinlock), level);
}
RTM_EXPORT(rt_mp_free);

/**@}*/

#endif /* RT_USING_MEMPOOL */
