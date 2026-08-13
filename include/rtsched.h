/*
 * Copyright (c) 2023-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2024-01-19     Shell        与 rt_thread_t 分开的调度语句
 *                             to rt_sched_thread_ctx. Add definitions of scheduler.
 */
#ifndef __RT_SCHED_H__
#define __RT_SCHED_H__

/**
 * @file rtsched.h
 * @brief 线程控制块中由调度器拥有的部分及其内部 API。
 *
 * RT_SCHED_THREAD_CTX 嵌入在 `struct rt_thread` 中。将调度状态放入专用子对象，可明确
 * 所有权和加锁要求，同时让 UP 与 SMP 调度器共用线程代码。
 *
 * 应用代码应使用公开的 rt_thread_* 和 IPC API，而不应直接修改这些状态。多数成员参与就绪
 * 队列归属、优先级位图、超时竞争或跨 CPU 决策；未持有调度器锁就读取或修改它们，可能破坏
 * 队列或得到过期状态。
 *
 * 优先级遵循 RT-Thread 约定：数值越小，调度优先级越高。同优先级线程由就绪链表及时间片/
 * 主动让出策略排序。
 */

#include "rttypes.h"
#include "rtcompiler.h"

#ifdef __cplusplus
extern "C" {
#endif

struct rt_thread;

/**
 * RT_THREAD_* 状态和辅助状态标志位的存储类型。使用 RT_THREAD_STAT_MASK 可取得基础
 * 生命周期状态，其他位可记录待让出等修饰状态。
 */
typedef rt_uint8_t rt_sched_thread_status_t;

/**
 * @brief 一个线程的调度器私有优先级和时间片记录。
 *
 * 调度器实现代码以外的调用者绝不可直接访问这些成员。派生掩码必须与 current_priority 及
 * 就绪队列保持一致；只更新一个成员会使可运行线程无法被最高优先级查找发现。
 */
struct rt_sched_thread_priv
{
    /** 线程让出或开始新一轮时重新装载的时间片。 */
    rt_tick_t                   init_tick;
    /** 当前时间片剩余 tick；由调度 tick 递减。 */
    rt_tick_t                   remaining_tick;

    /** 当前用于选择就绪队列的有效优先级。 */
    rt_uint8_t                  current_priority;
    /** 取消临时继承时使用的配置/基础优先级。 */
    rt_uint8_t                  init_priority;
#if RT_THREAD_PRIORITY_MAX > 32
    /** 两级优先级位图中的组索引（`current_priority >> 3`）。 */
    rt_uint8_t                  number;
    /** 在八个优先级组成的组内选择此优先级的位。 */
    rt_uint8_t                  high_mask;
#endif /* RT_THREAD_PRIORITY_MAX > 32 */
    /**
     * 就绪组位。优先级不超过 32 时它直接选择优先级；更多时选择由 number 指定的组。
     */
    rt_uint32_t                 number_mask;

};

/**
 * @brief 嵌入每个线程控制块、对调度器可见的状态。
 *
 * 虽然历史名称中含有“public”，成员仅对协作的内核子系统开放，并不对应用开放。除非实现
 * 明确说明存在无锁初始化阶段，否则读取或写入可变成员前必须持有调度器锁。
 */
struct rt_sched_thread_ctx
{
    /** 同一时刻仅供一个调度器拥有的侵入式链表使用的节点。 */
    rt_list_t                   thread_list_node;

    /** 基础 RT_THREAD_* 生命周期状态加 RT_THREAD_STAT_* 修饰位。 */
    rt_uint8_t                  stat;
    /** 预留给调度器锁所有权记录的每线程标记。 */
    rt_uint8_t                  sched_flag_locked:1;
    /** 调度器以此标记嵌入的超时定时器是否在当前等待中生效。 */
    rt_uint8_t                  sched_flag_ttmr_set:1;

#ifdef ARCH_USING_HW_THREAD_SELF
    /** 线程处于临界区时延后的重新调度请求。 */
    rt_uint8_t                  critical_switch_flag:1;
#endif /* ARCH_USING_HW_THREAD_SELF */

#ifdef RT_USING_SMP
    /**
     * 请求的 CPU 亲和性。RT_CPUS_NR 是未绑定线程的哨兵值；否则该值为逻辑 CPU 索引。
     */
    rt_uint8_t                  bind_cpu;
    /**
     * 当前执行此线程的 CPU；未运行时为 RT_CPU_DETACHED。这可防止线程同时在两个 CPU 上运行。
     */
    rt_uint8_t                  oncpu;

    /** 此线程拥有的嵌套调度器临界区深度。 */
    rt_base_t                   critical_lock_nest;
#endif

    /** 由调度器维护的私有位图、优先级和时间片数据。 */
    struct rt_sched_thread_priv sched_thread_priv;
};

/** 以 ABI 规定的成员名将调度器上下文放入 `struct rt_thread`。 */
#define RT_SCHED_THREAD_CTX struct rt_sched_thread_ctx sched_thread_ctx;

/** 访问线程指针所指对象的调度器私有子对象。 */
#define RT_SCHED_PRIV(thread) ((thread)->sched_thread_ctx.sched_thread_priv)
/** 访问线程指针所指对象的调度器可见上下文。 */
#define RT_SCHED_CTX(thread) ((thread)->sched_thread_ctx)

/**
 * @brief 将调度器链表节点转换回其所属线程。
 *
 * 首先由 rt_list_entry() 从嵌入节点恢复 `struct rt_sched_thread_ctx`，再由
 * rt_container_of() 从嵌入的调度上下文恢复 `struct rt_thread`。@p node 必须确为
 * thread_list_node；传入任意链表节点会产生未定义的指针运算。
 */
#define RT_THREAD_LIST_NODE_ENTRY(node)                                      \
    rt_container_of(                                                         \
        rt_list_entry((node), struct rt_sched_thread_ctx, thread_list_node), \
        struct rt_thread, sched_thread_ctx)
/** 以左值形式返回线程的调度器链表节点。 */
#define RT_THREAD_LIST_NODE(thread) (RT_SCHED_CTX(thread).thread_list_node)

/**
 * @name 系统调度器加锁
 *
 * 调度器锁保护就绪队列和状态转换，防止本地中断处理函数以及 SMP 中其他 CPU 的并发访问。
 * 保存的 level 是不透明的恢复令牌，不是布尔值。每次成功加锁都必须在每条控制流路径上使用
 * 原样返回的值配对解锁一次。
 *
 * 这些短时内部锁不同于面向应用的临界区嵌套 API。持锁代码不得阻塞，也不得执行需要调度器
 * 推进的操作。
 * @{
 */

/** 由 rt_sched_lock() 保存的不透明中断/锁状态。 */
typedef rt_ubase_t rt_sched_lock_level_t;

/**
 * @brief 锁定调度器状态，并把先前 level 保存到 @p plvl。
 * @return 成功返回 RT_EOK；@p plvl 为 RT_NULL 时返回 -RT_EINVAL。
 */
rt_err_t rt_sched_lock(rt_sched_lock_level_t *plvl);

/**
 * @brief 解锁调度器状态，但不显式请求重新调度。
 * @param level 成功调用 rt_sched_lock() 返回的原始令牌。
 * @return 恢复保存的调度器/中断状态后返回 RT_EOK。
 */
rt_err_t rt_sched_unlock(rt_sched_lock_level_t level);

/**
 * @brief 解锁调度器状态并处理待定的调度决定。
 *
 * 根据 UP/SMP 配置和调用上下文，切换可能立即发生、延后到中断退出，或返回说明此时不能调度的错误。
 *
 * @param level 成功调用 rt_sched_lock() 返回的原始令牌。
 * @return 完成时返回 RT_EOK。SMP 实现在调度不可用、从 ISR 请求调度，或仍被外层调度器
 *         临界区锁定时可报告负状态。
 */
rt_err_t rt_sched_unlock_n_resched(rt_sched_lock_level_t level);

/**
 * 返回调用 CPU 是否拥有调度器上下文锁。
 *
 * 公共实现在 scheduler_mp.c 中才存在。本代码树没有 UP 定义，UP 代码不得直接调用该声明；
 * 在该配置下，下面公开的调试宏会展开为空。
 */
rt_bool_t rt_sched_is_locked(void);

#ifdef RT_USING_SMP
/* 仅用于调试的所有权断言；不会获取或释放任何锁。 */
#define RT_SCHED_DEBUG_IS_LOCKED do { RT_ASSERT(rt_sched_is_locked()); } while (0)
#define RT_SCHED_DEBUG_IS_UNLOCKED do { RT_ASSERT(!rt_sched_is_locked()); } while (0)

#else /* !RT_USING_SMP */

#define RT_SCHED_DEBUG_IS_LOCKED
#define RT_SCHED_DEBUG_IS_UNLOCKED
#endif /* RT_USING_SMP */
/** @} */

/**
 * @name 内核私有线程调度操作
 *
 * 用户代码绝不可直接调用这些接口。应使用 rt_thread_* 或 IPC API，它们会验证生命周期状态并
 * 执行完整的锁/链表/定时器协议。这些声明只在编译内核或 IPC 源码时公开，以避免组件和应用误用。
 * @{
 */
#if defined(__RT_KERNEL_SOURCE__) || defined(__RT_IPC_SOURCE__)

/**
 * 为新构造的线程初始化完整的调度上下文。设置其生命周期状态和 SMP 分离/亲和哨兵，然后初始化私有优先级和时间片数据。  封闭对象可能已经链接到对象注册表中，因此创建路径必须防止它变得可调度，直到初始化完成。
 *
 * @param thread 新建线程控制块。
 * @param tick 调度程序滴答数中的初始时间片长度。
 * @param priority 基本优先级在 [0, RT_THREAD_PRIORITY_MAX) 中。
 */
void rt_sched_thread_init_ctx(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority);

/**
 * 初始化侵入列表、优先级位图和时间片字段。 UP 和 SMP 提供单独的实现，因为它们的锁簿记不同。
 *
 * @param thread 私有调度数据未初始化的线程。
 * @param tick 初始和剩余时间片。
 * @param priority 初始有效和基本优先级。
 */
void rt_sched_thread_init_priv(struct rt_thread *thread, rt_uint32_t tick, rt_uint8_t priority);

/**
 * 根据当前优先级计算就绪位图掩码，并将新线程置于挂起状态，普通恢复/启动路径可以从中准备好它。
 * @param thread 已初始化的线程在调度队列中尚不可见。
 */
void rt_sched_thread_startup(struct rt_thread *thread);

/**
 * 在低级上下文发生更改后完成 SMP 调度程序簿记。作为堆栈指针切换事务的一部分，在禁用本地中断的情况下调用；不是应用程序的通用后切换挂钩。  尽管有此无条件声明，但通用实现仅存在于 SMP 调度程序中； UP 代码不得调用它，除非其端口提供覆盖。
 * @param thread 现在拥有处理器上下文的传入线程。
 */
void rt_sched_post_ctx_switch(struct rt_thread *thread);

/**
 * 将@p tick记入当前线程的时间片。耗尽时，它将线程标记为已屈服并请求重新调度。通常从系统节拍中断路径调用。
 * @return RT_EOK 在记帐和任何调度请求之后。
 */
rt_err_t rt_sched_tick_increase(rt_tick_t tick);

/**
 * 在屏蔽修饰符位后返回 @p thread 的基本 RT_THREAD_* 状态。必须持有调度程序锁。
 */
rt_uint8_t rt_sched_thread_get_stat(struct rt_thread *thread);

/** 返回@p thread的有效/当前优先级；必须持有调度程序锁。 */
rt_uint8_t rt_sched_thread_get_curr_prio(struct rt_thread *thread);

/** 返回 @p thread 的配置/基本优先级用作继承基线。 */
rt_uint8_t rt_sched_thread_get_init_prio(struct rt_thread *thread);

/**
 * 重新加载@p thread的时间片并在调度程序锁定下设置其产量修饰符。
 * @return RT_EOK。
 */
rt_err_t rt_sched_thread_yield(struct rt_thread *thread);

/**
 * 解决可运行/等待成员资格后，将 @p thread 标记为关闭。
 * @return RT_EOK;必须持有调度程序锁。
 */
rt_err_t rt_sched_thread_close(struct rt_thread *thread);

/**
 * 以原子方式将挂起的线程转换为就绪状态。如果超时计时器处于活动状态，它将首先停止，从而允许检测超时/生产者竞争，而无需将线程放入两个队列中。
 * @param thread 暂停线程以使其可运行；必须持有调度程序锁。
 * @return RT_EOK 表示成功，或者如果状态/计时器竞争阻止此调用者完成转换，则为负状态。
 */
rt_err_t rt_sched_thread_ready(struct rt_thread *thread);

/**
 * 保留调度程序/端口暂停转换声明。
 *
 * 此树中常见的 UP 和 SMP 调度程序源不提供此符号的通用定义或公共调用协定。  内核代码使用 rt_thread_suspend_to_list() 和现有的就绪/列表协议。提供此可选符号的端口必须定义 @p level 及其锁定和返回值协定的含义。
 */
rt_err_t rt_sched_thread_suspend(struct rt_thread *thread, rt_sched_lock_level_t level);

/**
 * 仅更改 @p thread 的有效优先级，例如在继承期间。就绪线程被删除并重新插入，因此队列/位图状态保持一致。必须保持调度程序锁，并且 @p priority 必须位于配置的优先级范围内。
 */
rt_err_t rt_sched_thread_change_priority(struct rt_thread *thread, rt_uint8_t priority);

/**
 * 将 @p thread 的有效和配置/基本优先级更改为 @p priority。就绪线程将根据新的优先级重新排队。必须保持调度程序锁，并且 @p priority 必须位于配置的优先级范围内。
 */
rt_err_t rt_sched_thread_reset_priority(struct rt_thread *thread, rt_uint8_t priority);

/**
 * 将线程绑定到SMP中的逻辑CPU @p cpu。有效绑定ID为`0 .. RT_CPUS_NR - 1`； RT_CPUS_NR 表示未绑定，较大的值由 SMP 实现标准化为该哨兵。负 ID 不是有效输入。 UP 实现拒绝带有 -RT_EINVAL 的操作。
 * @param thread 线程的就绪队列放置/关联性可能会更新。
 * 更新后 @return RT_EOK 位于 SMP 中，或 -RT_EINVAL 位于 UP 版本中。
 * @note SMP 实现本身获取调度程序锁，因此预计在没有持有该锁的情况下调用。
 */
rt_err_t rt_sched_thread_bind_cpu(struct rt_thread *thread, int cpu);

/**
 * 当 @p thread 携带调度程序挂起状态掩码时返回非零。必须持有调度程序锁。
 */
rt_uint8_t rt_sched_thread_is_suspended(struct rt_thread *thread);

/**
 * 停止@p thread的调度程序管理的超时计时器并清除其跟踪标志。
 * @return 如果没有活动定时器需要停止则为 RT_EOK，否则为 rt_timer_stop() 状态。即使停止报告错误，跟踪标志也会被清除。
 * @note 必须持有调度程序锁。
 */
rt_err_t rt_sched_thread_timer_stop(struct rt_thread *thread);

/**
 * 将嵌入式超时计时器标记为调度程序管理的当前等待。这会更新竞赛跟踪标志；计时器子系统围绕该调用执行插入计时器列表的操作。
 * @return RT_EOK;必须持有调度程序锁。
 */
rt_err_t rt_sched_thread_timer_start(struct rt_thread *thread);

/**
 * 将 @p thread 插入适当的就绪队列并更新位图/状态。产量/时间片状态确定头与尾放置的优先级。 SMP 实现断言调度程序锁已被持有； UP 内核调用者同样在外部序列化完整的状态转换。
 */
void rt_sched_insert_thread(struct rt_thread *thread);

/**
 * 从就绪队列中删除 @p thread 并清除现在为空的优先级位。 SMP 实现需要持有调度程序锁。
 */
void rt_sched_remove_thread(struct rt_thread *thread);

/**
 * 保留调度程序/端口当前线程访问器。
 *
 * 此树中的常见调度程序源使用 rt_thread_self() 并且不定义此符号。  代码不得调用它，除非所选的体系结构或调度程序扩展提供并记录了实现。
 */
struct rt_thread *rt_sched_thread_self(void);

#endif /* defined(__RT_KERNEL_SOURCE__) || defined(__RT_IPC_SOURCE__) */
/** @} */

#ifdef __cplusplus
}
#endif

#endif /* __RT_SCHED_H__ */
