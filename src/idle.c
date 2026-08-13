/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-23     Bernard      the first version
 * 2010-11-10     Bernard      add cleanup callback function in thread exit.
 * 2012-12-29     Bernard      fix compiling warning.
 * 2013-12-21     Grissiom     let rt_thread_idle_excute loop until there is no
 *                             dead thread.
 * 2016-08-09     ArdaFu       add method to get the handler of the idle thread.
 * 2018-02-07     Bernard      lock scheduler to protect tid->cleanup.
 * 2018-07-14     armink       add idle hook list
 * 2018-11-22     Jesven       add per cpu idle task
 *                             combine the code of primary and secondary cpu
 * 2021-11-15     THEWON       Remove duplicate work between idle and _thread_exit
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-11-07     xqyjlj       fix thread exit
 * 2023-12-10     xqyjlj       add _hook_spinlock
*/

/**
 * @file idle.c
 * @brief 创建每 CPU 空闲线程，并管理空闲钩子和后台低功耗工作。
 *
 * 调度器必须始终至少有一个可运行线程，所以每个 CPU 都拥有一个最低优先级空闲
 * 线程。它永不阻塞、永不退出：没有正常工作时反复执行空闲钩子、僵尸回收（特定
 * UP 配置）和电源管理。SMP 的从核空闲线程直接进入架构提供的从核 idle 循环，
 * 主核则执行公共空闲工作。
 *
 * idle_hook_list 是固定容量回调表。回调在空闲线程上下文而非 ISR 中执行，但仍应
 * 快速返回且不能永久阻塞，否则会妨碍低功耗和后台回收。注册/删除由自旋锁保护，
 * 执行回调时不持锁，避免用户钩子造成锁递归。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_MODULE
#include <dlmodule.h>
#endif /* RT_USING_MODULE */

#ifdef RT_USING_HOOK
#ifndef RT_USING_IDLE_HOOK
#define RT_USING_IDLE_HOOK
#endif /* RT_USING_IDLE_HOOK */
#endif /* RT_USING_HOOK */

#ifndef IDLE_THREAD_STACK_SIZE
#if defined (RT_USING_IDLE_HOOK) || defined(RT_USING_HEAP)
#define IDLE_THREAD_STACK_SIZE  256
#else
#define IDLE_THREAD_STACK_SIZE  128
#endif /* (RT_USING_IDLE_HOOK) || defined(RT_USING_HEAP) */
#endif /* IDLE_THREAD_STACK_SIZE */

#define _CPUS_NR                RT_CPUS_NR

/* 每个 CPU 的静态空闲线程控制块及独立栈，生命周期覆盖整个系统运行期。 */
static struct rt_thread idle_thread[_CPUS_NR];
rt_align(RT_ALIGN_SIZE)
static rt_uint8_t idle_thread_stack[_CPUS_NR][IDLE_THREAD_STACK_SIZE];

#ifdef RT_USING_IDLE_HOOK
#ifndef RT_IDLE_HOOK_LIST_SIZE
#define RT_IDLE_HOOK_LIST_SIZE  4
#endif /* RT_IDLE_HOOK_LIST_SIZE */

/* RT_NULL 表示空槽；注册按下标寻找第一个可用位置。 */
static void (*idle_hook_list[RT_IDLE_HOOK_LIST_SIZE])(void);
static struct rt_spinlock _hook_spinlock;

/**
 * @brief 向固定容量的空闲钩子表注册一个回调。
 *
 * 在自旋锁内寻找第一个 RT_NULL 槽并写入。当前实现不检查重复注册：同一函数可占用
 * 多个槽，并在每轮空闲循环中被调用多次；传入 RT_NULL 也会返回成功，但槽仍表现为
 * 空闲，因此调用者应只传有效且尚未注册的函数。SMP 下公共空闲钩子只由 CPU 0 的
 * 空闲线程执行，从核进入 rt_hw_secondary_cpu_idle_exec()。
 *
 * @param hook 无参数、无返回值的空闲回调。回调位于空闲线程上下文，不持钩子锁，
 *             但必须快速返回且不应阻塞。
 * @return 成功写入返回 RT_EOK；所有槽均占用时返回 -RT_EFULL。
 */
rt_err_t rt_thread_idle_sethook(void (*hook)(void))
{
    rt_size_t i;
    rt_err_t ret = -RT_EFULL;
    rt_base_t level;

    level = rt_spin_lock_irqsave(&_hook_spinlock);

    /* 锁只覆盖槽位修改；钩子本身绝不会在锁内调用。 */
    for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
    {
        if (idle_hook_list[i] == RT_NULL)
        {
            idle_hook_list[i] = hook;
            ret = RT_EOK;
            break;
        }
    }

    rt_spin_unlock_irqrestore(&_hook_spinlock, level);

    return ret;
}

/**
 * @addtogroup group_thread_management
 * @{
 */

/**
 * @brief 从空闲钩子表中删除指定回调。
 *
 * @param hook 先前注册的函数指针。若同一指针重复存在，本次只删除第一个匹配项。
 *
 * @return `RT_EOK` 表示删除成功；`-RT_ENOSYS` 表示未找到该钩子。
 */
rt_err_t rt_thread_idle_delhook(void (*hook)(void))
{
    rt_size_t i;
    rt_err_t ret = -RT_ENOSYS;
    rt_base_t level;

    level = rt_spin_lock_irqsave(&_hook_spinlock);

    for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
    {
        if (idle_hook_list[i] == hook)
        {
            idle_hook_list[i] = RT_NULL;
            ret = RT_EOK;
            break;
        }
    }

    rt_spin_unlock_irqrestore(&_hook_spinlock, level);

    return ret;
}

#endif /* RT_USING_IDLE_HOOK */

static void idle_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);
#ifdef RT_USING_SMP
    /* 从核交给架构 idle 例程，公共钩子和系统后台任务仅在主核执行。 */
    if (rt_cpu_get_id() != 0)
    {
        while (1)
        {
            rt_hw_secondary_cpu_idle_exec();
        }
    }
#endif /* RT_USING_SMP */

    while (1)
    {
#ifdef RT_USING_IDLE_HOOK
        rt_size_t i;
        void (*idle_hook)(void);

        /* 逐槽复制函数指针后调用；执行期间不持有 _hook_spinlock。 */
        for (i = 0; i < RT_IDLE_HOOK_LIST_SIZE; i++)
        {
            idle_hook = idle_hook_list[i];
            if (idle_hook != RT_NULL)
            {
                idle_hook();
            }
        }
#endif /* RT_USING_IDLE_HOOK */

#if !defined(RT_USING_SMP) && !defined(RT_USING_SMART)
    /* 这些配置没有独立 tsystem 回收线程，故由空闲线程安全释放退出线程资源。 */
    rt_defunct_execute();
#endif

#ifdef RT_USING_PM
        /* 电源管理可在确认无工作后让 CPU 进入架构低功耗状态。 */
        void rt_system_power_manager(void);
        rt_system_power_manager();
#endif /* RT_USING_PM */
    }
}

/**
 * @brief 初始化并启动所有 CPU 的空闲线程。
 *
 * 每个线程使用最低优先级 RT_THREAD_PRIORITY_MAX - 1。SMP 下还将第 i 个线程绑定
 * 到 CPU i，并写入对应 rt_cpu 的 idle_thread 字段，供调度器在无普通就绪线程时
 * 识别兜底线程。
 *
 * @note 必须在系统启动阶段、调度器正式开始运行前调用一次。
 */
void rt_thread_idle_init(void)
{
    rt_ubase_t i;
#if RT_NAME_MAX > 0
    char idle_thread_name[RT_NAME_MAX];
#endif /* RT_NAME_MAX > 0 */

#ifdef RT_USING_IDLE_HOOK
    rt_spin_lock_init(&_hook_spinlock);
#endif

    for (i = 0; i < _CPUS_NR; i++)
    {
#if RT_NAME_MAX > 0
        rt_snprintf(idle_thread_name, RT_NAME_MAX, "tidle%d", i);
#endif /* RT_NAME_MAX > 0 */
        rt_thread_init(&idle_thread[i],
#if RT_NAME_MAX > 0
                idle_thread_name,
#else
                "tidle",
#endif /* RT_NAME_MAX > 0 */
                idle_thread_entry,
                RT_NULL,
                &idle_thread_stack[i][0],
                sizeof(idle_thread_stack[i]),
                RT_THREAD_PRIORITY_MAX - 1,
                32);
#ifdef RT_USING_SMP
        rt_thread_control(&idle_thread[i], RT_THREAD_CTRL_BIND_CPU, (void*)i);
#endif /* RT_USING_SMP */

        /* 建立 CPU 控制块到其专属空闲线程的反向关联。 */
        rt_cpu_index(i)->idle_thread = &idle_thread[i];

        /* 将空闲线程置为 READY，确保该 CPU 的就绪集合永不为空。 */
        rt_thread_startup(&idle_thread[i]);
    }
}

/**
 * @brief 获取当前 CPU 的空闲线程句柄。
 *
 * @return 当前逻辑 CPU 对应的静态空闲线程控制块。
 */
rt_thread_t rt_thread_idle_gethandler(void)
{
    int id = rt_cpu_get_id();

    return (rt_thread_t)(&idle_thread[id]);
}

/**
 * @brief 判断指定线程是否为任意 CPU 的系统空闲线程。
 *
 * @details RT-Thread 为每个 CPU 创建一个由调度器管理的空闲线程。当没有其他就绪
 * 线程时，它是保证调度器仍有可运行对象的最后兜底。
 *
 * 阻塞或挂起路径用本函数做防御检查，因为空闲线程绝不能进入阻塞/挂起态。否则
 * 对应 CPU 可能失去最后一个就绪线程，破坏调度器的基本不变量。
 *
 * @param thread 待判断的线程，允许为 RT_NULL。
 *
 * @return 若 @p thread 匹配任一 CPU 的空闲线程则返回 RT_TRUE，否则返回 RT_FALSE。
 *
 * @note SMP 下会遍历全部空闲线程；传入 RT_NULL 返回 RT_FALSE。
 */
rt_bool_t rt_thread_is_idle_thread(rt_thread_t thread)
{
    rt_ubase_t i;

    if (thread != RT_NULL)
    {
        for (i = 0; i < _CPUS_NR; i++)
        {
            if (thread == &idle_thread[i])
                return RT_TRUE;
        }
    }

    return RT_FALSE;
}

/** @} group_thread_management */
