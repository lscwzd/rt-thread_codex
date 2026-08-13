/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * (scheduler_comm.c) 调度流程的 UP/SMP 公共 API。
 *
 * Change Logs:
 * Date           Author       Notes
 * 2024-01-18     Shell        Separate scheduling related codes from thread.c, scheduler_.*
 * 2025-09-01     Rbb666       Add thread stack overflow hook.
 */

/**
 * @file scheduler_comm.c
 * @brief UP 与 SMP 调度器共用的线程调度上下文操作。
 *
 * scheduler_up.c 和 scheduler_mp.c 负责“如何从就绪集合选择并切换线程”，本文件
 * 则提供两种实现都需要的状态操作：初始化调度字段、管理线程超时定时器、读取状态
 * 与优先级、让线程就绪或让出、更新优先级、扣减时间片以及检查栈边界。
 *
 * 线程状态可粗略理解为 INIT（尚未运行/已移出队列）-> READY（在就绪队列）->
 * RUNNING（CPU 当前线程）或 SUSPEND（等待 IPC/延时）-> READY，最终进入 CLOSE。
 * 状态字段还叠加 YIELD 等标志，所以比较基本状态时必须使用 RT_THREAD_STAT_MASK。
 * 本文件大多数内部接口要求调度锁已持有，RT_SCHED_DEBUG_IS_LOCKED 用于调试验证。
 */

#define DBG_TAG "kernel.sched"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#include <rtthread.h>

/**
 * @brief 初始化新线程的公共调度上下文。
 *
 * 首先把线程置为 INIT，表示它尚未进入就绪队列；SMP 下再标记为未绑定任何 CPU、
 * 当前也不在任何 CPU 上运行。最后调用 UP/SMP 私有实现初始化时间片、优先级位图
 * 属性及其他调度字段。
 *
 * @param thread 已完成基础对象初始化、待建立调度字段的线程。
 * @param tick 初始时间片长度，每次时间片重装都以此为基准。
 * @param priority 初始优先级；RT-Thread 中数值越小优先级越高。
 */
void rt_sched_thread_init_ctx(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority)
{
    /* INIT 表示尚未挂入任何调度队列。 */
    RT_SCHED_CTX(thread).stat = RT_THREAD_INIT;

#ifdef RT_USING_SMP
    /* RT_CPUS_NR 用作“未绑定”哨兵，RT_CPU_DETACHED 表示不在 CPU 上运行。 */
    RT_SCHED_CTX(thread).bind_cpu = RT_CPUS_NR;
    RT_SCHED_CTX(thread).oncpu = RT_CPU_DETACHED;
#endif /* RT_USING_SMP */

    rt_sched_thread_init_priv(thread, tick, priority);
}

/**
 * @brief 标记线程的调度超时定时器已经启用。
 *
 * 本函数不直接启动 rt_timer；调用阻塞 API 的上层代码负责设置并启动具体定时器，
 * 这里维护 sched_flag_ttmr_set，使唤醒路径知道是否必须先停止一个潜在超时源。
 *
 * @param thread 要记录定时器状态的阻塞线程。
 * @return 始终返回 RT_EOK。
 * @note 调用者必须持有调度锁。
 */
rt_err_t rt_sched_thread_timer_start(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    RT_SCHED_CTX(thread).sched_flag_ttmr_set = 1;
    return RT_EOK;
}

/**
 * @brief 停止线程的调度超时定时器并清除活动标志。
 *
 * 若标志未设置，直接视为成功。若已设置，则调用 rt_timer_stop()；无论停止返回
 * 什么，都清掉标志，防止后续路径重复停止。停止失败往往意味着定时器 ISR 已经
 * 竞争到执行权，调用者必须检查并传播错误，不能继续把同一线程重复唤醒。
 *
 * @param thread 其超时源需要撤销的线程。
 * @return 未启用或成功停止时返回 RT_EOK，否则返回 rt_timer_stop() 的错误码。
 * @note 调用者必须持有调度锁。
 */
rt_err_t rt_sched_thread_timer_stop(struct rt_thread *thread)
{
    rt_err_t error;
    RT_SCHED_DEBUG_IS_LOCKED;

    if (RT_SCHED_CTX(thread).sched_flag_ttmr_set)
    {
        error = rt_timer_stop(&thread->thread_timer);

        /* 不论底层停止结果如何，本路径都不再把它当作已登记的线程定时器。 */
        RT_SCHED_CTX(thread).sched_flag_ttmr_set = 0;
    }
    else
    {
        error = RT_EOK;
    }
    return error;
}

/**
 * @brief 取得线程不含附加标志的基本状态。
 *
 * @param thread 待读取线程。
 *
 * @return stat 与 RT_THREAD_STAT_MASK 相与后的 INIT/READY/SUSPEND/CLOSE 等基本状态。
 * @note 调用者必须持有调度锁，因为状态可能被其他调度路径并发改变。
 */
rt_uint8_t rt_sched_thread_get_stat(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    return RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK;
}

/**
 * @brief 取得线程当前有效优先级。
 *
 * 该值可能因互斥量优先级继承而不同于 init_priority，调度器按它选择线程。
 *
 * @param thread 待读取线程。
 * @return 当前有效优先级，数值越小越高。
 * @note 调用者必须持有调度锁。
 */
rt_uint8_t rt_sched_thread_get_curr_prio(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    return RT_SCHED_PRIV(thread).current_priority;
}

/**
 * @brief 取得线程配置的基础（初始）优先级。
 *
 * init_priority 是优先级继承结束后恢复的基准，本函数只读该稳定字段，因此不要求
 * 调度锁。
 *
 * @param thread 待读取线程。
 * @return 线程基础优先级。
 */
rt_uint8_t rt_sched_thread_get_init_prio(struct rt_thread *thread)
{
    /* 该字段在此语义下按只读配置字段使用，故无需调度锁。 */
    return RT_SCHED_PRIV(thread).init_priority;
}

/**
 * @brief 判断线程状态是否包含完整的挂起标志组合。
 *
 * @param thread 待判断线程。
 *
 * @return 与 RT_THREAD_SUSPEND_MASK 完全匹配时为 1，否则为 0。
 * @note 调用者必须持有调度锁。
 */
rt_uint8_t rt_sched_thread_is_suspended(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    return (RT_SCHED_CTX(thread).stat & RT_THREAD_SUSPEND_MASK) == RT_THREAD_SUSPEND_MASK;
}

/**
 * @brief 把线程调度状态设置为 CLOSE。
 *
 * 这里只改状态；从队列摘除、僵尸入队和资源回收由线程退出路径的其他步骤负责。
 *
 * @param thread 即将永久关闭的线程。
 * @return 始终返回 RT_EOK。
 * @note 调用者必须持有调度锁。
 */
rt_err_t rt_sched_thread_close(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    RT_SCHED_CTX(thread).stat = RT_THREAD_CLOSE;
    return RT_EOK;
}

/**
 * @brief 标记线程主动让出本轮时间片。
 *
 * remaining_tick 立即重装为 init_tick，供线程下次获得 CPU 使用；YIELD 标志告诉
 * 就绪队列操作把它放到同优先级队列的合适位置，让同优先级伙伴先运行。
 *
 * @param thread 要让出 CPU 的线程。
 * @return 始终返回 RT_EOK。
 * @note 调用者必须持有调度锁；本函数只标记，真正切换由后续 reschedule 完成。
 */
rt_err_t rt_sched_thread_yield(struct rt_thread *thread)
{
    RT_SCHED_DEBUG_IS_LOCKED;

    RT_SCHED_PRIV(thread).remaining_tick = RT_SCHED_PRIV(thread).init_tick;
    RT_SCHED_CTX(thread).stat |= RT_THREAD_STAT_YIELD;

    return RT_EOK;
}

/**
 * @brief 把一个挂起线程安全地恢复到就绪队列。
 *
 * 唤醒与超时 ISR 可能同时争夺同一线程。先确认它仍处于 SUSPEND；若登记了超时
 * 定时器，则必须先成功停止定时器，才能从等待链表摘除并插入就绪队列。任一步发现
 * 状态已由竞争者改变就返回错误，让调用者放弃重复唤醒。Smart 的 wakeup_handle
 * 同时清空，表示一次阻塞等待已经结束。
 *
 * @param thread 要唤醒的挂起线程。
 * @return 成功返回 RT_EOK；非挂起返回 -RT_EINVAL；停止定时器失败时传播其错误码。
 * @note 调用者必须持有调度锁。
 */
rt_err_t rt_sched_thread_ready(struct rt_thread *thread)
{
    rt_err_t error;

    RT_SCHED_DEBUG_IS_LOCKED;

    if (!rt_sched_thread_is_suspended(thread))
    {
        /* 已由超时或另一唤醒者处理时，不得再次操作其链表节点。 */
        error = -RT_EINVAL;
    }
    else
    {
        if (RT_SCHED_CTX(thread).sched_flag_ttmr_set)
        {
            /*
             * 先让超时源静默。停止失败可能表示 ISR 正在唤醒线程，此时继续摘链会
             * 导致重复插入或链表损坏，所以必须停止当前路径。
             */
            error = rt_sched_thread_timer_stop(thread);
        }
        else
        {
            error = RT_EOK;
        }

        if (!error)
        {
            /* 从 IPC/延时等待链表摘除，同一 list 节点随后将用于就绪队列。 */
            rt_list_remove(&RT_THREAD_LIST_NODE(thread));

#ifdef RT_USING_SMART
            thread->wakeup_handle.func = RT_NULL;
#endif

            /* 插入与其当前优先级对应的就绪队列。 */
            rt_sched_insert_thread(thread);
        }
    }

    return error;
}

/**
 * @brief 按经过的节拍数扣减当前线程时间片。
 *
 * 在调度锁内做饱和减法，避免批量 tick 大于剩余时间片时发生无符号下溢。若仍有
 * 时间片，只解锁；若耗尽，则重装时间片、置 YIELD 并提出调度请求。时钟 ISR 中的
 * 请求不会在锁内强行切换，而由中断退出路径在安全时机兑现。
 *
 * @param tick 本次经过的节拍数。
 * @return 始终返回 RT_EOK。
 */
rt_err_t rt_sched_tick_increase(rt_tick_t tick)
{
    struct rt_thread *thread;
    rt_sched_lock_level_t slvl;

    thread = rt_thread_self();

    rt_sched_lock(&slvl);

    if (RT_SCHED_PRIV(thread).remaining_tick > tick)
    {
        RT_SCHED_PRIV(thread).remaining_tick -= tick;
    }
    else
    {
        RT_SCHED_PRIV(thread).remaining_tick = 0;
    }

    if (RT_SCHED_PRIV(thread).remaining_tick)
    {
        rt_sched_unlock(slvl);
    }
    else
    {
        rt_sched_thread_yield(thread);

        /* 即使当前在 ISR，也先登记请求；最外层中断退出后才真正切换。 */
        rt_sched_unlock_n_resched(slvl);
    }

    return RT_EOK;
}

/**
 * @brief 更新线程优先级及其就绪位图属性。
 *
 * READY 线程的链表位置和就绪位图由优先级决定，因此必须先从旧队列删除，更新字段
 * 后再插回新队列；非 READY 线程不在就绪队列，只需改字段。优先级超过 32 级时使用
 * 两级位图：number 选择 8 优先级一组，number_mask 标记组，high_mask 标记组内位；
 * 32 级以内则一个 number_mask 直接对应优先级。
 *
 * @param thread 要调整的线程。
 * @param priority 新优先级，必须小于 RT_THREAD_PRIORITY_MAX，数值越小越高。
 * @param update_init_prio 为 RT_TRUE 时同时改变基础优先级；否则只改当前有效优先级，
 *                         适合优先级继承等临时调整。
 * @return 始终返回 RT_EOK。
 * @note 调用者必须持有调度锁，且线程状态必须有效。
 */
static rt_err_t _rt_sched_update_priority(struct rt_thread *thread, rt_uint8_t priority, rt_bool_t update_init_prio)
{
    RT_ASSERT(priority < RT_THREAD_PRIORITY_MAX);
    RT_SCHED_DEBUG_IS_LOCKED;

    /* READY 线程必须迁移队列；其他状态没有就绪链表位置可迁移。 */
    if ((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_READY)
    {
        /* 旧位图和旧优先级链表仍依赖旧属性，必须先摘除。 */
        rt_sched_remove_thread(thread);

        /* 可选地更新长期基准，并始终更新本次调度使用的有效优先级。 */
        if (update_init_prio)
        {
            RT_SCHED_PRIV(thread).init_priority = priority;
        }
        RT_SCHED_PRIV(thread).current_priority = priority;

        /* 重算查找最高优先级所需的位图缓存。 */
#if RT_THREAD_PRIORITY_MAX > 32
        RT_SCHED_PRIV(thread).number = RT_SCHED_PRIV(thread).current_priority >> 3;               /* 高 5 位选择分组。 */
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).number;
        RT_SCHED_PRIV(thread).high_mask = 1 << (RT_SCHED_PRIV(thread).current_priority & 0x07);   /* 低 3 位选择组内优先级。 */
#else
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).current_priority;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
        RT_SCHED_CTX(thread).stat = RT_THREAD_INIT;

        /* 以新优先级重新进入就绪集合。 */
        rt_sched_insert_thread(thread);
    }
    else
    {
        if (update_init_prio)
        {
            RT_SCHED_PRIV(thread).init_priority = priority;
        }
        RT_SCHED_PRIV(thread).current_priority = priority;

        /* 虽然尚未就绪，也要预先准备其未来入队所需的位图属性。 */
#if RT_THREAD_PRIORITY_MAX > 32
        RT_SCHED_PRIV(thread).number = RT_SCHED_PRIV(thread).current_priority >> 3;               /* 高 5 位选择分组。 */
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).number;
        RT_SCHED_PRIV(thread).high_mask = 1 << (RT_SCHED_PRIV(thread).current_priority & 0x07);   /* 低 3 位选择组内优先级。 */
#else
        RT_SCHED_PRIV(thread).number_mask = 1 << RT_SCHED_PRIV(thread).current_priority;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    }

    return RT_EOK;
}

/**
 * @brief 仅更新目标线程当前有效优先级，保留其基础优先级。
 */
rt_err_t rt_sched_thread_change_priority(struct rt_thread *thread, rt_uint8_t priority)
{
    return _rt_sched_update_priority(thread, priority, RT_FALSE);
}

/**
 * @brief 同时重设目标线程的基础优先级和当前有效优先级。
 */
rt_err_t rt_sched_thread_reset_priority(struct rt_thread *thread, rt_uint8_t priority)
{
    return _rt_sched_update_priority(thread, priority, RT_TRUE);
}

#ifdef RT_USING_OVERFLOW_CHECK

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static rt_err_t (*rt_stack_overflow_hook)(struct rt_thread *thread);

/**
 * @brief 设置检测到线程栈越界时调用的钩子。
 *
 * @param hook 接收出错线程的回调；传 RT_NULL 取消。返回 RT_EOK 表示调用者声称已
 *             处理并允许系统继续，其他返回值会让默认路径停在死循环中。
 *
 * @note 钩子的上下文取决于栈检查发生处，可能位于调度或中断相关关键路径；它必须
 *       短小且不可阻塞。栈已经损坏时继续运行风险很高，通常只用于记录诊断、触发
 *       受控复位或平台特定恢复。
 *
 * @see rt_scheduler_stack_check()
 */
void rt_scheduler_stack_overflow_sethook(rt_err_t (*hook)(struct rt_thread *thread))
{
    rt_stack_overflow_hook = hook;
}
#endif /* RT_USING_HOOK */

/**
 * @brief 检查线程栈是否越界，并在接近边界时发出警告。
 *
 * 未启用硬件栈保护时，同时检查初始化时写入边界的 '#' 哨兵和保存的 SP 是否位于
 * [stack_addr, stack_addr + stack_size] 合法区间。栈向上/向下增长决定哨兵位于哪端。
 * 一旦确认越界，先记录错误并调用可选钩子；钩子未返回 RT_EOK 时停机，避免继续
 * 使用已破坏内存。未越界但 SP 距危险端很近时只告警。
 *
 * Smart 无 MMU 的线程在用户数据区中使用用户栈时，内核线程栈边界不适用于当前
 * SP，因此跳过检查。启用硬件栈保护后，破坏哨兵的检查交给硬件，只保留 SP 预警。
 *
 * @param thread 要检查的线程；不得为 RT_NULL。
 */
void rt_scheduler_stack_check(struct rt_thread *thread)
{
    RT_ASSERT(thread != RT_NULL);

#ifdef RT_USING_SMART
#ifndef ARCH_MM_MMU
    struct rt_lwp *lwp = thread ? (struct rt_lwp *)thread->lwp : 0;

    /* 当前 SP 位于用户数据/用户栈范围时，不能用内核栈边界判定它。 */
    if (lwp && ((rt_uint32_t)thread->sp > (rt_uint32_t)lwp->data_entry &&
                (rt_uint32_t)thread->sp <= (rt_uint32_t)lwp->data_entry + (rt_uint32_t)lwp->data_size))
    {
        return;
    }
#endif /* 未定义 ARCH_MM_MMU */
#endif /* RT_USING_SMART */

#ifndef RT_USING_HW_STACK_GUARD
#ifdef ARCH_CPU_STACK_GROWS_UPWARD
    if (*((rt_uint8_t *)((rt_uintptr_t)thread->stack_addr + thread->stack_size - 1)) != '#' ||
#else
    if (*((rt_uint8_t *)thread->stack_addr) != '#' ||
#endif /* ARCH_CPU_STACK_GROWS_UPWARD */
        (rt_uintptr_t)thread->sp <= (rt_uintptr_t)thread->stack_addr ||
        (rt_uintptr_t)thread->sp >
            (rt_uintptr_t)thread->stack_addr + (rt_uintptr_t)thread->stack_size)
    {
        rt_base_t dummy = 1;
        rt_err_t hook_result = -RT_ERROR;

        LOG_E("thread:%s stack overflow\n", thread->parent.name);

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
        if (rt_stack_overflow_hook != RT_NULL)
        {
            hook_result = rt_stack_overflow_hook(thread);
        }
#endif /* RT_USING_HOOK */

        /* 只有钩子明确返回 RT_EOK 才冒险继续，否则停机保护现场。 */
        if (hook_result != RT_EOK)
        {
            while (dummy)
                ;
        }
    }
#endif /* RT_USING_HW_STACK_GUARD */
#ifdef ARCH_CPU_STACK_GROWS_UPWARD
#ifndef RT_USING_HW_STACK_GUARD
    else if ((rt_uintptr_t)thread->sp > ((rt_uintptr_t)thread->stack_addr + thread->stack_size))
#else
    if ((rt_uintptr_t)thread->sp > ((rt_uintptr_t)thread->stack_addr + thread->stack_size))
#endif
    {
        LOG_W("warning: %s stack is close to the top of stack address.\n",
              thread->parent.name);
    }
#else
#ifndef RT_USING_HW_STACK_GUARD
    else if ((rt_uintptr_t)thread->sp <= ((rt_uintptr_t)thread->stack_addr + 32))
#else
    if ((rt_uintptr_t)thread->sp <= ((rt_uintptr_t)thread->stack_addr + 32))
#endif
    {
        LOG_W("warning: %s stack is close to end of stack address.\n",
              thread->parent.name);
    }
#endif /* ARCH_CPU_STACK_GROWS_UPWARD */
}

#endif /* RT_USING_OVERFLOW_CHECK */
