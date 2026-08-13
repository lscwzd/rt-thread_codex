/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-04-19     Shell        Fixup UP irq spinlock
 * 2024-05-22     Shell        Add UP cpu object and
 *                             maintain the rt_current_thread inside it
*/

/**
 * @file cpu_up.c
 * @brief 单处理器（UP）配置下的 CPU 对象与自旋锁兼容实现。
 *
 * 单核系统不存在另一个 CPU 同时修改共享数据，因此“自旋等待硬件锁”没有意义。
 * 为了让上层代码能与 SMP 共用同一套 API，本文件把普通自旋锁退化为调度临界区，
 * 把 irqsave 自旋锁退化为“关中断 + 调度临界区”。这种退化仍能防止本 CPU 的线程
 * 或 ISR 在临界区中并发访问共享数据，但绝不会忙等某个锁变量。
 */
#include <rthw.h>
#include <rtthread.h>

/* UP 永远只有一个 CPU 控制块，rt_cpu_index(0) 和 rt_cpu_self() 都返回它。 */
static struct rt_cpu _cpu;

/**
 * @addtogroup group_thread_comm
 *
 * @cond
 *
 * @{
 */

/**
 * @brief 初始化静态自旋锁的 UP 占位实现。
 *
 * UP 不需要底层锁状态，参数仅为保持与 SMP API 一致。
 *
 * @param lock 待初始化的锁对象；本实现不读取它。
 */
void rt_spin_lock_init(struct rt_spinlock *lock)
{
    RT_UNUSED(lock);
}

/**
 * @brief 在 UP 下通过进入调度临界区模拟普通自旋锁加锁。
 *
 * @note 这里只阻止线程调度，不关闭中断；若数据也由 ISR 访问，必须使用
 *       rt_spin_lock_irqsave()。调试配置仍记录锁拥有者，以便发现错误配对。
 *
 * @param lock 逻辑锁对象。
 */
void rt_spin_lock(struct rt_spinlock *lock)
{
    rt_enter_critical();
    RT_SPIN_LOCK_DEBUG(lock);
}

/**
 * @brief 退出普通自旋锁对应的调度临界区。
 *
 * @note 若临界区内已产生调度请求，退出最外层临界区时可能立即切换线程。
 *
 * @param lock 与加锁调用配对的逻辑锁对象。
 */
void rt_spin_unlock(struct rt_spinlock *lock)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_exit_critical_safe(critical_level);
}

/**
 * @brief 保存并关闭中断，然后进入调度临界区。
 *
 * @note UP 无需真正自旋；关中断排除了 ISR 并发，调度临界区排除了线程切换。
 *
 * @param lock 逻辑锁对象。
 *
 * @return 加锁前的中断状态，必须传回配对的解锁函数。
 */
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock)
{
    rt_base_t level;
    RT_UNUSED(lock);
    level = rt_hw_interrupt_disable();
    rt_enter_critical();
    RT_SPIN_LOCK_DEBUG(lock);
    return level;
}

/**
 * @brief 退出调度临界区并恢复加锁前的中断状态。
 *
 * @note 先处理临界区退出，再使用 @p level 恢复中断屏蔽状态。
 *
 * @param lock 与加锁调用配对的逻辑锁对象。
 *
 * @param level rt_spin_lock_irqsave() 返回的原始中断状态。
 */
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);
    rt_exit_critical_safe(critical_level);
    rt_hw_interrupt_enable(level);
}

/**
 * @brief 返回唯一的 UP CPU 控制块。
 *
 * @return &_cpu。
 */
struct rt_cpu *rt_cpu_self(void)
{
    return &_cpu;
}

/**
 * @brief 按编号取得 UP CPU 控制块。
 *
 * @param index 只能为 0。
 *
 * @return index 为 0 时返回 &_cpu，否则返回 RT_NULL。
 */
struct rt_cpu *rt_cpu_index(int index)
{
    return index == 0 ? &_cpu : RT_NULL;
}

/**
 * @}
 *
 * @endcond
 */
