/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-02-24     Bernard      first version
 * 2006-05-03     Bernard      add IRQ_DEBUG
 * 2016-08-09     ArdaFu       add interrupt enter and leave hook.
 * 2018-11-22     Jesven       rt_interrupt_get_nest function add disable irq
 * 2021-08-15     Supperthomas fix the comment
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to irq.c
 * 2022-07-04     Yunjie       fix RT_DEBUG_LOG
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2024-01-05     Shell        Fixup of data racing in rt_interrupt_get_nest
 * 2024-01-03     Shell        Support for interrupt context
*/

/**
 * @file irq.c
 * @brief 维护中断嵌套层数、中断上下文链以及中断进入/离开钩子。
 *
 * BSP/架构的中断汇编入口在调用具体 ISR 前应调用 rt_interrupt_enter()，返回前调用
 * rt_interrupt_leave()。嵌套计数让内核区分普通线程上下文与一层或多层中断上下文，
 * 从而把需要的调度延后到最外层中断退出。UP 使用单个全局计数，SMP 则每个 CPU
 * 独立维护 irq_nest，避免一个 CPU 的中断影响另一个 CPU 的上下文判断。
 */

#include <rthw.h>
#include <rtthread.h>

#define DBG_TAG           "kernel.irq"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)

void (*rt_interrupt_enter_hook)(void);
void (*rt_interrupt_leave_hook)(void);

/**
 * @ingroup group_hook
 *
 * @brief 设置中断进入钩子。
 *
 * 钩子在 rt_interrupt_enter() 已递增嵌套层数之后调用，处于 ISR 上下文，可以通过
 * rt_interrupt_get_nest() 观察当前层数。它可能在每一层嵌套中断触发，必须短小、
 * 可重入，不能阻塞、挂起或调用只允许在线程上下文使用的 API。
 *
 * @param hook 回调函数；RT_NULL 表示取消。再次设置会覆盖旧回调。
 */
void rt_interrupt_enter_sethook(void (*hook)(void))
{
    rt_interrupt_enter_hook = hook;
}

/**
 * @ingroup group_hook
 *
 * @brief 设置中断离开钩子。
 *
 * 钩子在嵌套层数递减之前调用，所以它看到的仍是当前中断层数。与进入钩子一样，
 * 它运行在 ISR 退出路径，必须短小、不可阻塞，并注意嵌套中断带来的可重入性。
 *
 * @param hook 回调函数；RT_NULL 表示取消。再次设置会覆盖旧回调。
 */
void rt_interrupt_leave_sethook(void (*hook)(void))
{
    rt_interrupt_leave_hook = hook;
}
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_kernel_core
 */

/**@{*/

#ifdef RT_USING_SMP
/* SMP 的中断嵌套是 per-CPU 数据。 */
#define rt_interrupt_nest rt_cpu_self()->irq_nest
#else
/* UP 只有一个执行 CPU；原子类型保证读取与中断更新之间不会出现撕裂。 */
volatile rt_atomic_t rt_interrupt_nest = 0;
#endif /* RT_USING_SMP */

#ifdef ARCH_USING_IRQ_CTX_LIST
void rt_interrupt_context_push(rt_interrupt_context_t this_ctx)
{
    /* 嵌套中断把最新上下文压到单链表头，形成按进入顺序反向排列的栈。 */
    struct rt_cpu *this_cpu = rt_cpu_self();
    rt_slist_insert(&this_cpu->irq_ctx_head, &this_ctx->node);
}

void rt_interrupt_context_pop(void)
{
    /* 最外层返回前弹出当前上下文，使上一级中断重新成为链表首项。 */
    struct rt_cpu *this_cpu = rt_cpu_self();
    rt_slist_pop(&this_cpu->irq_ctx_head);
}

void *rt_interrupt_context_get(void)
{
    /* 返回当前最内层中断由架构保存的上下文地址。 */
    struct rt_cpu *this_cpu = rt_cpu_self();
    return rt_slist_first_entry(&this_cpu->irq_ctx_head, struct rt_interrupt_context, node)->context;
}
#endif /* ARCH_USING_IRQ_CTX_LIST */

/**
 * @brief 标记当前 CPU 进入一层中断服务程序。
 *
 * 先原子增加嵌套层数，再调用进入钩子，所以钩子能看到非零层数。该弱实现允许架构
 * 在保持同等语义的前提下覆盖。每次调用都必须在同一路径最终配对一次 leave。
 *
 * @note 仅供 BSP/架构中断入口调用，应用代码不得伪造中断上下文。
 *
 * @see rt_interrupt_leave
 */
rt_weak void rt_interrupt_enter(void)
{
    rt_atomic_add(&(rt_interrupt_nest), 1);
    RT_OBJECT_HOOK_CALL(rt_interrupt_enter_hook,());
    LOG_D("irq has come..., irq current nest:%d",
          (rt_int32_t)rt_atomic_load(&(rt_interrupt_nest)));
}
RTM_EXPORT(rt_interrupt_enter);


/**
 * @brief 标记当前 CPU 离开一层中断服务程序。
 *
 * 离开钩子在计数递减前触发；随后嵌套层数减一。外层架构退出路径会依据最终层数和
 * 调度请求决定是否执行中断后的线程切换。
 *
 * @note 仅供 BSP/架构中断出口调用，且必须与 enter 严格配对。
 *
 * @see rt_interrupt_enter
 */
rt_weak void rt_interrupt_leave(void)
{
    LOG_D("irq is going to leave, irq current nest:%d",
                 (rt_int32_t)rt_atomic_load(&(rt_interrupt_nest)));
    RT_OBJECT_HOOK_CALL(rt_interrupt_leave_hook,());
    rt_atomic_sub(&(rt_interrupt_nest), 1);

}
RTM_EXPORT(rt_interrupt_leave);


/**
 * @brief 返回当前 CPU 的中断嵌套层数。
 *
 * 读取期间短暂关闭本地中断，避免本 CPU 在取值时又进入/退出一层中断。返回 0
 * 表示线程上下文，非 0 表示 ISR 上下文；数值本身表示当前嵌套深度。
 *
 * @return 当前中断嵌套层数。
 */
rt_weak rt_uint8_t rt_interrupt_get_nest(void)
{
    rt_uint8_t ret;
    rt_base_t level;

    level = rt_hw_local_irq_disable();
    ret = rt_atomic_load(&rt_interrupt_nest);
    rt_hw_local_irq_enable(level);
    return ret;
}
RTM_EXPORT(rt_interrupt_get_nest);

RTM_EXPORT(rt_hw_interrupt_disable);
RTM_EXPORT(rt_hw_interrupt_enable);

rt_weak rt_bool_t rt_hw_interrupt_is_disabled(void)
{
    /* 通用弱占位无法查询架构状态；需要准确结果的 BSP 应提供强实现覆盖。 */
    return RT_FALSE;
}
RTM_EXPORT(rt_hw_interrupt_is_disabled);
/**@}*/
