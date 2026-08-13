/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-12     Bernard      first version
 * 2006-05-27     Bernard      add support for same priority thread schedule
 * 2006-08-10     Bernard      remove the last rt_schedule in rt_tick_increase
 * 2010-03-08     Bernard      remove rt_passed_second
 * 2010-05-20     Bernard      fix the tick exceeds the maximum limits
 * 2010-07-13     Bernard      fix rt_tick_from_millisecond issue found by kuronca
 * 2011-06-26     Bernard      add rt_tick_set function.
 * 2018-11-22     Jesven       add per cpu tick
 * 2020-12-29     Meco Man     implement rt_tick_get_millisecond()
 * 2021-06-01     Meco Man     add critical section projection for rt_tick_increase()
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-10-16     RiceChen     fix: only the main core detection rt_timer_check(), in SMP mode
*/

/**
 * @file clock.c
 * @brief 内核节拍（tick）计数、时间换算以及周期性调度工作的入口。
 *
 * 初学者可以把“节拍”理解为内核使用的离散时钟单位。硬件定时器通常每隔固定
 * 时间产生一次中断，BSP 的时钟中断服务程序随后调用 rt_tick_increase()。一次
 * 节拍到来后，本文件依次完成四件事：统计当前线程的 CPU 时间、递增节拍计数、
 * 扣减当前线程的时间片，以及检查已经到期的软件定时器。因此，它把硬件时钟与
 * 调度器、定时器子系统连接起来。
 *
 * UP（单核）配置只有一个全局 rt_tick；SMP 配置则让每个 CPU 保存自己的 tick，
 * 对外作为“系统时间”的 rt_tick 宏固定引用 CPU 0 的计数。各 CPU 都维护本地
 * 调度时间片，但只有 CPU 0 检查全局软件定时器，避免同一定时器被多个核重复处理。
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtatomic.h>

#if defined(RT_USING_SMART) && defined(RT_USING_VDSO)
#include "vdso_kernel.h"
#endif

#ifdef RT_USING_SMP
/* SMP 中 CPU 0 的 tick 是供公共时间 API 使用的系统基准。 */
#define rt_tick rt_cpu_index(0)->tick
#else
/* UP 中由中断写、普通线程读，所以使用原子类型保证访问不可被撕裂。 */
static volatile rt_atomic_t rt_tick = 0;
#endif /* RT_USING_SMP */

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_tick_hook)(void);

/**
 * @addtogroup group_hook
 */

/**@{*/

/**
 * @brief 设置节拍到达钩子。
 *
 * 每次 rt_tick_increase() 或 rt_tick_increase_tick() 开始处理节拍时调用该钩子，
 * 调用点位于时钟中断上下文，并且早于计数递增、时间片处理和定时器检查。钩子必须
 * 足够短，不能睡眠、等待 IPC 或执行任何可能阻塞的操作。再次设置会覆盖原钩子，
 * 传入 RT_NULL 可取消钩子。
 *
 * @param hook 无参数、无返回值的回调函数指针。
 */
void rt_tick_sethook(void (*hook)(void))
{
    rt_tick_hook = hook;
}
/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_clock_management
 */

/**@{*/

/**
 * @brief 返回自系统启动以来的当前节拍计数。
 *
 * @return 当前系统节拍。计数达到类型上限后会自然回绕。
 */
rt_tick_t rt_tick_get(void)
{
    /* 原子读取可避免与时钟中断的并发更新产生撕裂值。 */
    return (rt_tick_t)rt_atomic_load(&(rt_tick));
}
RTM_EXPORT(rt_tick_get);

/**
 * @brief 计算从基准节拍到当前时刻经过的节拍数，并正确处理一次计数回绕。
 *
 * @param base 先前由 rt_tick_get() 等途径取得的基准节拍。
 *
 * @return 从 @p base 到当前节拍的无符号距离。
 */
rt_tick_t rt_tick_get_delta(rt_tick_t base)
{
    rt_tick_t tnow = rt_tick_get();
    if (tnow >= base)
        return tnow - base;
    return RT_TICK_MAX - base + tnow + 1;
}
RTM_EXPORT(rt_tick_get_delta);

/**
 * @brief 直接设置系统节拍计数。
 *
 * 该接口会改变所有基于绝对 tick 观察到的时间，通常只供底层移植、测试或时钟恢复
 * 代码使用。SMP 下实际修改 CPU 0 的系统基准 tick。
 *
 * @param tick 要写入的新节拍值。
 */
void rt_tick_set(rt_tick_t tick)
{
    rt_atomic_store(&(rt_tick), tick);
}

#ifdef RT_USING_CPU_USAGE_TRACER
static void _update_process_times(rt_tick_t tick)
{
    struct rt_thread *thread = rt_thread_self();
    struct rt_cpu *pcpu = rt_cpu_self();

    /*
     * LWP_IS_USER_MODE() 的返回语义由 Smart 移植定义。这里完全按照内核现有判定
     * 分别累计 user_time 或 system_time；若当前是空闲线程，再记入 idle 时间。
     */
    if (!LWP_IS_USER_MODE(thread))
    {
        thread->user_time += tick;
        pcpu->cpu_stat.user += tick;
    }
    else
    {
        thread->system_time += tick;
        if (thread == pcpu->idle_thread)
        {
            pcpu->cpu_stat.idle += tick;
        }
        else
        {
            pcpu->cpu_stat.system += tick;
        }
    }
}

#else

#define _update_process_times(tick)
#endif /* RT_USING_CPU_USAGE_TRACER */

/**
 * @brief 通知内核已经过去一个节拍。
 *
 * 该函数必须在调用 rt_interrupt_enter() 之后的时钟 ISR 中执行；开头的断言正是
 * 为了捕获从普通线程上下文误调用的情况。处理顺序如下：
 * 1. 调用节拍钩子，让跟踪代码观察本次 tick；
 * 2. 给当前线程/CPU 累计一个 tick 的运行时间；
 * 3. 原子递增本 CPU 的 tick；
 * 4. 通知调度器扣减时间片，必要时提出调度请求；
 * 5. 仅由 CPU 0 检查到期定时器，并在启用 VDSO 时同步共享时钟数据。
 *
 * 函数本身不负责调用 rt_interrupt_leave()，该配对动作属于 BSP 中断入口/出口。
 */
void rt_tick_increase(void)
{
    RT_ASSERT(rt_interrupt_get_nest() > 0);

    RT_OBJECT_HOOK_CALL(rt_tick_hook, ());

    /* 可选的 CPU 使用时间统计，仍然运行在中断上下文。 */
    _update_process_times(1);

    /* 增加计数；SMP 更新当前 CPU，UP 更新唯一的全局计数。 */
#ifdef RT_USING_SMP
    /* 每 CPU 计数避免多个核争用同一个写热点。 */
    rt_atomic_add(&(rt_cpu_self()->tick), 1);
#else
    rt_atomic_add(&(rt_tick), 1);
#endif /* RT_USING_SMP */

    /* 更新当前线程的剩余时间片；调度动作通常在中断退出路径兑现。 */
    rt_sched_tick_increase(1);

    /* 软件定时器是全局资源，SMP 下只允许主核检查一次。 */
#ifdef RT_USING_SMP
    if (rt_cpu_get_id() != 0)
    {
        return;
    }
#endif
    rt_timer_check();

#ifdef RT_USING_VDSO
    rt_vdso_sync_clock_data();
#endif
}

/**
 * @brief 通知内核一次性经过了 @p tick 个节拍。
 *
 * 这是无节拍（tickless）或补偿遗漏节拍场景的批量版本，调用上下文和基本步骤与
 * rt_tick_increase() 相同。节拍钩子只调用一次，但 CPU 时间、系统 tick 和线程
 * 时间片都按 @p tick 一次性累加/扣减；定时器检查会处理此时已经到期的定时器。
 *
 * @param tick 本次需要补记的节拍数。
 */
void rt_tick_increase_tick(rt_tick_t tick)
{
    RT_ASSERT(rt_interrupt_get_nest() > 0);

    RT_OBJECT_HOOK_CALL(rt_tick_hook, ());

    /* 将整段经过时间计入当前执行实体。 */
    _update_process_times(tick);

    /* 批量推进本 CPU 的节拍计数。 */
#ifdef RT_USING_SMP
    /* SMP 的公共系统时钟仍以 CPU 0 的计数为准。 */
    rt_atomic_add(&(rt_cpu_self()->tick), tick);
#else
    rt_atomic_add(&(rt_tick), tick);
#endif /* RT_USING_SMP */

    /* 一次性消耗相应数量的时间片。 */
    rt_sched_tick_increase(tick);

    /* 与单 tick 入口相同，只由 CPU 0 扫描全局定时器。 */
#ifdef RT_USING_SMP
    if (rt_cpu_get_id() != 0)
    {
        return;
    }
#endif
    rt_timer_check();
}

/**
 * @brief 将毫秒数换算成内核节拍数。
 *
 * 对正数采用向上取整：只要请求的毫秒数不是整 tick，就多等待一个 tick，从而避免
 * 实际等待短于调用者要求。负数映射为 RT_WAITING_FOREVER，零表示不等待。
 *
 * @param ms 指定毫秒数：负数表示永久等待，0 表示立即返回，正数最大为 0x7fffffff。
 *
 * @return 对应的 rt_tick_t 节拍数。
 */
rt_tick_t rt_tick_from_millisecond(rt_int32_t ms)
{
    rt_tick_t tick;

    if (ms < 0)
    {
        tick = (rt_tick_t)RT_WAITING_FOREVER;
    }
    else
    {
#if RT_TICK_PER_SECOND == 1000u
        tick = ms;
#else
        tick = RT_TICK_PER_SECOND * (ms / 1000);
        tick += (RT_TICK_PER_SECOND * (ms % 1000) + 999) / 1000;
#endif /* RT_TICK_PER_SECOND == 1000u */
    }

    /* 返回已经按当前 RT_TICK_PER_SECOND 配置换算并向上取整的结果。 */
    return tick;
}
RTM_EXPORT(rt_tick_from_millisecond);

/**
 * @brief 返回从系统启动至今经过的毫秒数。
 *
 * @note 只有当 1000 能被 RT_TICK_PER_SECOND 整除时，整数换算才准确。否则本弱
 *       实现发出编译警告并返回 0，BSP 应使用高精度硬件定时器提供同名强实现。
 *
 * @return 启动后的毫秒数；默认实现无法精确换算时返回 0。
 */
rt_weak rt_tick_t rt_tick_get_millisecond(void)
{
#if RT_TICK_PER_SECOND == 0 /* 同时让 cppcheck 明确识别除数非零约束。 */
#error "RT_TICK_PER_SECOND must be greater than zero"
#endif

#if 1000 % RT_TICK_PER_SECOND == 0u
    return rt_tick_get() * (1000u / RT_TICK_PER_SECOND);
#else
#warning "rt-thread cannot provide a correct 1ms-based tick any longer,\
    please redefine this function in another file by using a high-precision hard-timer."
    return 0;
#endif /* 1000 % RT_TICK_PER_SECOND == 0u */
}

/**@}*/
