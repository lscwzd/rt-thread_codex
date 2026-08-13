/*
 * Copyright (c) 2006-2023, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-10-30     Bernard      The first version
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-12-10     xqyjlj       spinlock should lock sched
 * 2024-01-25     Shell        Using rt_exit_critical_safe
*/

/**
 * @file cpu_mp.c
 * @brief SMP 配置下的每 CPU 状态、全局 CPU 锁以及自旋锁封装。
 *
 * SMP 内核允许多个处理器同时运行线程，因此仅“禁止本地中断”并不能阻止另一个
 * CPU 修改共享调度数据。本文件提供两层同步：普通 rt_spin_lock 用于某个具体共享
 * 对象；rt_cpus_lock 使用全局 _cpus_lock 串行化跨 CPU 的调度关键区。二者还会
 * 进入调度临界区，保证持锁线程不会在锁尚未释放时被普通调度切走。
 *
 * 每个 struct rt_cpu 保存当前线程、空闲线程、中断嵌套、调度请求和本地 tick 等
 * 状态。_cpus 数组按逻辑 CPU 编号索引；_cpus_lock 则是全系统唯一的调度大锁。
 */
#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_SMART
#include <lwp.h>
#endif

#ifdef RT_USING_DEBUG
/* 记录首次取得全局 CPU 锁时的临界区层数，供安全退出与调试检查使用。 */
rt_base_t _cpus_critical_level;
#endif /* RT_USING_DEBUG */

/* 每个逻辑 CPU 独享一个控制块。 */
static struct rt_cpu _cpus[RT_CPUS_NR];
/* 保护跨 CPU 调度器共享状态的全局硬件自旋锁。 */
rt_hw_spinlock_t _cpus_lock;
#if defined(RT_DEBUGING_SPINLOCK)
void *_cpus_lock_owner = 0;
void *_cpus_lock_pc = 0;

#endif /* RT_DEBUGING_SPINLOCK */

/**
 * @addtogroup group_thread_comm
 *
 * @cond DOXYGEN_SMP
 *
 * @{
 */

/**
 * @brief 初始化一个静态自旋锁对象。
 *
 * @param lock 待初始化的内核自旋锁。
 *
 * @note 本文件是 SMP 实现，会真正初始化底层硬件锁；UP 版本不需要硬件锁状态。
 */
void rt_spin_lock_init(struct rt_spinlock *lock)
{
    rt_hw_spin_lock_init(&lock->lock);
}
RTM_EXPORT(rt_spin_lock_init)

/**
 * @brief 进入调度临界区并获取自旋锁。
 *
 * 先禁止当前线程被普通调度，再在底层硬件锁上忙等。忙等期间中断并未被本接口
 * 关闭；若共享数据也会在本 CPU 的 ISR 中访问，应改用 rt_spin_lock_irqsave()。
 * 获取多个锁时必须保持固定顺序，否则不同 CPU 可能互相等待形成死锁。
 *
 * @param lock 要获取的锁。
 *
 * @note 不能在持锁期间睡眠或等待 IPC；解锁必须由同一执行路径配对完成。
 */
void rt_spin_lock(struct rt_spinlock *lock)
{
    rt_enter_critical();
    rt_hw_spin_lock(&lock->lock);
    RT_SPIN_LOCK_DEBUG(lock);
}
RTM_EXPORT(rt_spin_lock)

/**
 * @brief 释放自旋锁并退出对应的调度临界区。
 *
 * 先释放硬件锁，再恢复临界区层数。若持锁期间已经产生调度请求，最外层临界区退出
 * 时可能立即发生线程切换，因此调用者不能假定解锁后的下一条语句会马上执行。
 *
 * @param lock 先前由当前路径获取的锁。
 *
 * @note 本函数必须与 rt_spin_lock() 成对使用。
 */
void rt_spin_unlock(struct rt_spinlock *lock)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_hw_spin_unlock(&lock->lock);
    rt_exit_critical_safe(critical_level);
}
RTM_EXPORT(rt_spin_unlock)

/**
 * @brief 保存并关闭本地中断，然后进入调度临界区并获取自旋锁。
 *
 * 关闭的只是当前 CPU 中断；其他 CPU 仍可运行，跨核互斥由硬件自旋锁保证。这种
 * 组合适合同时会被线程和 ISR 访问的短小共享数据。锁竞争时本 CPU 持续轮询，故
 * 临界区必须尽可能短。
 *
 * @param lock 要获取的锁。
 *
 * @return 加锁前的本地中断状态，必须原样传给 rt_spin_unlock_irqrestore()。
 *
 * @note 该接口同时保护线程和本 CPU ISR 上下文。
 */
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock)
{
    rt_base_t level;

    level = rt_hw_local_irq_disable();
    rt_enter_critical();
    rt_hw_spin_lock(&lock->lock);
    RT_SPIN_LOCK_DEBUG(lock);
    return level;
}
RTM_EXPORT(rt_spin_lock_irqsave)

/**
 * @brief 释放自旋锁、退出调度临界区并恢复本地中断状态。
 *
 * 恢复顺序与加锁相反：先让其他 CPU 能取得锁，再处理可能延后的调度，最后按照
 * @p level 恢复本 CPU 先前的中断开关状态。
 *
 * @param lock 先前取得的锁。
 *
 * @param level rt_spin_lock_irqsave() 返回的中断状态，不应自行构造。
 *
 * @note 必须与 rt_spin_lock_irqsave() 成对使用。
 */
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level)
{
    rt_base_t critical_level;

    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_hw_spin_unlock(&lock->lock);
    rt_exit_critical_safe(critical_level);
    rt_hw_local_irq_enable(level);
}
RTM_EXPORT(rt_spin_unlock_irqrestore)

/**
 * @brief 取得当前正在执行代码的 CPU 控制块。
 *
 * @return 由架构 rt_hw_cpu_id() 确定的 struct rt_cpu 指针。
 *
 * @note 当前 CPU 在调用期间必须保持稳定。
 */
struct rt_cpu *rt_cpu_self(void)
{
    return &_cpus[rt_hw_cpu_id()];
}

/**
 * @brief 按逻辑编号取得 CPU 控制块。
 *
 * @param index 目标 CPU 下标，调用者必须保证在 [0, RT_CPUS_NR) 内。
 *
 * @return 对应的 struct rt_cpu 指针；本函数不做越界检查。
 *
 * @note 可用于查询其他 CPU 的状态，但读取并发字段仍需相应同步。
 */
struct rt_cpu *rt_cpu_index(int index)
{
    return &_cpus[index];
}

/**
 * @brief 关闭本地中断并以可嵌套方式获取全局 CPU 调度锁。
 *
 * 每个线程用 cpus_lock_nest 记录嵌套层数。只有从 0 变为 1 时才进入调度临界区并
 * 真正获取 _cpus_lock，内层调用只递增计数。这样同一线程中的嵌套内核路径不会
 * 在不可重入的硬件锁上自我死锁。若启动早期尚无 current_thread，本函数只关闭
 * 本地中断，不触碰线程级嵌套字段。
 *
 * @return 调用前的本地中断状态，供 rt_cpus_unlock() 精确恢复。
 *
 * @note 该函数仅在 SMP 版本存在。
 */
rt_base_t rt_cpus_lock(void)
{
    rt_base_t level;
    struct rt_cpu* pcpu;

    level = rt_hw_local_irq_disable();
    pcpu = rt_cpu_self();
    if (pcpu->current_thread != RT_NULL)
    {
        rt_ubase_t lock_nest = rt_atomic_load(&(pcpu->current_thread->cpus_lock_nest));

        rt_atomic_add(&(pcpu->current_thread->cpus_lock_nest), 1);
        if (lock_nest == 0)
        {
            rt_enter_critical();
            rt_hw_spin_lock(&_cpus_lock);
#ifdef RT_USING_DEBUG
            _cpus_critical_level = rt_critical_level();
#endif /* RT_USING_DEBUG */

#ifdef RT_DEBUGING_SPINLOCK
            _cpus_lock_owner = pcpu->current_thread;
            _cpus_lock_pc = __GET_RETURN_ADDRESS;
#endif /* RT_DEBUGING_SPINLOCK */
        }
    }

    return level;
}
RTM_EXPORT(rt_cpus_lock);

/**
 * @brief 退出一层全局 CPU 锁并恢复本地中断。
 *
 * 只有嵌套层数降到 0 才真正释放 _cpus_lock 并退出调度临界区。严格的 LIFO 配对
 * 很重要：每一次 rt_cpus_lock() 都必须使用它返回的状态调用一次本函数。
 *
 * @param level 配对的 rt_cpus_lock() 返回值。
 *
 * @note 该函数仅在 SMP 版本存在。
 */
void rt_cpus_unlock(rt_base_t level)
{
    struct rt_cpu* pcpu = rt_cpu_self();

    if (pcpu->current_thread != RT_NULL)
    {
        rt_base_t critical_level = 0;
        RT_ASSERT(rt_atomic_load(&(pcpu->current_thread->cpus_lock_nest)) > 0);
        rt_atomic_sub(&(pcpu->current_thread->cpus_lock_nest), 1);

        if (pcpu->current_thread->cpus_lock_nest == 0)
        {
#if defined(RT_DEBUGING_SPINLOCK)
            _cpus_lock_owner = __OWNER_MAGIC;
            _cpus_lock_pc = RT_NULL;
#endif /* RT_DEBUGING_SPINLOCK */
#ifdef RT_USING_DEBUG
            critical_level = _cpus_critical_level;
            _cpus_critical_level = 0;
#endif /* RT_USING_DEBUG */
            rt_hw_spin_unlock(&_cpus_lock);
            rt_exit_critical_safe(critical_level);
        }
    }
    rt_hw_local_irq_enable(level);
}
RTM_EXPORT(rt_cpus_unlock);

/**
 * @brief 上下文切换后恢复目标线程所期望的 CPU 锁/地址空间状态。
 *
 * 调度器在已经选中并切换到 @p thread 后调用它。Smart MMU 配置先切换到该线程
 * 所属进程的地址空间，再由 rt_sched_post_ctx_switch() 根据线程保存的嵌套状态
 * 完成切换后处理。它不是普通应用可调用的加锁接口。
 *
 * @param thread 本次切换进入的目标线程。
 *
 * @note 该函数仅在 SMP 版本存在。
 */
void rt_cpus_lock_status_restore(struct rt_thread *thread)
{
#if defined(ARCH_MM_MMU) && defined(RT_USING_SMART)
    lwp_aspace_switch(thread);
#endif
    rt_sched_post_ctx_switch(thread);
}
RTM_EXPORT(rt_cpus_lock_status_restore);

/* 面向普通内核代码的带约束检查 CPU 编号接口。 */

#undef rt_cpu_get_id
/**
 * @brief 获取当前逻辑 CPU 编号。
 *
 * 为避免“取到编号后线程迁移到别的 CPU”这一竞态，断言要求调用者满足至少一个
 * 条件：线程已绑定 CPU、本地中断已关闭，或调度器尚不可用。在这些条件下当前
 * 执行流不会在读取期间迁移。
 *
 * @return 当前架构报告的逻辑 CPU 编号。
 *
 * @note 该安全包装仅在 SMP 版本存在。
 */
rt_base_t rt_cpu_get_id(void)
{

    RT_ASSERT(rt_sched_thread_is_binding(RT_NULL) ||
              rt_hw_interrupt_is_disabled() ||
              !rt_scheduler_is_available());

    return rt_hw_cpu_id();
}

/**
 * @}
 *
 * @endcond
 */
