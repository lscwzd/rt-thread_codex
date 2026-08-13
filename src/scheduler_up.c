/*
 * Copyright (c) 2006-2025 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-17     Bernard      the first version
 * 2006-04-28     Bernard      fix the scheduler algorthm
 * 2006-04-30     Bernard      add SCHEDULER_DEBUG
 * 2006-05-27     Bernard      fix the scheduler algorthm for same priority
 *                             thread schedule
 * 2006-06-04     Bernard      rewrite the scheduler algorithm
 * 2006-08-03     Bernard      add hook support
 * 2006-09-05     Bernard      add 32 priority level support
 * 2006-09-24     Bernard      add rt_system_scheduler_start function
 * 2009-09-16     Bernard      fix _rt_scheduler_stack_check
 * 2010-04-11     yi.qiu       add module feature
 * 2010-07-13     Bernard      fix the maximal number of rt_scheduler_lock_nest
 *                             issue found by kuronca
 * 2010-12-13     Bernard      add defunct list initialization even if not use heap.
 * 2011-05-10     Bernard      clean scheduler debug log.
 * 2013-12-21     Grissiom     add rt_critical_level
 * 2018-11-22     Jesven       remove the current task from ready queue
 *                             add per cpu ready queue
 *                             add _scheduler_get_highest_priority_thread to find highest priority task
 *                             rt_schedule_insert_thread won't insert current task to ready queue
 *                             in smp version, rt_hw_context_switch_interrupt maybe switch to
 *                             new task directly
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to scheduler.c
 * 2023-03-27     rose_man     Split into scheduler upc and scheduler_mp.c
 * 2023-10-17     ChuShicheng  Modify the timing of clearing RT_THREAD_STAT_YIELD flag bits
 * 2025-08-04     Pillar       Add rt_scheduler_critical_switch_flag
 * 2025-08-20     RyanCW       rt_scheduler_lock_nest use atomic operations
 * 2025-09-20     wdfk_prog    fix scheduling exception caused by interrupt preemption in rt_schedule
 */

/**
 * @file scheduler_up.c
 * @brief 单核（UP）固定优先级抢占调度器的就绪队列、位图和上下文切换实现。
 *
 * 每个优先级对应 rt_thread_priority_table 中的一条双向链表；同优先级线程按链表
 * 顺序轮转。rt_thread_ready_priority_group 是快速判断“哪些优先级非空”的位图，
 * __rt_ffs() 找到最低置位，恰好对应数值最小、实际最高的优先级。超过 32 个优先级
 * 时再用 rt_thread_ready_table 构成两级位图，从而仍能常数时间找到最高优先级。
 *
 * UP 中保护调度数据只需关闭本 CPU 中断。rt_sched_lock() 是非常短的底层锁；
 * rt_enter_critical() 则增加可嵌套的“禁止调度”计数，但不会一直关闭中断。若锁住
 * 调度时出现更高优先级线程，rt_schedule() 只记录 pending 标志，最外层退出临界区
 * 后再完成切换。
 */

#define __RT_IPC_SOURCE__
#include <rtthread.h>
#include <rthw.h>

#define DBG_TAG           "kernel.scheduler"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

/* 每个优先级一条就绪链表；表头本身不代表线程。 */
rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
/* 一级非空位图：最低置位代表当前最高就绪优先级或优先级组。 */
rt_uint32_t rt_thread_ready_priority_group;
#if RT_THREAD_PRIORITY_MAX > 32
/* 最多 256 级时，每个字节表示一组 8 个具体优先级。 */
rt_uint8_t rt_thread_ready_table[32];
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

/* 可嵌套的禁止调度计数；0 表示允许立即调度。 */
static rt_atomic_t rt_scheduler_lock_nest;
rt_uint8_t rt_current_priority;

/* 临界区中曾请求调度时置 1，最后一层退出时消费并清零。 */
static rt_int8_t rt_scheduler_critical_switch_flag;
#define IS_CRITICAL_SWITCH_PEND()  (rt_scheduler_critical_switch_flag == 1)
#define SET_CRITICAL_SWITCH_FLAG() (rt_scheduler_critical_switch_flag = 1)
#define CLR_CRITICAL_SWITCH_FLAG() (rt_scheduler_critical_switch_flag = 0)

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_scheduler_hook)(struct rt_thread *from, struct rt_thread *to);
static void (*rt_scheduler_switch_hook)(struct rt_thread *tid);

/**
 * @addtogroup group_hook
 */

/**@{*/

/**
 * @brief 设置线程选择发生变化时的调度钩子。
 *
 * 钩子在关中断的调度关键区中、更新 current_thread 之后、真正保存/恢复上下文之前
 * 调用，参数同时给出离开和进入线程。必须极短且不可阻塞；传 RT_NULL 可取消。
 *
 * @param hook 形如 hook(from, to) 的回调函数。
 */
void rt_scheduler_sethook(void (*hook)(struct rt_thread *from, struct rt_thread *to))
{
    rt_scheduler_hook = hook;
}

/**
 * @brief 设置普通线程上下文切换前的 switch 钩子。
 *
 * 本 UP 实现在非中断切换前调用它，传入即将被切出的 from 线程。钩子位于中断关闭
 * 且调度器内部状态正在转换的上下文，不能阻塞或重新进入调度器。再次设置会覆盖。
 *
 * @param hook 接收被切出线程的回调；RT_NULL 表示取消。
 */
void rt_scheduler_switch_sethook(void (*hook)(struct rt_thread *tid))
{
    rt_scheduler_switch_hook = hook;
}

/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_thread_management
 *
 * @cond
 *
 * @{
 */

static struct rt_thread* _scheduler_get_highest_priority_thread(rt_ubase_t *highest_prio)
{
    struct rt_thread *highest_priority_thread;
    rt_ubase_t highest_ready_priority;

#if RT_THREAD_PRIORITY_MAX > 32
    rt_ubase_t number;

    /* 先找非空的 8 级分组，再在该组内找最低置位。__rt_ffs 返回从 1 开始的位置。 */
    number = __rt_ffs(rt_thread_ready_priority_group) - 1;
    highest_ready_priority = (number << 3) + __rt_ffs(rt_thread_ready_table[number]) - 1;
#else
    highest_ready_priority = __rt_ffs(rt_thread_ready_priority_group) - 1;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* 同优先级链表的 next 指向本次应最先运行的线程。 */
    highest_priority_thread = RT_THREAD_LIST_NODE_ENTRY(rt_thread_priority_table[highest_ready_priority].next);

    *highest_prio = highest_ready_priority;

    return highest_priority_thread;
}

/**
 * @brief 通过关闭中断锁住 UP 调度器，并保存原中断状态。
 *
 * 这是保护一次短小调度数据操作的底层锁，不等同于可跨多层函数使用的
 * rt_enter_critical()。关闭本 CPU 中断后，不会有 ISR 或其他线程抢占当前修改。
 *
 * @param plvl 输出加锁前的中断状态。
 * @return 成功返回 RT_EOK；@p plvl 为 RT_NULL 时返回 -RT_EINVAL。
 * @note 必须与 rt_sched_unlock() 或 rt_sched_unlock_n_resched() 成对使用。
 */
rt_err_t rt_sched_lock(rt_sched_lock_level_t *plvl)
{
    rt_base_t level;
    if (!plvl)
        return -RT_EINVAL;

    level = rt_hw_interrupt_disable();
    *plvl = level;

    return RT_EOK;
}

/**
 * @brief 恢复 rt_sched_lock() 保存的中断状态。
 *
 * @param level 配对的 rt_sched_lock() 返回到输出参数中的原状态。
 * @return 始终返回 RT_EOK。
 * @note 必须严格配对，不能自行构造 @p level。
 */
rt_err_t rt_sched_unlock(rt_sched_lock_level_t level)
{
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

/**
 * @brief 请求一次调度，然后恢复加锁前的中断状态。
 *
 * 若 current_thread 已建立，先在中断仍保持关闭的情况下进入 rt_schedule()，以免
 * 解锁和调度之间出现新的竞态；启动早期没有当前线程时跳过。最后恢复原中断状态。
 *
 * @param level 配对 rt_sched_lock() 保存的状态。
 * @return 始终返回 RT_EOK。
 */
rt_err_t rt_sched_unlock_n_resched(rt_sched_lock_level_t level)
{
    if (rt_thread_self())
    {
        /* current_thread 非空表示首次调度已经建立运行上下文。 */
        rt_schedule();
    }
    rt_hw_interrupt_enable(level);

    return RT_EOK;
}

/**
 * @brief 初始化单核调度器的全部全局数据。
 *
 * 清零临界区嵌套计数，为每个优先级初始化空链表，并清空一级/二级就绪位图。调用后
 * 还没有就绪线程；后续线程 startup 才逐一填充这些结构。
 *
 * @note 必须在创建可调度线程和启动调度器前调用一次。
 */
void rt_system_scheduler_init(void)
{
    rt_base_t offset;
    rt_scheduler_lock_nest = 0;

    LOG_D("start scheduler: max priority 0x%02x",
          RT_THREAD_PRIORITY_MAX);

    for (offset = 0; offset < RT_THREAD_PRIORITY_MAX; offset ++)
    {
        rt_list_init(&rt_thread_priority_table[offset]);
    }

    /* 没有任何非空优先级。 */
    rt_thread_ready_priority_group = 0;

#if RT_THREAD_PRIORITY_MAX > 32
    /* 两级位图的所有具体优先级也均为空。 */
    rt_memset(rt_thread_ready_table, 0, sizeof(rt_thread_ready_table));
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
}

/**
 * @brief 选择最高优先级就绪线程并完成系统第一次上下文切换。
 *
 * 启动代码已保证至少空闲线程在就绪集合中，因此可以直接取链表首线程。选中后写入
 * CPU 的 current_thread，清除启动期间遗留的 pending 标志，从就绪队列摘除并置为
 * RUNNING。rt_hw_context_switch_to() 不需要保存启动栈，只恢复目标线程初始上下文。
 *
 * @note 正常情况下不返回；必须在调度器初始化且至少一个线程就绪后调用。
 */
void rt_system_scheduler_start(void)
{
    struct rt_thread *to_thread;
    rt_ubase_t highest_ready_priority;

    to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);

    rt_cpu_self()->current_thread = to_thread;

    /* 启动边界不继承此前尚无运行线程时产生的调度请求。 */
    CLR_CRITICAL_SWITCH_FLAG();

    rt_sched_remove_thread(to_thread);
    RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING;

    /* 首次切换只有“to”上下文，不再返回启动调用栈。 */

    rt_hw_context_switch_to((rt_uintptr_t)&to_thread->sp);

    /* 正常执行永远到不了这里。 */
}

/**
 * @brief 执行一次最高优先级抢占决策，并在需要时切换上下文。
 *
 * 决策规则：只有就绪位图非空才查看候选线程；若当前 RUNNING 线程优先级更高，继续
 * 运行；优先级相同且当前没有 YIELD，也继续运行；否则候选线程获胜，当前仍可运行
 * 时重新插入就绪队列。目标线程从就绪队列摘除并置 RUNNING。
 *
 * 普通线程上下文调用 rt_hw_context_switch() 立即保存 from、恢复 to；ISR 中则调用
 * rt_hw_context_switch_interrupt() 登记中断退出时的切换。返回到某线程后还会检查它
 * 的 SIGNAL_PENDING 标志，在开中断的线程上下文处理信号。
 *
 * 若 rt_scheduler_lock_nest 非零，不能更改运行线程，只设置 critical switch pending，
 * 最外层 rt_exit_critical() 会重新调用本函数。
 */
void rt_schedule(void)
{
    rt_base_t level;
    struct rt_thread *to_thread;
    struct rt_thread *from_thread;
    struct rt_thread *curr_thread;
    rt_base_t interrupt_nest;

    /* 整个选择和队列迁移过程必须相对于 ISR 原子。 */
    level = rt_hw_interrupt_disable();

    /* 保存当前线程快照，避免决策期间重复读取 CPU 字段。 */
    curr_thread = rt_thread_self();

    /* 临界区嵌套为 0 才允许真正切换。 */
    if (rt_scheduler_lock_nest == 0)
    {
        rt_ubase_t highest_ready_priority;

        if (rt_thread_ready_priority_group != 0)
        {
            /* 当前线程仍可运行但被换出时，需要重新放回就绪集合。 */
            int need_insert_from_thread = 0;

            to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);

            if ((RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_RUNNING)
            {
                if (RT_SCHED_PRIV(curr_thread).current_priority < highest_ready_priority)
                {
                    to_thread = curr_thread;
                }
                else if (RT_SCHED_PRIV(curr_thread).current_priority == highest_ready_priority
                         && (RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_YIELD_MASK) == 0)
                {
                    to_thread = curr_thread;
                }
                else
                {
                    need_insert_from_thread = 1;
                }
            }

            if (to_thread != curr_thread)
            {
                /* 候选者胜出：先提交内核调度状态，再执行架构上下文切换。 */
                rt_current_priority = (rt_uint8_t)highest_ready_priority;
                from_thread                   = curr_thread;
                rt_cpu_self()->current_thread = to_thread;

                RT_OBJECT_HOOK_CALL(rt_scheduler_hook, (from_thread, to_thread));

                if (need_insert_from_thread)
                {
                    rt_sched_insert_thread(from_thread);
                }

                if ((RT_SCHED_CTX(from_thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
                {
                    RT_SCHED_CTX(from_thread).stat &= ~RT_THREAD_STAT_YIELD_MASK;
                }

                rt_sched_remove_thread(to_thread);
                RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(to_thread).stat & ~RT_THREAD_STAT_MASK);

                /* 根据是否处于中断上下文选择两种架构切换入口。 */
                interrupt_nest = rt_interrupt_get_nest();
                LOG_D("[%d]switch to priority#%d "
                         "thread:%.*s(sp:0x%08x), "
                         "from thread:%.*s(sp: 0x%08x)",
                         interrupt_nest, highest_ready_priority,
                         RT_NAME_MAX, to_thread->parent.name, to_thread->sp,
                         RT_NAME_MAX, from_thread->parent.name, from_thread->sp);

                RT_SCHEDULER_STACK_CHECK(to_thread);

                if (interrupt_nest == 0)
                {
                    extern void rt_thread_handle_sig(rt_bool_t clean_state);

                    RT_OBJECT_HOOK_CALL(rt_scheduler_switch_hook, (from_thread));

                    rt_hw_context_switch((rt_uintptr_t)&from_thread->sp,
                            (rt_uintptr_t)&to_thread->sp);

                    /* 运行到这里时已经是在日后恢复的线程上下文中。 */
                    rt_hw_interrupt_enable(level);

#ifdef RT_USING_SIGNALS
                    /* 原子地消费 pending 标志，再开中断执行可能较复杂的信号处理。 */
                    level = rt_hw_interrupt_disable();
                    if (RT_SCHED_CTX(curr_thread).stat & RT_THREAD_STAT_SIGNAL_PENDING)
                    {
                        extern void rt_thread_handle_sig(rt_bool_t clean_state);

                        RT_SCHED_CTX(curr_thread).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;

                        rt_hw_interrupt_enable(level);

                        /* clean_state 表示处理结束后恢复干净的信号状态。 */
                        rt_thread_handle_sig(RT_TRUE);
                    }
                    else
                    {
                        rt_hw_interrupt_enable(level);
                    }
#endif /* RT_USING_SIGNALS */
                    goto __exit;
                }
                else
                {
                    LOG_D("switch in interrupt");

                    rt_hw_context_switch_interrupt((rt_uintptr_t)&from_thread->sp,
                            (rt_uintptr_t)&to_thread->sp, from_thread, to_thread);
                }
            }
            else
            {
                rt_sched_remove_thread(curr_thread);
                RT_SCHED_CTX(curr_thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(curr_thread).stat & ~RT_THREAD_STAT_MASK);
            }
        }
    }
    else
    {
        SET_CRITICAL_SWITCH_FLAG();
    }

    /* 无切换或中断式延迟切换路径在此恢复进入函数前的中断状态。 */
    rt_hw_interrupt_enable(level);

__exit:
    return;
}

/**
 * @brief 为首次启动计算线程的就绪位图属性，并把它变为可恢复的挂起态。
 *
 * 超过 32 级时，优先级高位选择 8 级分组（number/number_mask），低 3 位形成组内
 * high_mask；不超过 32 级时一个 number_mask 足够。最后置 SUSPEND，使统一的
 * resume/ready 路径能将新线程第一次插入就绪队列。
 *
 * @param thread 已初始化但尚未启动的线程。
 * @note 创建阶段通常没有竞争者，所以本函数不获取调度锁。
 */
void rt_sched_thread_startup(struct rt_thread *thread)
{
#if RT_THREAD_PRIORITY_MAX > 32
    RT_SCHED_PRIV(thread).number = RT_SCHED_PRIV(thread).current_priority >> 3;            /* 高 5 位选择分组。 */
    RT_SCHED_PRIV(thread).number_mask = 1L << RT_SCHED_PRIV(thread).number;
    RT_SCHED_PRIV(thread).high_mask = 1L << (RT_SCHED_PRIV(thread).current_priority & 0x07);  /* 低 3 位选择组内优先级。 */
#else
    RT_SCHED_PRIV(thread).number_mask = 1L << RT_SCHED_PRIV(thread).current_priority;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* startup 随后借用“恢复挂起线程”的路径完成首次就绪。 */
    RT_SCHED_CTX(thread).stat = RT_THREAD_SUSPEND;
}

/**
 * @brief 初始化线程私有的 UP 调度字段。
 *
 * 初始化可复用的调度链表节点；令 init_priority 与 current_priority 相同；暂时清零
 * 位图缓存，因为 INIT 线程不会加入调度队列；最后保存初始时间片并装满剩余时间片。
 *
 * @param thread 线程控制块。
 * @param tick 初始时间片长度。
 * @param priority 初始优先级，必须小于 RT_THREAD_PRIORITY_MAX。
 */
void rt_sched_thread_init_priv(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority)
{
    rt_list_init(&RT_THREAD_LIST_NODE(thread));

    /* 创建时尚未发生优先级继承，基础值和有效值相同。 */
    RT_ASSERT(priority < RT_THREAD_PRIORITY_MAX);
    RT_SCHED_PRIV(thread).init_priority    = priority;
    RT_SCHED_PRIV(thread).current_priority = priority;

    /* INIT 线程不在就绪集合，真正 startup 时才计算掩码。 */
    RT_SCHED_PRIV(thread).number_mask = 0;
#if RT_THREAD_PRIORITY_MAX > 32
    RT_SCHED_PRIV(thread).number = 0;
    RT_SCHED_PRIV(thread).high_mask = 0;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* 初始剩余时间片为满额；耗尽或 yield 后重装 init_tick。 */
    RT_SCHED_PRIV(thread).init_tick = tick;
    RT_SCHED_PRIV(thread).remaining_tick = tick;
}

/**
 * @brief 把线程加入系统就绪队列，并维护对应优先级位图。
 *
 * 若传入的恰是当前线程，无需把 RUNNING 线程挂入就绪链表，只修正基本状态。其他
 * 线程置 READY 后按 YIELD 标志选择插入位置：已让出时间片的线程放到表头之前，
 * 即同优先级队尾；仍有时间片的唤醒线程放到表头之后，即同优先级队首，使其下次
 * 优先运行。最后置位该优先级（及分组）位图。
 *
 * @param thread 要变为就绪态的线程，不得为 RT_NULL。
 * @note 这是内核调度内部接口，应用代码不得直接调用；函数内部会短暂关闭中断。
 */
void rt_sched_insert_thread(struct rt_thread *thread)
{
    rt_base_t level;

    RT_ASSERT(thread != RT_NULL);

    /* 链表和位图必须作为一个不可分割的状态一起更新。 */
    level = rt_hw_interrupt_disable();

    /* 当前线程由 CPU 控制块持有，不应同时出现在普通就绪链表中。 */
    if (thread == rt_current_thread)
    {
        RT_SCHED_CTX(thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
        goto __exit;
    }

    /* 保留 YIELD/SIGNAL 等附加位，只替换基本状态。 */
    RT_SCHED_CTX(thread).stat = RT_THREAD_READY | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
    /* YIELD 表示本轮机会已用完，插到同优先级 FIFO 队尾。 */
    if((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
    {
        rt_list_insert_before(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                              &RT_THREAD_LIST_NODE(thread));
    }
    /* 普通唤醒仍有运行资格，插到同优先级队首。 */
    else
    {
        rt_list_insert_after(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                              &RT_THREAD_LIST_NODE(thread));
    }

    LOG_D("insert thread[%.*s], the priority: %d",
          RT_NAME_MAX, thread->parent.name, RT_SCHED_PRIV(rt_current_thread).current_priority);

    /* 先置具体优先级位，再置一级分组位，选择器即可看到该线程。 */
#if RT_THREAD_PRIORITY_MAX > 32
    rt_thread_ready_table[RT_SCHED_PRIV(thread).number] |= RT_SCHED_PRIV(thread).high_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    rt_thread_ready_priority_group |= RT_SCHED_PRIV(thread).number_mask;

__exit:
    /* 恢复调用前的中断状态，而不是无条件开中断。 */
    rt_hw_interrupt_enable(level);
}

/**
 * @brief 从系统就绪队列移除线程，并在队列变空时清除位图。
 *
 * 移除节点后，只有当该优先级链表已空，才能清除对应位；超过 32 级时还需检查整个
 * 8 级分组是否已空，只有组内字节为 0 才清一级分组位。这样链表与快速选择位图
 * 始终一致。
 *
 * @param thread 当前确实位于就绪队列中的线程。
 * @note 内核内部接口；重复移除或移除 RUNNING/挂起线程会破坏链表。
 */
void rt_sched_remove_thread(struct rt_thread *thread)
{
    rt_base_t level;

    RT_ASSERT(thread != RT_NULL);

    /* 防止 ISR 唤醒线程并同时修改相同队列/位图。 */
    level = rt_hw_interrupt_disable();

    LOG_D("remove thread[%.*s], the priority: %d",
          RT_NAME_MAX, thread->parent.name,
          RT_SCHED_PRIV(rt_current_thread).current_priority);

    /* 先摘节点，再根据摘除后的链表是否为空维护位图。 */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));
    if (rt_list_isempty(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority])))
    {
#if RT_THREAD_PRIORITY_MAX > 32
        rt_thread_ready_table[RT_SCHED_PRIV(thread).number] &= ~RT_SCHED_PRIV(thread).high_mask;
        if (rt_thread_ready_table[RT_SCHED_PRIV(thread).number] == 0)
        {
            rt_thread_ready_priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
        }
#else
        rt_thread_ready_priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    }

    /* 恢复进入函数前的中断状态。 */
    rt_hw_interrupt_enable(level);
}

#ifdef RT_DEBUGING_CRITICAL

static volatile int _critical_error_occurred = 0;

/**
 * @brief 在调试构建中核对临界区层数后安全退出一层。
 *
 * rt_enter_critical() 返回进入后的层数。正确的 LIFO 使用应在退出时仍看到相同层数；
 * 不匹配说明漏配对、跨层退出或错误保存了 level。检查期间关闭中断，首次错误打印
 * 当前层数、调用者层数和回溯后停机，避免继续破坏全局调度状态。
 *
 * @param critical_level 配对进入调用返回的预期层数。
 */
void rt_exit_critical_safe(rt_base_t critical_level)
{
    rt_base_t level;
    /* 保证层数检查与错误标志更新不可被 ISR 打断。 */
    level = rt_hw_interrupt_disable();

    if (!_critical_error_occurred)
    {
        if (critical_level != rt_scheduler_lock_nest)
        {
            int dummy = 1;
            _critical_error_occurred = 1;

            rt_kprintf("%s: un-compatible critical level\n" \
                       "\tCurrent %d\n\tCaller %d\n",
                       __func__, rt_scheduler_lock_nest,
                       critical_level);
            rt_backtrace();

            while (dummy) ;
        }
    }
    rt_hw_interrupt_enable(level);

    rt_exit_critical();
}

#else /* !RT_DEBUGING_CRITICAL */

/**
 * @brief 非调试构建的安全退出包装。
 *
 * 为保持相同 API 接收 @p critical_level，但非调试实现不验证它，只退出当前一层。
 * 若此前有延迟调度请求，最外层退出仍会由 rt_exit_critical() 触发调度。
 *
 * @param critical_level 非调试构建中未使用的预期层数。
 */
void rt_exit_critical_safe(rt_base_t critical_level)
{
    rt_exit_critical();
}

#endif/* RT_DEBUGING_CRITICAL */
RTM_EXPORT(rt_exit_critical_safe);

/**
 * @brief 进入一层禁止线程调度的临界区。
 *
 * 原子递增嵌套计数，但不会在整个临界区持续关闭硬件中断：ISR 仍可运行，只是其
 * 唤醒造成的线程切换会延后。该特性与 rt_hw_interrupt_disable() 的“禁止中断”
 * 不同，初学者不要混用二者语义。
 *
 * @return 进入后的新嵌套层数，可传给 rt_exit_critical_safe() 做配对检查。
 * @note 每次进入必须退出一次；持有期间不能执行会等待另一个线程的阻塞操作。
 */
rt_base_t rt_enter_critical(void)
{
    rt_base_t critical_level;

    critical_level = rt_atomic_add(&rt_scheduler_lock_nest, 1) + 1;
    return critical_level;
}
RTM_EXPORT(rt_enter_critical);

/**
 * @brief 退出一层禁止调度临界区，并在最外层处理延迟调度。
 *
 * 短暂关中断后递减嵌套。达到 0 时先恢复中断，再检查 pending 标志并调度；这样
 * rt_schedule() 使用自己完整的中断保护。仍大于 0 时只恢复中断，继续延迟切换。
 * 代码把小于 0 的异常也钳制为 0，但这不代表多余退出是合法的。
 *
 * @note 必须与 rt_enter_critical() 成对；只有完全解锁才可能切换线程。
 */
void rt_exit_critical(void)
{
    rt_base_t level;

    /* 保护嵌套计数和 pending 标志的联合判断。 */
    level = rt_hw_interrupt_disable();

    rt_scheduler_lock_nest --;
    if (rt_scheduler_lock_nest <= 0)
    {
        rt_scheduler_lock_nest = 0;
        /* 先恢复调用者中断状态，再进入调度器自己的保护区。 */
        rt_hw_interrupt_enable(level);

        if (IS_CRITICAL_SWITCH_PEND())
        {
            CLR_CRITICAL_SWITCH_FLAG();
            /* 临界区中积累的请求只在最外层退出时消费一次。 */
            rt_schedule();
        }
    }
    else
    {
        /* 仍在外层临界区，只恢复中断，不执行线程调度。 */
        rt_hw_interrupt_enable(level);
    }
}
RTM_EXPORT(rt_exit_critical);

/**
 * @brief 获取当前禁止调度临界区的嵌套层数。
 *
 * @return 当前层数；0 表示调度未被临界区锁定。
 */
rt_uint16_t rt_critical_level(void)
{
    return (rt_uint16_t)rt_atomic_load(&rt_scheduler_lock_nest);
}
RTM_EXPORT(rt_critical_level);

rt_err_t rt_sched_thread_bind_cpu(struct rt_thread *thread, int cpu)
{
    /* UP 没有可选择的目标 CPU，保留接口但始终报告参数/能力无效。 */
    return -RT_EINVAL;
}

/**
 * @} group_thread_management
 *
 * @endcond
 */
