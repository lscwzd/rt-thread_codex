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
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-12-10     xqyjlj       use rt_hw_spinlock
 * 2024-01-05     Shell        Fixup of data racing in rt_critical_level
 * 2024-01-18     Shell        support rt_sched_thread of scheduling status for better mt protection
 * 2024-01-18     Shell        support rt_hw_thread_self to improve overall performance
 */

/**
 * @file scheduler_mp.c
 * @brief SMP 固定优先级调度器：全局/本地就绪队列、跨核唤醒和上下文切换。
 *
 * 可迁移线程进入全局 rt_thread_priority_table；绑定到某个 CPU 的线程进入该 CPU
 * 自己的 priority_table。每次调度同时比较两边的最高优先级，数值更小者获胜。
 * 全局和每 CPU 队列都配有一级或两级位图，避免线性扫描所有优先级。
 *
 * 所有共享调度队列由 _mp_scheduler_lock 串行化，本地中断关闭则保护当前 CPU 的
 * 运行现场。普通唤醒可能通过 RT_SCHEDULE_IPI 通知其他 CPU 重新调度；ISR 中不能
 * 直接切换线程，只设置 irq_switch_flag，由最外层中断返回时调用
 * rt_scheduler_do_irq_switch()。每个线程的 critical_lock_nest 使“禁止调度”可嵌套，
 * 最外层退出时消费延迟切换标志。
 */

#include <rtthread.h>
#include <rthw.h>

#define DBG_TAG           "kernel.scheduler"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

/* 未绑定线程共享的每优先级就绪链表。 */
rt_list_t rt_thread_priority_table[RT_THREAD_PRIORITY_MAX];
/* 同时保护全局与各 CPU 就绪表、位图以及运行归属字段。 */
static struct rt_spinlock _mp_scheduler_lock;

#define SCHEDULER_LOCK_FLAG(percpu) ((percpu)->sched_lock_flag)

/* 当前线程的禁止调度嵌套层数；启动早期 curthr 可为空。 */
#define SCHEDULER_ENTER_CRITICAL(curthr)                    \
    do                                                      \
    {                                                       \
        if (curthr) RT_SCHED_CTX(curthr).critical_lock_nest++; \
    } while (0)

#define SCHEDULER_EXIT_CRITICAL(curthr)                     \
    do                                                      \
    {                                                       \
        if (curthr) RT_SCHED_CTX(curthr).critical_lock_nest--; \
    } while (0)

/*
 * CONTEXT_LOCK/UNLOCK 只处理全局调度自旋锁和每 CPU 调试标志，不重复管理
 * critical_lock_nest。调用者必须已经关闭本地中断，且严格配对。
 */
#define SCHEDULER_CONTEXT_LOCK(percpu)               \
    do                                               \
    {                                                \
        RT_ASSERT(SCHEDULER_LOCK_FLAG(percpu) == 0); \
        _fast_spin_lock(&_mp_scheduler_lock);        \
        SCHEDULER_LOCK_FLAG(percpu) = 1;             \
    } while (0)

#define SCHEDULER_CONTEXT_UNLOCK(percpu)             \
    do                                               \
    {                                                \
        RT_ASSERT(SCHEDULER_LOCK_FLAG(percpu) == 1); \
        SCHEDULER_LOCK_FLAG(percpu) = 0;             \
        _fast_spin_unlock(&_mp_scheduler_lock);      \
    } while (0)

/*
 * 完整底层调度锁顺序：关本地中断 -> 增加当前线程临界层数 -> 取得全局锁；解锁
 * 反向执行。固定顺序同时解决本 CPU ISR 竞态和其他 CPU 并发。
 */
#define SCHEDULER_LOCK(level)              \
    do                                     \
    {                                      \
        rt_thread_t _curthr;               \
        struct rt_cpu *_percpu;            \
        level = rt_hw_local_irq_disable(); \
        _percpu = rt_cpu_self();           \
        _curthr = _percpu->current_thread; \
        SCHEDULER_ENTER_CRITICAL(_curthr); \
        SCHEDULER_CONTEXT_LOCK(_percpu);   \
    } while (0)

#define SCHEDULER_UNLOCK(level)            \
    do                                     \
    {                                      \
        rt_thread_t _curthr;               \
        struct rt_cpu *_percpu;            \
        _percpu = rt_cpu_self();           \
        _curthr = _percpu->current_thread; \
        SCHEDULER_CONTEXT_UNLOCK(_percpu); \
        SCHEDULER_EXIT_CRITICAL(_curthr);  \
        rt_hw_local_irq_enable(level);     \
    } while (0)

#ifdef ARCH_USING_HW_THREAD_SELF
/* 架构能快速得到 current thread 时，把 pending 标志保存在随线程迁移的上下文中。 */
#define IS_CRITICAL_SWITCH_PEND(pcpu, curthr)  (RT_SCHED_CTX(curthr).critical_switch_flag)
#define SET_CRITICAL_SWITCH_FLAG(pcpu, curthr) (RT_SCHED_CTX(curthr).critical_switch_flag = 1)
#define CLR_CRITICAL_SWITCH_FLAG(pcpu, curthr) (RT_SCHED_CTX(curthr).critical_switch_flag = 0)

#else /* !ARCH_USING_HW_THREAD_SELF */
/* 否则标志保存在 per-CPU 控制块；线程不可在读取边界随意迁移。 */
#define IS_CRITICAL_SWITCH_PEND(pcpu, curthr)  ((pcpu)->critical_switch_flag)
#define SET_CRITICAL_SWITCH_FLAG(pcpu, curthr) ((pcpu)->critical_switch_flag = 1)
#define CLR_CRITICAL_SWITCH_FLAG(pcpu, curthr) ((pcpu)->critical_switch_flag = 0)

#endif /* ARCH_USING_HW_THREAD_SELF */

static rt_uint32_t rt_thread_ready_priority_group;
#if RT_THREAD_PRIORITY_MAX > 32
/* 最多 256 级时，每个字节记录一组 8 个全局具体优先级。 */
static rt_uint8_t rt_thread_ready_table[32];
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

/**
 * @brief 调度器已建立临界区时使用的轻量自旋锁入口。
 *
 * 它只操作底层硬件锁和调试所有者，不进入/退出普通调度临界区；否则调度器内部会
 * 重复增加嵌套层数。只允许本文件在已经关闭本地中断并保证调用次序的路径使用。
 */
rt_inline void _fast_spin_lock(struct rt_spinlock *lock)
{
    rt_hw_spin_lock(&lock->lock);

    RT_SPIN_LOCK_DEBUG(lock);
}

rt_inline void _fast_spin_unlock(struct rt_spinlock *lock)
{
    rt_base_t critical_level;
    RT_SPIN_UNLOCK_DEBUG(lock, critical_level);

    /* 调度器自己维护层数，故这里只保留调试宏输出但忽略其层数结果。 */
    RT_UNUSED(critical_level);

    rt_hw_spin_unlock(&lock->lock);
}

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_scheduler_hook)(struct rt_thread *from, struct rt_thread *to);
static void (*rt_scheduler_switch_hook)(struct rt_thread *tid);

/**
 * @addtogroup group_hook
 */

/**@{*/

/**
 * @brief 设置调度器选中不同线程时的钩子。
 *
 * 在全局调度锁和本地关中断保护下、真正上下文切换前调用，参数为 from/to。钩子
 * 可能在不同 CPU 并发执行，必须可重入、不可阻塞，也不能重新获取调度锁。
 *
 * @param hook 回调函数；RT_NULL 表示取消。
 */
void rt_scheduler_sethook(void (*hook)(struct rt_thread *from, struct rt_thread *to))
{
    rt_scheduler_hook = hook;
}

/**
 * @brief 设置架构上下文切换前的 switch 钩子。
 *
 * 当前实现传入被切出的线程。调用点仍持有调度上下文锁且关闭本地中断，钩子只适合
 * 做极短的跟踪/统计。SMP 下多个核可能同时调用。
 *
 * @param hook 回调函数；RT_NULL 表示取消。
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
 * @cond DOXYGEN_SMP
 *
 * @{
 */

#if RT_THREAD_PRIORITY_MAX > 32

rt_inline rt_base_t _get_global_highest_ready_prio(void)
{
    rt_ubase_t number;
    rt_ubase_t highest_ready_priority;

    /* __rt_ffs 从 1 起计数；减 1 后得到最低置位的 8 级分组下标。 */
    number = __rt_ffs(rt_thread_ready_priority_group) - 1;
    if (number != -1)
    {
        highest_ready_priority = (number << 3) + __rt_ffs(rt_thread_ready_table[number]) - 1;
    }
    else
    {
        highest_ready_priority = -1;
    }
    return highest_ready_priority;
}

rt_inline rt_base_t _get_local_highest_ready_prio(struct rt_cpu* pcpu)
{
    rt_ubase_t number;
    rt_ubase_t local_highest_ready_priority;

    number = __rt_ffs(pcpu->priority_group) - 1;
    if (number != -1)
    {
        local_highest_ready_priority = (number << 3) + __rt_ffs(pcpu->ready_table[number]) - 1;
    }
    else
    {
        local_highest_ready_priority = -1;
    }
    return local_highest_ready_priority;
}

#else /* if RT_THREAD_PRIORITY_MAX <= 32 */

rt_inline rt_base_t _get_global_highest_ready_prio(void)
{
    return __rt_ffs(rt_thread_ready_priority_group) - 1;
}

rt_inline rt_base_t _get_local_highest_ready_prio(struct rt_cpu* pcpu)
{
    return __rt_ffs(pcpu->priority_group) - 1;
}

#endif /* RT_THREAD_PRIORITY_MAX > 32 */

/*
 * 比较可迁移全局队列和当前 CPU 绑定队列的最高优先级。-1 以无符号最大值形式表示
 * 队列为空，因此另一边有线程时自然获胜；相等时选择本地队列，减少迁移和竞争。
 */
static struct rt_thread* _scheduler_get_highest_priority_thread(rt_ubase_t *highest_prio)
{
    struct rt_thread *highest_priority_thread;
    rt_ubase_t highest_ready_priority, local_highest_ready_priority;
    struct rt_cpu* pcpu = rt_cpu_self();

    highest_ready_priority = _get_global_highest_ready_prio();
    local_highest_ready_priority = _get_local_highest_ready_prio(pcpu);

    /* 数值更小代表实际优先级更高。 */
    if (highest_ready_priority < local_highest_ready_priority)
    {
        *highest_prio = highest_ready_priority;

        highest_priority_thread = RT_THREAD_LIST_NODE_ENTRY(
            rt_thread_priority_table[highest_ready_priority].next);
    }
    else
    {
        *highest_prio = local_highest_ready_priority;
        if (local_highest_ready_priority != -1)
        {
            highest_priority_thread = RT_THREAD_LIST_NODE_ENTRY(
                pcpu->priority_table[local_highest_ready_priority].next);
        }
        else
        {
            highest_priority_thread = RT_NULL;
        }
    }

    RT_ASSERT(!highest_priority_thread ||
              rt_object_get_type(&highest_priority_thread->parent) == RT_Object_Class_Thread);
    return highest_priority_thread;
}

/**
 * @brief 在持锁条件下把线程置 READY 并插入合适的全局或本地队列。
 *
 * READY 线程说明已经入队，直接返回；oncpu 非 DETACHED 说明它仍在某 CPU 运行，
 * 此 API 不能把 RUNNING 线程重复入队，只修复基本状态。真正待入队线程按 bind_cpu
 * 选择全局队列或目标 CPU 本地队列，并按 YIELD 决定同优先级队首/队尾。
 *
 * 全局可迁移线程可能改善任一其他 CPU 的选择，因此向除本 CPU 外所有核发 IPI；
 * 绑定线程只需在目标不是本 CPU 时通知目标核。
 *
 * @param thread 要变为就绪态的线程。
 * @note 调用者必须持有 `_mp_scheduler_lock` 且本地中断已关闭。
 */
static void _sched_insert_thread_locked(struct rt_thread *thread)
{
    int cpu_id;
    int bind_cpu;
    rt_uint32_t cpu_mask;

    if ((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_READY)
    {
        /* 幂等保护：避免同一 list 节点重复链接。 */
        return ;
    }
    else if (RT_SCHED_CTX(thread).oncpu != RT_CPU_DETACHED)
    {
        /*
         * 本接口只允许 YIELD/SUSPEND -> READY；oncpu 表明它实际仍在运行，因此不能
         * 入队，只把基本状态恢复为 RUNNING 并保留附加标志。
         */
        RT_SCHED_CTX(thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
        return ;
    }

    /* 保留 YIELD/SIGNAL 等附加位，仅替换基本状态。 */
    RT_SCHED_CTX(thread).stat = RT_THREAD_READY | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);

    cpu_id   = rt_hw_cpu_id();
    bind_cpu = RT_SCHED_CTX(thread).bind_cpu;

    /* 未绑定线程进入全局可迁移队列。 */
    if (bind_cpu == RT_CPUS_NR)
    {
#if RT_THREAD_PRIORITY_MAX > 32
        rt_thread_ready_table[RT_SCHED_PRIV(thread).number] |= RT_SCHED_PRIV(thread).high_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
        rt_thread_ready_priority_group |= RT_SCHED_PRIV(thread).number_mask;

        /* YIELD/时间片耗尽者进入同优先级队尾。 */
        if((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
        {
            rt_list_insert_before(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                                  &RT_THREAD_LIST_NODE(thread));
        }
        /* 普通唤醒者进入同优先级队首。 */
        else
        {
            rt_list_insert_after(&(rt_thread_priority_table[RT_SCHED_PRIV(thread).current_priority]),
                                 &RT_THREAD_LIST_NODE(thread));
        }

        /* 其他所有 CPU 都可能需要抢占当前线程；不必给正在执行本路径的 CPU 发 IPI。 */
        cpu_mask = RT_CPU_MASK ^ (1 << cpu_id);
        rt_hw_ipi_send(RT_SCHEDULE_IPI, cpu_mask);
    }
    else
    {
        struct rt_cpu *pcpu = rt_cpu_index(bind_cpu);

#if RT_THREAD_PRIORITY_MAX > 32
        pcpu->ready_table[RT_SCHED_PRIV(thread).number] |= RT_SCHED_PRIV(thread).high_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
        pcpu->priority_group |= RT_SCHED_PRIV(thread).number_mask;

        /* 绑定线程只参与目标 CPU 的本地 FIFO 轮转。 */
        if((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_YIELD_MASK) != 0)
        {
            rt_list_insert_before(&(rt_cpu_index(bind_cpu)->priority_table[RT_SCHED_PRIV(thread).current_priority]),
                                  &RT_THREAD_LIST_NODE(thread));
        }
        /* 未让出时间片者放到目标 CPU 同优先级队首。 */
        else
        {
            rt_list_insert_after(&(rt_cpu_index(bind_cpu)->priority_table[RT_SCHED_PRIV(thread).current_priority]),
                                 &RT_THREAD_LIST_NODE(thread));
        }

        if (cpu_id != bind_cpu)
        {
            cpu_mask = 1 << bind_cpu;
            rt_hw_ipi_send(RT_SCHEDULE_IPI, cpu_mask);
        }
    }

    LOG_D("insert thread[%.*s], the priority: %d",
          RT_NAME_MAX, thread->parent.name, RT_SCHED_PRIV(thread).current_priority);
}

/*
 * 在持有全局调度锁时从就绪队列摘除线程。根据 bind_cpu 找到全局或目标 CPU 位图；
 * 只有对应优先级链表已经为空才清具体位，超过 32 级时只有整个组为空才清分组位。
 */
static void _sched_remove_thread_locked(struct rt_thread *thread)
{
    LOG_D("%s [%.*s], the priority: %d", __func__,
          RT_NAME_MAX, thread->parent.name,
          RT_SCHED_PRIV(thread).current_priority);

    /* 先摘除节点，再依据摘除后的容器状态维护位图。 */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));

    if (RT_SCHED_CTX(thread).bind_cpu == RT_CPUS_NR)
    {
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
    }
    else
    {
        struct rt_cpu *pcpu = rt_cpu_index(RT_SCHED_CTX(thread).bind_cpu);

        if (rt_list_isempty(&(pcpu->priority_table[RT_SCHED_PRIV(thread).current_priority])))
        {
#if RT_THREAD_PRIORITY_MAX > 32
            pcpu->ready_table[RT_SCHED_PRIV(thread).number] &= ~RT_SCHED_PRIV(thread).high_mask;
            if (pcpu->ready_table[RT_SCHED_PRIV(thread).number] == 0)
            {
                pcpu->priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
            }
#else
            pcpu->priority_group &= ~RT_SCHED_PRIV(thread).number_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
        }
    }
}

/**
 * @brief 初始化 SMP 调度器的全局和每 CPU 数据结构。
 *
 * 初始化全局调度锁、全局每优先级链表；随后为每个 CPU 初始化本地链表、IRQ 切换
 * 标志、当前优先级、current_thread 和位图。Smart 配置还初始化每 CPU 私有锁。
 * 最后清空全局位图。完成时所有队列都为空，线程 startup 才会填充它们。
 *
 * @note 必须在任何线程调度发生前调用；该 API 同时有 UP 实现。
 */
/*
 * 以下细节仅属于 SMP，故不纳入公共 API 文档：每 CPU 都有绑定线程队列、就绪位图
 * 与运行状态；所有这些队列和全局可迁移队列共享同一调度自旋锁。
 */
void rt_system_scheduler_init(void)
{
    int cpu;
    rt_base_t offset;

    LOG_D("start scheduler: max priority 0x%02x",
          RT_THREAD_PRIORITY_MAX);

    rt_spin_lock_init(&_mp_scheduler_lock);

    for (offset = 0; offset < RT_THREAD_PRIORITY_MAX; offset ++)
    {
        rt_list_init(&rt_thread_priority_table[offset]);
    }

    for (cpu = 0; cpu < RT_CPUS_NR; cpu++)
    {
        struct rt_cpu *pcpu =  rt_cpu_index(cpu);
        for (offset = 0; offset < RT_THREAD_PRIORITY_MAX; offset ++)
        {
            rt_list_init(&pcpu->priority_table[offset]);
        }

        pcpu->irq_switch_flag = 0;
        pcpu->current_priority = RT_THREAD_PRIORITY_MAX - 1;
        pcpu->current_thread = RT_NULL;
        pcpu->priority_group = 0;

#if RT_THREAD_PRIORITY_MAX > 32
        rt_memset(pcpu->ready_table, 0, sizeof(pcpu->ready_table));
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

#ifdef RT_USING_SMART
        rt_spin_lock_init(&(pcpu->spinlock));
#endif
    }

    /* 全局可迁移队列目前没有任何非空优先级。 */
    rt_thread_ready_priority_group = 0;

#if RT_THREAD_PRIORITY_MAX > 32
    /* 清空两级位图的第二级。 */
    rt_memset(rt_thread_ready_table, 0, sizeof(rt_thread_ready_table));
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
}

/**
 * @brief 在当前 CPU 启动调度器并切换到最高优先级就绪线程。
 *
 * 启动入口先释放历史兼容的 _cpus_lock，再关闭本地中断并直接取得内部调度锁。此时
 * 尚无 current_thread，不能使用依赖线程嵌套字段的完整 SCHEDULER_LOCK。选中线程
 * 后从队列移除、把 oncpu 设为当前 CPU、状态置 RUNNING，释放锁并使用只有“to”
 * 上下文的架构入口完成第一次切换。
 *
 * @note 成功后不返回；调用前调度器必须已初始化且至少空闲线程已经就绪。
 */
/*
 * 这些步骤是 SMP 特有启动细节，不纳入公共 UP/MP API 文档。
 */
void rt_system_scheduler_start(void)
{
    struct rt_thread *to_thread;
    rt_ubase_t highest_ready_priority;

    /*
     * 兼容旧 BSP 的 rt_cpus_lock 启动用法。新调度器不依赖该锁，因此入口处释放
     * 启动代码预先取得的锁，避免首次线程一直继承锁定状态。
     */
    rt_hw_spin_unlock(&_cpus_lock);

    /* 首次建立运行现场期间禁止本地 ISR 观察到半更新状态。 */
    rt_hw_local_irq_disable();

    /*
     * 保护共享调度上下文。此刻还没有 current_thread，故不能调用会操作线程嵌套
     * 字段的 SCHEDULER_LOCK。
     */
    _fast_spin_lock(&_mp_scheduler_lock);

    /* 从全局/本地就绪集合选择实际优先级最高者。 */
    to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);
    RT_ASSERT(to_thread);

    /* RUNNING 线程由 CPU 控制块持有，不能继续留在就绪链表。 */
    _sched_remove_thread_locked(to_thread);

    /* 双向记录“该线程正在当前核运行”。 */
    RT_SCHED_CTX(to_thread).oncpu = rt_hw_cpu_id();
    RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING;

    LOG_D("[cpu#%d] switch to priority#%d thread:%.*s(sp:0x%08x)",
          rt_hw_cpu_id(), RT_SCHED_PRIV(to_thread).current_priority,
          RT_NAME_MAX, to_thread->parent.name, to_thread->sp);

    _fast_spin_unlock(&_mp_scheduler_lock);

    /* 恢复新线程初始栈帧，后续由切换后处理设置 current_thread。 */
    rt_hw_context_switch_to((rt_ubase_t)&to_thread->sp, to_thread);

    /* 正常情况下不再回到启动栈。 */
}

/**
 * @brief 处理其他 CPU 发来的调度 IPI。
 *
 * IPI 表示全局或本 CPU 就绪集合可能出现了更合适的线程。处理函数只调用
 * rt_schedule()；由于当前处于 ISR，它会设置 irq_switch_flag，真正选择与切换在
 * 最外层中断退出路径完成。
 *
 * @param vector 调度 IPI 中断号；本实现不读取。
 *
 * @param param 未使用，可为 RT_NULL。
 *
 * @note 仅 SMP 存在，BSP 应把它注册为 RT_SCHEDULE_IPI 的 ISR。
 */
void rt_scheduler_ipi_handler(int vector, void *param)
{
    rt_schedule();
}

/**
 * @brief 获取 SMP 全局调度锁并保存本地中断状态。
 *
 * 完整加锁同时关闭本地中断、增加当前线程 critical_lock_nest、取得
 * _mp_scheduler_lock。此后调用者可原子修改全局和各 CPU 调度结构。
 *
 * @param plvl 输出加锁前的本地中断状态。
 * @return 成功为 RT_EOK；空输出指针为 -RT_EINVAL。
 *
 * @note 必须与一种 rt_sched_unlock 接口严格配对；公共 API 同时有 UP 实现。
 */
rt_err_t rt_sched_lock(rt_sched_lock_level_t *plvl)
{
    rt_base_t level;
    if (!plvl)
        return -RT_EINVAL;

    SCHEDULER_LOCK(level);
    *plvl = level;

    return RT_EOK;
}

/**
 * @brief 释放全局调度锁、退出一层临界区并恢复本地中断。
 *
 * @param level 配对 rt_sched_lock() 保存的原状态。
 *
 * @return 始终返回 RT_EOK。
 *
 * @note 本入口不主动重新调度；需要调度时使用 rt_sched_unlock_n_resched()。
 */
rt_err_t rt_sched_unlock(rt_sched_lock_level_t level)
{
    SCHEDULER_UNLOCK(level);

    return RT_EOK;
}

/**
 * @brief 判断当前 CPU 是否正持有调度上下文锁。
 *
 * 短暂关闭本地中断后读取 per-CPU sched_lock_flag，避免本 CPU 调度路径在读取时
 * 改变它。它只回答当前 CPU 的内部上下文锁状态，不代表其他 CPU 是否正在持锁。
 *
 * @return 已锁定返回 RT_TRUE，否则返回 RT_FALSE。
 *
 * @note 仅 SMP 提供。
 */
rt_bool_t rt_sched_is_locked(void)
{
    rt_bool_t rc;
    rt_base_t level;
    struct rt_cpu *pcpu;

    level = rt_hw_local_irq_disable();
    pcpu = rt_cpu_self();

    /* 该字段只取 0/1，且与 CONTEXT_LOCK/UNLOCK 同步更新。 */
    rc = pcpu->sched_lock_flag;

    rt_hw_local_irq_enable(level);
    return rc;
}

/**
 * @brief 在持锁状态下决定当前 CPU 下一位运行线程并准备切换。
 *
 * 先比较全局/本地最高就绪线程，并暂时把当前线程 oncpu 置 DETACHED。若当前线程仍
 * RUNNING、允许在本 CPU 运行且优先级更高（或相同且未 YIELD），它继续占用 CPU；
 * 否则将其插回合适队列。选中的新线程写入 oncpu，若确实发生切换，则触发钩子、
 * 摘除目标、置 RUNNING 并检查目标栈。
 *
 * @param cpu_id 当前逻辑 CPU 编号。
 * @param pcpu 当前 CPU 控制块。
 * @param current_thread 调度前的运行线程。
 * @return 需要切换时返回目标线程；当前线程继续运行或没有候选者时返回 RT_NULL。
 * @note 调用者必须持有调度上下文锁。本函数为切换准备状态，但锁通常由切换后处理
 *       或无切换分支释放。
 */
static rt_thread_t _prepare_context_switch_locked(int cpu_id,
                                                  struct rt_cpu *pcpu,
                                                  rt_thread_t current_thread)
{
    rt_thread_t to_thread = RT_NULL;
    rt_ubase_t highest_ready_priority;

    /* 位图均为 0 时没有候选者，可跳过链表访问。 */
    if (rt_thread_ready_priority_group != 0 || pcpu->priority_group != 0)
    {
        /* 同时考虑可迁移全局队列和绑定到本 CPU 的本地队列。 */
        to_thread = _scheduler_get_highest_priority_thread(&highest_ready_priority);

        /* 先解除运行归属，随后无论继续运行还是入队都会重新建立一致状态。 */
        RT_SCHED_CTX(current_thread).oncpu = RT_CPU_DETACHED;

        /* 只有基本状态仍为 RUNNING 才有资格被重新排队/继续运行。 */
        if ((RT_SCHED_CTX(current_thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_RUNNING)
        {
            /* 未绑定或正好绑定本 CPU 才能继续留在本核。 */
            if (RT_SCHED_CTX(current_thread).bind_cpu == RT_CPUS_NR
                || RT_SCHED_CTX(current_thread).bind_cpu == cpu_id)
            {
                /* 当前有效优先级更高，无需切换。 */
                if (RT_SCHED_PRIV(current_thread).current_priority < highest_ready_priority)
                {
                    to_thread = current_thread;
                }
                /* 同级且未主动让出时，避免无意义切换。 */
                else if (RT_SCHED_PRIV(current_thread).current_priority == highest_ready_priority &&
                         (RT_SCHED_CTX(current_thread).stat & RT_THREAD_STAT_YIELD_MASK) == 0)
                {
                    to_thread = current_thread;
                }
                /* 候选更优或当前已 YIELD，把当前线程放回队列。 */
                else
                {
                    _sched_insert_thread_locked(current_thread);
                }
            }
            else
            {
                /* 绑定已改变，当前线程必须进入目标 CPU 本地队列。 */
                _sched_insert_thread_locked(current_thread);
            }

            /* YIELD 只影响本次队列次序，决策后消费。 */
            RT_SCHED_CTX(current_thread).stat &= ~RT_THREAD_STAT_YIELD_MASK;
        }

        /**
         * 目标已经确定，先写它的 oncpu。pcpu->current_thread 要到架构切换后处理才
         * 更新；调度锁在整个空窗期持续持有，因此正确获取同一锁的观察者不会看到
         * 这组字段的中间不一致状态。
         */
        RT_SCHED_CTX(to_thread).oncpu = cpu_id;

        /* 目标不同才需要迁移队列和保存/恢复寄存器。 */
        if (to_thread != current_thread)
        {
            pcpu->current_priority = (rt_uint8_t)highest_ready_priority;

            RT_OBJECT_HOOK_CALL(rt_scheduler_hook, (current_thread, to_thread));

            /* RUNNING 线程不留在就绪队列，保留其附加状态位。 */
            _sched_remove_thread_locked(to_thread);
            RT_SCHED_CTX(to_thread).stat = RT_THREAD_RUNNING | (RT_SCHED_CTX(to_thread).stat & ~RT_THREAD_STAT_MASK);

            RT_SCHEDULER_STACK_CHECK(to_thread);

            RT_OBJECT_HOOK_CALL(rt_scheduler_switch_hook, (current_thread));
        }
        else
        {
            /* 返回 RT_NULL 告诉调用者只需解锁，不调用架构切换。 */
            to_thread = RT_NULL;
        }
    }
    else
    {
        /* 正常系统通常至少有空闲线程；此分支仍防御空位图。 */
        to_thread = RT_NULL;
    }

    return to_thread;
}

#ifdef RT_USING_SIGNALS
/**
 * @brief 在正式调度前让有待处理信号的挂起线程恢复就绪。
 *
 * 信号只能在线程上下文执行处理器；若目标仍因 IPC/等待而挂起，pending 标志本身
 * 不足以让它获得 CPU，所以先用 Smart wakeup 或普通 resume 打断等待。调用点持有
 * 调度上下文锁，防止状态在检查和恢复之间变化。
 *
 * @param current_thread 要检查的线程。
 */
static void _sched_thread_preprocess_signal(struct rt_thread *current_thread)
{
    /* 运行/就绪线程本来就有机会处理，无需额外恢复。 */
    if (rt_sched_thread_is_suspended(current_thread))
    {
        /* 仅 pending 信号需要打断挂起。 */
        if ((RT_SCHED_CTX(current_thread).stat & RT_THREAD_STAT_SIGNAL_MASK) & RT_THREAD_STAT_SIGNAL_PENDING)
        {
#ifdef RT_USING_SMART
            rt_thread_wakeup(current_thread);
#else
            rt_thread_resume(current_thread);
#endif
        }
    }
}

/**
 * @brief 在当前线程上下文消费 pending 标志并执行信号处理器。
 *
 * 在调度锁内原子检查并清除 SIGNAL_PENDING，随后必须先解锁再调用
 * rt_thread_handle_sig()，因为用户信号处理可能复杂且不能持有全局调度自旋锁。
 * 没有 pending 时只解锁返回。
 *
 * @param current_thread 当前正在本 CPU 运行的线程。
 */
static void _sched_thread_process_signal(struct rt_thread *current_thread)
{
    rt_base_t level;
    SCHEDULER_LOCK(level);

    /* 标志检查与清除作为同一个受锁操作，避免丢失并发投递。 */
    if (RT_SCHED_CTX(current_thread).stat & RT_THREAD_STAT_SIGNAL_PENDING)
    {
        extern void rt_thread_handle_sig(rt_bool_t clean_state);

        RT_SCHED_CTX(current_thread).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;

        SCHEDULER_UNLOCK(level);

        /* 已在普通线程上下文且不持调度锁，允许进入完整信号处理流程。 */
        rt_thread_handle_sig(RT_TRUE);
    }
    else
    {
        SCHEDULER_UNLOCK(level);
    }

    /* 两个分支都已经释放锁。 */
}

#define SCHED_THREAD_PREPROCESS_SIGNAL(pcpu, curthr)    \
    do                                            \
    {                                             \
        SCHEDULER_CONTEXT_LOCK(pcpu);   \
        _sched_thread_preprocess_signal(curthr);  \
        SCHEDULER_CONTEXT_UNLOCK(pcpu); \
    } while (0)
#define SCHED_THREAD_PREPROCESS_SIGNAL_LOCKED(curthr) \
    _sched_thread_preprocess_signal(curthr)
#define SCHED_THREAD_PROCESS_SIGNAL(curthr) _sched_thread_process_signal(curthr)

#else /* ! RT_USING_SIGNALS */

#define SCHED_THREAD_PREPROCESS_SIGNAL(pcpu, curthr)
#define SCHED_THREAD_PREPROCESS_SIGNAL_LOCKED(curthr)
#define SCHED_THREAD_PROCESS_SIGNAL(curthr)
#endif /* RT_USING_SIGNALS */

/**
 * @brief 释放一个已持有的调度锁，并尽可能立即完成重新调度。
 *
 * 此入口假定调用者刚用 rt_sched_lock() 获取了上下文锁。若启动早期无当前线程，
 * 只能解锁并返回 EBUSY；若处于 ISR，只置 irq_switch_flag 并返回 ESCHEDISR；若还有
 * 外层 critical_lock_nest，则设置延迟切换并返回 ESCHEDLOCKED。只有最外层线程
 * 上下文才真正选择目标并调用架构切换。无论是否切换，最终恢复 @p level，并在
 * 锁外处理当前线程的 pending 信号。
 *
 * @param level 配对 rt_sched_lock() 保存的本地中断状态。
 * @return RT_EOK 表示已完成决策；-RT_EBUSY 表示调度器未启动；-RT_ESCHEDISR 表示
 *         已延迟到中断退出；-RT_ESCHEDLOCKED 表示仍有外层禁止调度临界区。
 *
 * @note 必须与 rt_sched_lock() 配对，且可能在返回前切换线程；API 同时有 UP 实现。
 */
rt_err_t rt_sched_unlock_n_resched(rt_sched_lock_level_t level)
{
    struct rt_thread *to_thread;
    struct rt_thread *current_thread;
    struct rt_cpu    *pcpu;
    int              cpu_id;
    rt_err_t         error = RT_EOK;

    cpu_id = rt_hw_cpu_id();
    pcpu   = rt_cpu_index(cpu_id);
    current_thread = pcpu->current_thread;

    if (!current_thread)
    {
        /* 启动阶段尚无运行线程，释放能释放的锁状态后报告不可调度。 */
        SCHEDULER_CONTEXT_UNLOCK(pcpu);
        SCHEDULER_EXIT_CRITICAL(current_thread);
        rt_hw_local_irq_enable(level);
        return -RT_EBUSY;
    }

    /* ISR 不能在这里直接改用线程栈，登记给中断退出路径。 */
    if (rt_atomic_load(&(pcpu->irq_nest)))
    {
        pcpu->irq_switch_flag = 1;
        SCHEDULER_CONTEXT_UNLOCK(pcpu);
        SCHEDULER_EXIT_CRITICAL(current_thread);
        rt_hw_local_irq_enable(level);
        return -RT_ESCHEDISR;
    }

    /* pending 信号可能需要先解除该线程的挂起状态。 */
    SCHED_THREAD_PREPROCESS_SIGNAL_LOCKED(current_thread);

    /* 当前这一层之外仍有临界区时，禁止实际线程切换。 */
    if (RT_SCHED_CTX(current_thread).critical_lock_nest > 1)
    {
        /* 释放全局锁，使其他 CPU 可继续调度；只保留线程级外层临界状态。 */
        SCHEDULER_CONTEXT_UNLOCK(pcpu);

        SET_CRITICAL_SWITCH_FLAG(pcpu, current_thread);
        error = -RT_ESCHEDLOCKED;

        SCHEDULER_EXIT_CRITICAL(current_thread);
    }
    else
    {
        /* 本次将完成完整决策，消费旧的延迟请求。 */
        CLR_CRITICAL_SWITCH_FLAG(pcpu, current_thread);

        /* 函数内部准备队列、状态、钩子和目标 oncpu。 */
        to_thread = _prepare_context_switch_locked(cpu_id, pcpu, current_thread);
        if (to_thread)
        {
            /* 锁由切换后的 rt_sched_post_ctx_switch() 负责释放。 */
            LOG_D("[cpu#%d] UNLOCK switch to priority#%d "
                  "thread:%.*s(sp:0x%08x), "
                  "from thread:%.*s(sp: 0x%08x)",
                  cpu_id, RT_SCHED_PRIV(to_thread).current_priority,
                  RT_NAME_MAX, to_thread->parent.name, to_thread->sp,
                  RT_NAME_MAX, current_thread->parent.name, current_thread->sp);

            rt_hw_context_switch((rt_ubase_t)&current_thread->sp,
                                 (rt_ubase_t)&to_thread->sp, to_thread);
        }
        else
        {
            SCHEDULER_CONTEXT_UNLOCK(pcpu);
            SCHEDULER_EXIT_CRITICAL(current_thread);
        }
    }

    /* 恢复调用 rt_sched_lock() 之前的本地中断状态。 */
    rt_hw_local_irq_enable(level);

    /* 仅在不持调度锁的普通线程上下文运行处理器。 */
    SCHED_THREAD_PROCESS_SIGNAL(current_thread);

    return error;
}

/**
 * @brief 执行一次 SMP 调度：比较全局和本地队列并在需要时切换线程。
 *
 * 普通线程上下文中，先关本地中断防止运行现场被 ISR 改变，再增加临界层数避免递归。
 * 如果实际在 ISR 中调用，只设置 irq_switch_flag 返回。若已有外层临界区，同样只
 * 设置 pending；否则取得全局上下文锁、准备目标并调用 rt_hw_context_switch()。
 * 无需切换时本路径自行解锁；需要切换时由新上下文的 post-switch 路径解锁。
 *
 * @note 公共 API 同时有 UP 实现；调用可能使当前线程直到很久以后才从函数中返回。
 */

/*
 * 以下控制流是 SMP 专用细节，故不放入公共 Doxygen：本地 IRQ 保护 CPU 现场，
 * 全局自旋锁保护队列，二者分别解决本地中断与跨 CPU 两类并发。
 */
void rt_schedule(void)
{
    rt_base_t level;
    struct rt_thread *to_thread;
    struct rt_thread *current_thread;
    struct rt_cpu    *pcpu;
    int              cpu_id;

    /* 先冻结本 CPU 的中断现场。 */
    level = rt_hw_local_irq_disable();

    /* 在禁止迁移/中断的窗口取得一致的 CPU 与当前线程。 */
    cpu_id = rt_hw_cpu_id();
    pcpu   = rt_cpu_index(cpu_id);
    current_thread = pcpu->current_thread;

    /* ISR 中只登记，不能立刻把 ISR 所在栈切成普通线程栈。 */
    if (rt_atomic_load(&(pcpu->irq_nest)))
    {
        pcpu->irq_switch_flag = 1;
        rt_hw_local_irq_enable(level);
        return ; /* 语义等同“调度已延后到 ISR 退出”。 */
    }

    /* 本函数自己占一层，借此检测调用者是否已有外层临界区。 */
    SCHEDULER_ENTER_CRITICAL(current_thread);

    /* 信号可能使挂起线程恢复就绪，必须先反映到候选集合。 */
    SCHED_THREAD_PREPROCESS_SIGNAL(pcpu, current_thread);

    /* 大于 1 表明除本函数这一层外还有禁止调度者。 */
    if (RT_SCHED_CTX(current_thread).critical_lock_nest > 1)
    {
        SET_CRITICAL_SWITCH_FLAG(pcpu, current_thread);

        SCHEDULER_EXIT_CRITICAL(current_thread);

        /* 记录请求即可；外层退出时会再次调用调度器。 */
    }
    else
    {
        /* 现在允许完整决策，清除已消费的延迟标志。 */
        CLR_CRITICAL_SWITCH_FLAG(pcpu, current_thread);
        pcpu->irq_switch_flag = 0;

        /*
         * 真正修改共享就绪队列前获取全局锁。若发生切换，锁由切换后入口释放；
         * 否则本函数下方显式释放。
         */
        SCHEDULER_CONTEXT_LOCK(pcpu);

        /* 选择最高优先级可运行线程，并准备把当前 CPU 交给它。 */
        to_thread = _prepare_context_switch_locked(cpu_id, pcpu, current_thread);

        if (to_thread)
        {
            LOG_D("[cpu#%d] switch to priority#%d "
                  "thread:%.*s(sp:0x%08x), "
                  "from thread:%.*s(sp: 0x%08x)",
                  cpu_id, RT_SCHED_PRIV(to_thread).current_priority,
                  RT_NAME_MAX, to_thread->parent.name, to_thread->sp,
                  RT_NAME_MAX, current_thread->parent.name, current_thread->sp);

            rt_hw_context_switch((rt_ubase_t)&current_thread->sp,
                                 (rt_ubase_t)&to_thread->sp, to_thread);
        }
        else
        {
            /* 没有更合适目标，恢复调度锁和本函数增加的临界层数。 */
            SCHEDULER_CONTEXT_UNLOCK(pcpu);
            SCHEDULER_EXIT_CRITICAL(current_thread);
        }
    }

    /* 恢复进入函数前的本地中断状态。 */
    rt_hw_local_irq_enable(level);

    /* 此时已无全局自旋锁，可安全进入完整信号处理。 */
    SCHED_THREAD_PROCESS_SIGNAL(current_thread);
}

/**
 * @brief 在最外层中断退出边界兑现延迟的调度请求。
 *
 * 先查看 irq_switch_flag；没有请求则继续当前线程。仍有外层禁止调度临界区时把请求
 * 转为 critical_switch_flag。只有 irq_nest 已降为 0 且无外层锁，才取得全局调度锁、
 * 选择目标，并调用能改写异常返回现场的 rt_hw_context_switch_interrupt()。
 *
 * @param context 架构保存的中断返回上下文，供切换函数修改返回目标。
 *
 * @note 仅 SMP 中断退出代码调用；进入时本地中断应仍受控。
 */
void rt_scheduler_do_irq_switch(void *context)
{
    int              cpu_id;
    rt_base_t        level;
    struct rt_cpu    *pcpu;
    struct rt_thread *to_thread;
    struct rt_thread *current_thread;

    level = rt_hw_local_irq_disable();

    cpu_id = rt_hw_cpu_id();
    pcpu   = rt_cpu_index(cpu_id);
    current_thread = pcpu->current_thread;

    /* 临时增加一层，用同一套嵌套规则防止递归调度。 */
    SCHEDULER_ENTER_CRITICAL(current_thread);

    SCHED_THREAD_PREPROCESS_SIGNAL(pcpu, current_thread);

    /* 没有 IPI/ISR 唤醒等产生的 pending 请求就走快速返回。 */
    if (pcpu->irq_switch_flag == 0)
    {
        /* 保持原异常返回目标不变。 */
        SCHEDULER_EXIT_CRITICAL(current_thread);
        rt_hw_local_irq_enable(level);
        return;
    }

    /* 外层临界区仍存在时不能改变运行线程。 */
    if (RT_SCHED_CTX(current_thread).critical_lock_nest > 1)
    {
        SET_CRITICAL_SWITCH_FLAG(pcpu, current_thread);
        SCHEDULER_EXIT_CRITICAL(current_thread);
    }
    else if (rt_atomic_load(&(pcpu->irq_nest)) == 0)
    {
        /* 本次将消费中断和临界区两类 pending 请求。 */
        CLR_CRITICAL_SWITCH_FLAG(pcpu, current_thread);
        pcpu->irq_switch_flag = 0;

        SCHEDULER_CONTEXT_LOCK(pcpu);

        /* 在全局锁内完成队列选择与状态迁移。 */
        to_thread = _prepare_context_switch_locked(cpu_id, pcpu, current_thread);
        if (to_thread)
        {
            LOG_D("[cpu#%d] IRQ switch to priority#%d "
                  "thread:%.*s(sp:0x%08x), "
                  "from thread:%.*s(sp: 0x%08x)",
                  cpu_id, RT_SCHED_PRIV(to_thread).current_priority,
                  RT_NAME_MAX, to_thread->parent.name, to_thread->sp,
                  RT_NAME_MAX, current_thread->parent.name, current_thread->sp);

            rt_hw_context_switch_interrupt(context, (rt_ubase_t)&current_thread->sp,
                                           (rt_ubase_t)&to_thread->sp, to_thread);
        }
        else
        {
            /* 候选不优于当前线程，恢复锁状态并按原现场返回。 */
            SCHEDULER_CONTEXT_UNLOCK(pcpu);
            SCHEDULER_EXIT_CRITICAL(current_thread);
        }
    }
    else
    {
        SCHEDULER_EXIT_CRITICAL(current_thread);
    }

    /* 恢复进入本函数前的本地中断状态。 */
    rt_hw_local_irq_enable(level);
}

/**
 * @brief 在已持锁条件下将线程加入 SMP 就绪集合。
 *
 * 具体插入全局还是绑定 CPU 的本地队列由 _sched_insert_thread_locked() 决定。
 *
 * @param thread 待就绪线程，不得为 RT_NULL。
 * @note 内核内部接口；调用者必须已经持有调度锁，应用不得直接调用。
 *
 * @note 公共接口同时有 UP 实现。
 */
void rt_sched_insert_thread(struct rt_thread *thread)
{
    RT_ASSERT(thread != RT_NULL);
    RT_SCHED_DEBUG_IS_LOCKED;

    /* 内部函数同时维护状态、链表、位图和必要的跨核 IPI。 */
    _sched_insert_thread_locked(thread);
}

/**
 * @brief 从 SMP 就绪集合移除线程，并暂置为不可中断挂起态。
 *
 * @param thread 当前位于相应就绪队列中的线程。
 *
 * @note 内核内部接口；调用者必须持有调度锁且保证节点确实已入队。
 *
 * @note 公共接口同时有 UP 实现。
 */
void rt_sched_remove_thread(struct rt_thread *thread)
{
    RT_ASSERT(thread != RT_NULL);
    RT_SCHED_DEBUG_IS_LOCKED;

    /* 摘链并维护全局/本地位图。 */
    _sched_remove_thread_locked(thread);

    RT_SCHED_CTX(thread).stat = RT_THREAD_SUSPEND_UNINTERRUPTIBLE;
}

/**
 * @brief 初始化线程私有的 SMP 调度字段。
 *
 * 初始化调度链表节点、基础/当前优先级、尚未使用的位图缓存、初始/剩余时间片，
 * 并把每线程 critical_lock_nest 清零。创建阶段尚未入队，所以位图掩码暂为 0。
 *
 * @param thread 待初始化线程。
 * @param tick 初始时间片长度。
 * @param priority 初始优先级，数值越小越高。
 *
 * @note 公共接口同时有 UP 实现。
 */
void rt_sched_thread_init_priv(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority)
{
    rt_list_init(&RT_THREAD_LIST_NODE(thread));

    /* 尚无优先级继承，基础值与有效值相同。 */
    RT_ASSERT(priority < RT_THREAD_PRIORITY_MAX);
    RT_SCHED_PRIV(thread).init_priority    = priority;
    RT_SCHED_PRIV(thread).current_priority = priority;

    /* INIT 状态不在就绪集合，startup 时再计算真正掩码。 */
    RT_SCHED_PRIV(thread).number_mask = 0;
#if RT_THREAD_PRIORITY_MAX > 32
    RT_SCHED_PRIV(thread).number = 0;
    RT_SCHED_PRIV(thread).high_mask = 0;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */

    /* 首次运行拥有完整时间片。 */
    RT_SCHED_PRIV(thread).init_tick = tick;
    RT_SCHED_PRIV(thread).remaining_tick = tick;

#ifdef RT_USING_SMP

    /* 每线程的禁止调度嵌套从 0 开始。 */
    RT_SCHED_CTX(thread).critical_lock_nest = 0;
#endif /* RT_USING_SMP */

}

/**
 * @brief 为首次 startup 计算优先级位图属性，并转为可恢复的挂起态。
 *
 * 超过 32 级时用 number 选择 8 级组、number_mask 标记组、high_mask 标记组内
 * 优先级；否则 number_mask 直接对应优先级。最后置 SUSPEND，让统一 resume 路径
 * 完成首次插入就绪队列。
 *
 * @param thread 创建完成、尚未与其他路径并发的线程。
 *
 * @note 创建阶段无竞争，因此本操作无锁；公共接口同时有 UP 实现。
 *
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

    /* 复用挂起 -> 就绪的标准启动路径。 */
    RT_SCHED_CTX(thread).stat = RT_THREAD_SUSPEND;
}

/**
 * @brief 在架构上下文切换完成后提交当前 CPU 的运行线程并释放旧调度锁。
 *
 * 切换前路径故意保持 _mp_scheduler_lock，并让旧线程 critical_lock_nest 为 1，防止
 * 其他 CPU 观察到 SP 与 current_thread 的半更新状态。新上下文进入此函数后验证
 * 本地中断仍关闭，清旧线程层数、释放全局锁，最后把 pcpu->current_thread 改为
 * 新线程。于是“保存/恢复 SP + 更新当前线程”对正确加锁的观察者表现为原子操作。
 *
 * @param thread 上下文切换后实际在当前 CPU 运行的目标线程。
 *
 * @note 仅 SMP 架构切换汇编/包装代码调用，且必须保持本地中断关闭。
 */
void rt_sched_post_ctx_switch(struct rt_thread *thread)
{
    struct rt_cpu* pcpu = rt_cpu_self();
    rt_thread_t from_thread = pcpu->current_thread;

    RT_ASSERT(rt_hw_interrupt_is_disabled());

    if (from_thread)
    {
        RT_ASSERT(RT_SCHED_CTX(from_thread).critical_lock_nest == 1);

        /* 旧线程的切换临界区到此结束，释放跨 CPU 可见的上下文锁。 */
        RT_SCHED_CTX(from_thread).critical_lock_nest = 0;
        SCHEDULER_CONTEXT_UNLOCK(pcpu);
    }
    /* 本地中断关闭保证本 CPU 不会在写指针时再次进入调度。 */
    pcpu->current_thread = thread;
}

#ifdef RT_DEBUGING_CRITICAL

static volatile int _critical_error_occurred = 0;

/**
 * @brief 调试构建中核对当前线程临界区嵌套层数后退出一层。
 *
 * 正确 LIFO 配对应使传入层数等于线程当前 critical_lock_nest。不匹配说明漏退出、
 * 多退出或跨层使用 level；首次发现时输出现场和回溯并停机。随后正常路径调用
 * rt_exit_critical() 完成实际退出。
 *
 * @param critical_level 配对 rt_enter_critical() 返回的预期层数。
 * @note 公共接口同时有 UP 实现。
 */
void rt_exit_critical_safe(rt_base_t critical_level)
{
    struct rt_cpu *pcpu = rt_cpu_self();
    rt_thread_t current_thread = pcpu->current_thread;
    if (current_thread && !_critical_error_occurred)
    {
        if (critical_level != RT_SCHED_CTX(current_thread).critical_lock_nest)
        {
            int dummy = 1;
            _critical_error_occurred = 1;

            rt_kprintf("%s: un-compatible critical level\n" \
                       "\tCurrent %d\n\tCaller %d\n",
                       __func__, RT_SCHED_CTX(current_thread).critical_lock_nest,
                       critical_level);
            rt_backtrace();

            while (dummy) ;
        }
    }
    rt_exit_critical();
}

#else /* !RT_DEBUGING_CRITICAL */

/**
 * @brief 非调试构建的安全退出包装。
 *
 * 非调试构建不核对 @p critical_level，只显式标记未使用并退出当前一层。
 *
 * @param critical_level 未使用的预期层数。
 * @note 公共接口同时有 UP 实现。
 */
void rt_exit_critical_safe(rt_base_t critical_level)
{
    RT_UNUSED(critical_level);
    return rt_exit_critical();
}

#endif /* RT_DEBUGING_CRITICAL */
RTM_EXPORT(rt_exit_critical_safe);

#ifdef ARCH_USING_HW_THREAD_SELF
#define FREE_THREAD_SELF(lvl)

#else /* !ARCH_USING_HW_THREAD_SELF */
#define FREE_THREAD_SELF(lvl)        \
    do                               \
    {                                \
        rt_hw_local_irq_enable(lvl); \
    } while (0)

#endif /* ARCH_USING_HW_THREAD_SELF */

/**
 * @brief 为当前线程进入一层可嵌套的禁止调度临界区。
 *
 * 有硬件 current-thread 寄存器的架构可直接取线程；否则短暂关闭本地中断后从 CPU
 * 控制块读取，避免读取期间迁移/切换。这里只增加线程嵌套计数，不在整个临界区关闭
 * 中断，也不长期持有全局调度自旋锁；ISR 和其他 CPU 仍运行，但涉及该线程的切换
 * 会被延后。
 *
 * @return 进入后的嵌套层数；调度器尚无 current_thread 时返回 -RT_EINVAL。
 * @note 可嵌套，但每次必须与 rt_exit_critical() 配对；公共 API 同时有 UP 实现。
 */
rt_base_t rt_enter_critical(void)
{
    rt_base_t critical_level;
    struct rt_thread *current_thread;

#ifndef ARCH_USING_HW_THREAD_SELF
    rt_base_t level;
    struct rt_cpu *pcpu;

    /* 软件 current_thread 读取需要一个不可被本地调度打断的窗口。 */
    level = rt_hw_local_irq_disable();

    pcpu = rt_cpu_self();
    current_thread = pcpu->current_thread;

#else /* !ARCH_USING_HW_THREAD_SELF */
    current_thread = rt_hw_thread_self();

#endif /* ARCH_USING_HW_THREAD_SELF */

    if (!current_thread)
    {
        FREE_THREAD_SELF(level);
        /* 首次调度前没有线程可承载嵌套字段。 */
        return -RT_EINVAL;
    }

    /* 禁止的是当前线程被调度切出，而不是所有 CPU 或硬件中断。 */
    RT_SCHED_CTX(current_thread).critical_lock_nest++;
    critical_level = RT_SCHED_CTX(current_thread).critical_lock_nest;

    FREE_THREAD_SELF(level);

    return critical_level;
}
RTM_EXPORT(rt_enter_critical);

/**
 * @brief 退出当前线程的一层禁止调度临界区。
 *
 * 原子地减少线程层数。降到 0 时读取并清除延迟切换标志，先恢复取得线程时关闭的
 * 本地中断，再按需调用 rt_schedule()；仍大于 0 时断言层数合法并继续延后调度。
 *
 * @note 必须配对，最外层退出可能立即切换线程；公共 API 同时有 UP 实现。
 */
void rt_exit_critical(void)
{
    struct rt_thread *current_thread;
    rt_bool_t need_resched;

#ifndef ARCH_USING_HW_THREAD_SELF
    rt_base_t level;
    struct rt_cpu *pcpu;

    /* 软件路径关闭本地中断以稳定 current_thread 与嵌套字段。 */
    level = rt_hw_local_irq_disable();

    pcpu = rt_cpu_self();
    current_thread = pcpu->current_thread;

#else /* !ARCH_USING_HW_THREAD_SELF */
    current_thread = rt_hw_thread_self();

#endif /* ARCH_USING_HW_THREAD_SELF */

    if (!current_thread)
    {
        FREE_THREAD_SELF(level);
        return;
    }

    /* irq disable/enable 已提供此处需要的内存顺序屏障。 */
    RT_SCHED_CTX(current_thread).critical_lock_nest--;

    /* 只有最外层退出才有权消费并执行延迟调度。 */
    if (RT_SCHED_CTX(current_thread).critical_lock_nest == 0)
    {
        /* 先保存请求再清标志，避免同一请求重复执行。 */
        need_resched = IS_CRITICAL_SWITCH_PEND(pcpu, current_thread);
        CLR_CRITICAL_SWITCH_FLAG(pcpu, current_thread);

        FREE_THREAD_SELF(level);

        if (need_resched)
            rt_schedule();
    }
    else
    {
        /* 每次退出必须严格对应一次进入，层数不能降到负值。 */
        RT_ASSERT(RT_SCHED_CTX(current_thread).critical_lock_nest > 0);

        FREE_THREAD_SELF(level);
    }
}
RTM_EXPORT(rt_exit_critical);

/**
 * @brief 获取当前线程的禁止调度嵌套层数。
 *
 * 读取时短暂关闭本地中断，确保 current_thread 不在途中变化；启动早期无当前线程
 * 时按 0 处理。
 *
 * @return 当前层数，0 表示未锁调度。
 * @note 公共接口同时有 UP 实现。
 */
rt_uint16_t rt_critical_level(void)
{
    rt_base_t level;
    rt_uint16_t critical_lvl;
    struct rt_thread *current_thread;

    level = rt_hw_local_irq_disable();

    current_thread = rt_cpu_self()->current_thread;

    if (current_thread)
    {
        /* 中断屏蔽同时提供读取该并发字段需要的屏障。 */
        critical_lvl = RT_SCHED_CTX(current_thread).critical_lock_nest;
    }
    else
    {
        critical_lvl = 0;
    }

    rt_hw_local_irq_enable(level);
    return critical_lvl;
}
RTM_EXPORT(rt_critical_level);

/**
 * @brief 将线程绑定到指定 CPU，或恢复为可迁移状态。
 *
 * READY 线程必须先从旧队列摘除，修改 bind_cpu 后插入新队列并重新调度；RUNNING
 * 线程只更新绑定字段，然后视运行核和目标核发送 IPI，使不再符合亲和性的线程尽快
 * 迁移。挂起等其他状态尚未入队，只需记录绑定，未来唤醒时自然进入正确队列。
 *
 * @param thread 要改变亲和性的线程。
 * @param cpu 目标 CPU 下标；任何大于等于 RT_CPUS_NR 的值都归一化为 RT_CPUS_NR，
 *            表示解除绑定、允许迁移。
 * @return 操作完成返回 RT_EOK。
 *
 * @note 调用前调度器必须未锁，本函数内部会获取全局调度锁。
 *
 * @note UP 兼容实现不执行绑定并返回 -RT_EINVAL。
 */
rt_err_t rt_sched_thread_bind_cpu(struct rt_thread *thread, int cpu)
{
    rt_sched_lock_level_t slvl;
    rt_uint8_t thread_stat;

    RT_SCHED_DEBUG_IS_UNLOCKED;

    if (cpu >= RT_CPUS_NR)
    {
        cpu = RT_CPUS_NR;
    }

    rt_sched_lock(&slvl);

    thread_stat = rt_sched_thread_get_stat(thread);

    if (thread_stat == RT_THREAD_READY)
    {
        /* READY 节点的位置由绑定属性决定，先按旧属性摘除。 */
        rt_sched_remove_thread(thread);
        /* 修改路由属性。 */
        RT_SCHED_CTX(thread).bind_cpu = cpu;
        /* 按新属性进入全局或目标 CPU 本地队列。 */
        rt_sched_insert_thread(thread);

        if (rt_thread_self() != RT_NULL)
        {
            rt_sched_unlock_n_resched(slvl);
        }
        else
        {
            rt_sched_unlock(slvl);
        }
    }
    else
    {
        RT_SCHED_CTX(thread).bind_cpu = cpu;
        if (thread_stat == RT_THREAD_RUNNING)
        {
            /* RUNNING 线程可能需要其当前 CPU 主动让出。 */
            int current_cpu = rt_hw_cpu_id();

            if (cpu != RT_CPUS_NR)
            {
                if (RT_SCHED_CTX(thread).oncpu == current_cpu)
                {
                    /* 被操作线程恰在调用者 CPU 运行。 */
                    if (cpu != current_cpu)
                    {
                        /* 通知目标核有绑定线程，并让本核重新选择。 */
                        rt_hw_ipi_send(RT_SCHEDULE_IPI, 1U << cpu);
                        /* 解锁并在本核立刻重新调度，停止继续违反新亲和性。 */
                        rt_sched_unlock_n_resched(slvl);
                    }
                    else
                    {
                        /* 仍绑定当前核，无需迁移。 */
                        rt_sched_unlock(slvl);
                    }
                }
                else
                {
                    /* 线程在另一核运行，通知其运行核重新检查新绑定。 */
                    rt_hw_ipi_send(RT_SCHEDULE_IPI, 1U << RT_SCHED_CTX(thread).oncpu);
                    rt_sched_unlock(slvl);
                }
            }
            else
            {
                /* 解除绑定不会强制立即迁移，当前核可继续运行它。 */
                rt_sched_unlock(slvl);
            }
        }
        else
        {
            rt_sched_unlock(slvl);
        }
    }

    return RT_EOK;
}

/**
 * @} group_thread_management
 *
 * @endcond
 */
