/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2017/10/5      Bernard      the first version
 * 2018/09/17     Jesven       fix: in _signal_deliver RT_THREAD_STAT_MASK to RT_THREAD_STAT_SIGNAL_MASK
 * 2018/11/22     Jesven       in smp version rt_hw_context_switch_to add a param
 */

/**
 * @file signal.c
 * @brief RT-Thread 线程信号的登记、屏蔽、投递、等待与处理。
 *
 * 信号由三类线程字段配合表示：sig_pending 位图说明哪些信号已经投递；sig_mask 在
 * 本实现中以“位为 1 表示允许/未屏蔽”使用；sig_vectors 保存每个信号的处理函数。
 * 每次投递的详细 siginfo_t 放进 si_list，节点来自固定内存池，避免在关键路径使用
 * 不可预测的普通堆分配。_thread_signal_lock 保护这些字段及链表的并发访问。
 *
 * 投递不等于立即执行处理器：给自己投递且处于线程上下文时可直接处理；目标挂起时
 * 先唤醒；目标在其他上下文时设置 SIGNAL_PENDING，让调度/中断退出路径在目标线程
 * 真正运行时建立信号入口。用户 handler 始终在目标线程上下文、且不持信号锁执行。
 */

#include <stdint.h>
#include <string.h>

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_SIGNALS

#ifndef RT_SIG_INFO_MAX
    #ifdef ARCH_CPU_64BIT
        #define RT_SIG_INFO_MAX 64
    #else
        #define RT_SIG_INFO_MAX 32
    #endif /* ARCH_CPU_64BIT */
#endif /* RT_SIG_INFO_MAX */

#define DBG_TAG     "SIGN"
#define DBG_LVL     DBG_WARNING
#include <rtdbg.h>

#ifdef RT_USING_MUSLLIBC
    /* musl 的信号编号从 1 映射到位 0；其他配置按编号直接映射。 */
    #define sig_mask(sig_no)    (1u << (sig_no - 1))
#else
    #define sig_mask(sig_no)    (1u << sig_no)
#endif
#define sig_valid(sig_no)   (sig_no >= 0 && sig_no < RT_SIG_MAX)

/* 同时保护所有线程的信号位图、向量表指针和 siginfo 链。 */
static struct rt_spinlock _thread_signal_lock = RT_SPINLOCK_INIT;

/** 一个待处理信号的详细信息节点。 */
struct siginfo_node
{
    siginfo_t si;               /**< 信号编号、来源码和附带值。 */
    struct rt_slist_node list;  /**< 链接到目标线程 si_list 的单链表节点。 */
};

/* 启动时创建的固定块池，每块恰好容纳一个 siginfo_node。 */
static struct rt_mempool *_siginfo_pool;
static void _signal_deliver(rt_thread_t tid);
void rt_thread_handle_sig(rt_bool_t clean_state);

static void _signal_default_handler(int signo)
{
    RT_UNUSED(signo);
    LOG_I("handled signo[%d] with default action.", signo);
    return ;
}

static void _signal_entry(void *parameter)
{
    RT_UNUSED(parameter);

    rt_thread_t tid = rt_thread_self();

    /* 在目标线程自己的栈/上下文中依次运行可投递信号处理器。 */
    rt_thread_handle_sig(RT_FALSE);

#ifdef RT_USING_SMP
#else
    /* UP 保存了原 SP，处理完恢复原线程执行现场。 */
    tid->sp = tid->sig_ret;
    tid->sig_ret = RT_NULL;
#endif /* RT_USING_SMP */

    LOG_D("switch back to: 0x%08x\n", tid->sp);
    RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL;

#ifdef RT_USING_SMP
    rt_hw_context_switch_to((rt_uintptr_t)&parameter, tid);
#else
    rt_hw_context_switch_to((rt_uintptr_t)&(tid->sp));
#endif /* RT_USING_SMP */
}

/*
 * 把已经写入 pending/si_list 的信号推进到“目标线程能够处理”的阶段：
 * 1. 目标挂起：先恢复线程，并设置 SIGNAL 与 SIGNAL_PENDING；
 * 2. 目标就是当前线程：设置 SIGNAL，若不在 ISR 则立即处理；
 * 3. 目标是其他就绪/运行线程：设置 pending。UP 直接在目标栈上构造一次性
 *    _signal_entry 上下文；SMP 由目标 CPU 的调度/中断退出路径完成安全注入。
 * 屏蔽信号仍保留 pending，但不会触发上述动作。
 */
static void _signal_deliver(rt_thread_t tid)
{
    rt_base_t level;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    /* pending 与允许位图没有交集：保留信息，等待以后 unmask。 */
    if (!(tid->sig_pending & tid->sig_mask))
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        return;
    }

    if ((RT_SCHED_CTX(tid).stat & RT_THREAD_SUSPEND_MASK) == RT_THREAD_SUSPEND_MASK)
    {
        /* 处理器只能在线程运行时执行，先解除原等待。 */
#ifdef RT_USING_SMART
        rt_thread_wakeup(tid);
#else
        rt_thread_resume(tid);
#endif
        /* SIGNAL 表示处于信号处理流程，PENDING 请求调度边界注入。 */
        RT_SCHED_CTX(tid).stat |= (RT_THREAD_STAT_SIGNAL | RT_THREAD_STAT_SIGNAL_PENDING);

        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

        /* 刚唤醒的目标可能优先级更高。 */
        rt_schedule();
    }
    else
    {
        if (tid == rt_thread_self())
        {
            /* 当前线程无需构造远程上下文，只标记正在处理信号。 */
            RT_SCHED_CTX(tid).stat |= RT_THREAD_STAT_SIGNAL;

            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

            /* ISR 中不能调用用户处理器，交给之后的调度边界。 */
            if (rt_interrupt_get_nest() == 0)
            {
                rt_thread_handle_sig(RT_TRUE);
            }
        }
        else if (!((RT_SCHED_CTX(tid).stat & RT_THREAD_STAT_SIGNAL_MASK) & RT_THREAD_STAT_SIGNAL))
        {
            /* 避免为同一目标重复构造信号入口。 */
            RT_SCHED_CTX(tid).stat |= (RT_THREAD_STAT_SIGNAL | RT_THREAD_STAT_SIGNAL_PENDING);

#ifdef RT_USING_SMP
            {
                int cpu_id;

                cpu_id = RT_SCHED_CTX(tid).oncpu;
                if ((cpu_id != RT_CPU_DETACHED) && (cpu_id != rt_cpu_get_id()))
                {
                    rt_uint32_t cpu_mask;

                    cpu_mask = RT_CPU_MASK ^ (1 << cpu_id);
                    rt_hw_ipi_send(RT_SCHEDULE_IPI, cpu_mask);
                }
            }
#else
            /* UP 可直接保存原 SP，并在其栈附近构造 _signal_entry 初始帧。 */
            RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;
            tid->sig_ret = tid->sp;
            tid->sp = rt_hw_stack_init((void *)_signal_entry, RT_NULL,
                                       (void *)((char *)tid->sig_ret - 32), RT_NULL);
#endif /* RT_USING_SMP */

            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
            LOG_D("signal stack pointer @ 0x%08x", tid->sp);

            /* 让目标线程尽快运行新构造的入口。 */
            rt_schedule();
        }
        else
        {
            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        }
    }
}

#ifdef RT_USING_SMP
/**
 * @brief 在 SMP 中断返回边界检查是否要注入线程信号入口。
 *
 * 只有已经退出所有中断嵌套、且 current_thread 的 CPU 锁嵌套恰为切换路径预期值 1
 * 时，才消费 SIGNAL_PENDING 并基于当前异常现场构造 _signal_entry 栈帧。否则保持
 * 原 @p context，让更安全的后续边界重试。
 *
 * @param context 当前架构中断返回上下文。
 * @return 原上下文，或新构造的信号入口上下文。
 */
void *rt_signal_check(void* context)
{
    rt_sched_lock_level_t level;
    int cpu_id;
    struct rt_cpu* pcpu;
    struct rt_thread *current_thread;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    cpu_id = rt_cpu_get_id();
    pcpu   = rt_cpu_index(cpu_id);
    current_thread = pcpu->current_thread;

    if (pcpu->irq_nest)
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        return context;
    }

    if (current_thread->cpus_lock_nest == 1)
    {
        if (RT_SCHED_CTX(current_thread).stat & RT_THREAD_STAT_SIGNAL_PENDING)
        {
            void *sig_context;

            RT_SCHED_CTX(current_thread).stat &= ~RT_THREAD_STAT_SIGNAL_PENDING;

            rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
            sig_context = rt_hw_stack_init((void *)_signal_entry, context,
                    (void*)((char*)context - 32), RT_NULL);
            return sig_context;
        }
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
    return context;
}
#endif /* RT_USING_SMP */

/**
 * @brief 为当前线程的指定信号安装处理函数，并返回旧处理函数。
 *
 * 首次安装时按需分配整张向量表，所有槽默认指向 _signal_default_handler。SIG_IGN
 * 在内部存成 RT_NULL，SIG_DFL 恢复默认处理器，其他值直接作为用户 handler。
 * 分配动作在释放自旋锁后执行，避免持锁调用堆；重新加锁后处理并发分配竞争。
 *
 * @note 还需调用 rt_signal_unmask() 允许该信号，处理函数才会被投递执行。
 *
 * @see      rt_signal_unmask()
 *
 * @param signo 信号编号，必须满足 0 <= signo < RT_SIG_MAX。
 *
 * @param handler 新处理函数，或 SIG_IGN/SIG_DFL。
 *
 * @return 旧处理函数；仅 SIG_ERR 表示操作失败。
 */
rt_sighandler_t rt_signal_install(int signo, rt_sighandler_t handler)
{
    rt_base_t level;
    rt_sighandler_t old = RT_NULL;
    rt_thread_t tid = rt_thread_self();

    if (!sig_valid(signo)) return SIG_ERR;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    if (tid->sig_vectors == RT_NULL)
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

        rt_thread_alloc_sig(tid);

        level = rt_spin_lock_irqsave(&_thread_signal_lock);
    }

    if (tid->sig_vectors)
    {
        old = tid->sig_vectors[signo];

        if (handler == SIG_IGN) tid->sig_vectors[signo] = RT_NULL;
        else if (handler == SIG_DFL) tid->sig_vectors[signo] = _signal_default_handler;
        else tid->sig_vectors[signo] = handler;
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    return old;
}

/**
 * @brief 屏蔽当前线程的指定信号。
 *
 * 清除 sig_mask 对应位。之后投递仍可记录在 sig_pending/si_list 中，但不会执行，
 * 直到重新 unmask。
 *
 * @see      rt_thread_kill()
 *
 * @param signo 要屏蔽的信号编号；调用者应保证编号有效。
 */
void rt_signal_mask(int signo)
{
    rt_base_t level;
    rt_thread_t tid = rt_thread_self();

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    tid->sig_mask &= ~sig_mask(signo);

    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
}

/**
 * @brief 解除当前线程对指定信号的屏蔽。
 *
 * 设置 sig_mask 位后，若它与已有 sig_pending 相交，则先释放信号锁，再调用
 * _signal_deliver() 推进投递。先解锁可避免递归获取同一自旋锁。
 *
 * @see      rt_thread_kill()
 *
 * @param signo 要解除屏蔽的信号编号；调用者应保证编号有效。
 */
void rt_signal_unmask(int signo)
{
    rt_base_t level;
    rt_thread_t tid = rt_thread_self();

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    tid->sig_mask |= sig_mask(signo);

    /* 若刚解除屏蔽的集合中已有 pending，立即推进目标线程处理。 */
    if (tid->sig_mask & tid->sig_pending)
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
        _signal_deliver(tid);
    }
    else
    {
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
    }
}

/**
 * @brief 同步等待集合中的任一信号，并取出对应 siginfo。
 *
 * 若已有匹配 pending，立即从 si_list 取出第一项；否则 timeout 为 0 时立即超时，
 * 其他情况把自身以不可中断方式挂起并置 SIGNAL_WAIT。有限超时复用线程内置定时器，
 * 调度回来后清等待标志，区分超时唤醒与信号唤醒。成功取出节点后复制信息、修复
 * 单链表、清 pending 位并把节点归还内存池。
 *
 * @param set 等待位图，通常用 sigaddset() 构造；不得为空或为 0。
 *
 * @param si 输出收到的信号信息；本实现要求非 RT_NULL，并在等待前清零。
 *
 * @param timeout 等待节拍数；0 表示轮询，RT_WAITING_FOREVER 表示无限等待。
 * @return 成功为 RT_EOK；参数错误为 -RT_EINVAL；到期为 -RT_ETIMEOUT。
 * @note 只能在线程上下文调用，因为该函数可能挂起并调度。
 */
int rt_signal_wait(const rt_sigset_t *set, rt_siginfo_t *si, rt_int32_t timeout)
{
    int ret = RT_EOK;
    rt_base_t level;
    rt_thread_t tid = rt_thread_self();
    struct siginfo_node *si_node = RT_NULL, *si_prev = RT_NULL;

    /* 信号同步等待会阻塞，ISR 上下文非法。 */
    RT_DEBUG_IN_THREAD_CONTEXT;

    /* 本实现即使调用者“不关心信息”也要求提供有效输出缓冲。 */
    if (set == NULL || *set == 0 || si == NULL )
    {
        ret = -RT_EINVAL;
        goto __done_return;
    }

    /* 先清零，失败返回时也不会暴露未初始化内容。 */
    memset(si, 0x0, sizeof(rt_siginfo_t));

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    /* 已有匹配信号时无需挂起，直接进入链表提取。 */
    if (tid->sig_pending & *set) goto __done;

    if (timeout == 0)
    {
        ret = -RT_ETIMEOUT;
        goto __done_int;
    }

    /* 原子地将自己从运行态转为挂起，等待信号路径负责恢复。 */
    rt_thread_suspend_with_flag(tid, RT_UNINTERRUPTIBLE);
    /* 区分同步 signal_wait 与异步 handler 投递。 */
    RT_SCHED_CTX(tid).stat |= RT_THREAD_STAT_SIGNAL_WAIT;

    /* 有限等待复用线程内置单次超时定时器。 */
    if (timeout != RT_WAITING_FOREVER)
    {
        rt_tick_t timeout_tick = timeout;
        /* 每次等待前重设超时，因为同一定时器被各种阻塞 API 复用。 */
        rt_timer_control(&(tid->thread_timer),
                         RT_TIMER_CTRL_SET_TIME,
                         &timeout_tick);
        rt_timer_start(&(tid->thread_timer));
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    /* 当前线程已挂起，切换给其他就绪线程。 */
    rt_schedule();

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    /* 无论因何唤醒，本次同步等待阶段都已结束。 */
    RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL_WAIT;

    /* 超时回调通过线程 error 字段报告唤醒原因。 */
    if (tid->error == -RT_ETIMEOUT)
    {
        tid->error = RT_EOK;
        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

        /* 恢复通用 error 槽，向调用者返回本次超时。 */
        ret = -RT_ETIMEOUT;
        goto __done_return;
    }

__done:
    /* 按投递链顺序寻找集合中第一条详细信息。 */
    si_node = (struct siginfo_node *)tid->si_list;
    while (si_node)
    {
        int signo;

        signo = si_node->si.si_signo;
        if (sig_mask(signo) & *set)
        {
            *si  = si_node->si;

            LOG_D("sigwait: %d sig raised!", signo);
            if (si_prev) si_prev->list.next = si_node->list.next;
            else
            {
                struct siginfo_node *node_next;

                if (si_node->list.next)
                {
                    node_next = (void *)rt_slist_entry(si_node->list.next, struct siginfo_node, list);
                    tid->si_list = node_next;
                }
                else
                {
                    tid->si_list = RT_NULL;
                }
            }

            /* 当前实现每个信号号只保留一个 pending 节点，取走即可清位。 */
            tid->sig_pending &= ~sig_mask(signo);
            rt_mp_free(si_node);
            break;
        }

        si_prev = si_node;
        if (si_node->list.next)
        {
            si_node = (void *)rt_slist_entry(si_node->list.next, struct siginfo_node, list);
        }
        else
        {
            si_node = RT_NULL;
        }
     }

__done_int:
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

__done_return:
    return ret;
}

/**
 * @brief 在当前线程上下文依次处理所有未屏蔽 pending 信号。
 *
 * 每轮在锁内取下 si_list 首节点、查询 handler 并清 pending 位，然后释放锁调用用户
 * handler；这既避免长时间持有自旋锁，也允许 handler 再投递信号。返回后重新加锁、
 * 归还节点，并把线程 error 设为 -RT_EINTR，表示原阻塞调用被信号打断。处于
 * SIGNAL_WAIT 的线程由 rt_signal_wait() 消费节点，本函数不会抢走它们。
 *
 * @param clean_state 为 RT_TRUE 时，全部处理后清除 RT_THREAD_STAT_SIGNAL；为
 *                    RT_FALSE 时由 _signal_entry 的返回路径稍后清除。
 */
void rt_thread_handle_sig(rt_bool_t clean_state)
{
    rt_base_t level;

    rt_thread_t tid = rt_thread_self();
    struct siginfo_node *si_node;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    if (tid->sig_pending & tid->sig_mask)
    {
        /* 同步等待者必须由 wait API 选择集合并提取信息。 */
        if (!(RT_SCHED_CTX(tid).stat & RT_THREAD_STAT_SIGNAL_WAIT))
        {
            while (tid->sig_pending & tid->sig_mask)
            {
                int signo, error;
                rt_sighandler_t handler;

                si_node = (struct siginfo_node *)tid->si_list;
                if (!si_node) break;

                /* 在锁内从表头摘除，保证只有当前处理路径拥有该节点。 */
                if (si_node->list.next == RT_NULL)
                    tid->si_list = RT_NULL;
                else
                    tid->si_list = (void *)rt_slist_entry(si_node->list.next, struct siginfo_node, list);

                signo   = si_node->si.si_signo;
                handler = tid->sig_vectors[signo];
                tid->sig_pending &= ~sig_mask(signo);
                rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

                LOG_D("handle signal: %d, handler 0x%08x", signo, handler);
                if (handler) handler(signo);

                level = rt_spin_lock_irqsave(&_thread_signal_lock);
                error = -RT_EINTR;

                rt_mp_free(si_node); /* 处理完成，将固定块归还信号内存池。 */
                /* 让可能被信号打断的上层等待看到 EINTR。 */
                tid->error = error;
            }

            /* 直接处理路径在这里清状态；专用入口需要保留到恢复原 SP 前。 */
            if (clean_state == RT_TRUE)
            {
                RT_SCHED_CTX(tid).stat &= ~RT_THREAD_STAT_SIGNAL;
            }
            else
            {
                rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
                return;
            }
        }
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
}

/**
 * @brief 按需为线程分配并安装信号处理向量表。
 *
 * 先在锁外分配并填入默认处理器，再加锁用“仅当仍为空才安装”解决两个 CPU 同时
 * 首次分配的竞态。竞争失败的一方在锁外释放自己的多余表。
 *
 * @param tid 目标线程。
 */
void rt_thread_alloc_sig(rt_thread_t tid)
{
    int index;
    rt_bool_t need_free = RT_FALSE;
    rt_base_t level;
    rt_sighandler_t *vectors;

    vectors = (rt_sighandler_t *)RT_KERNEL_MALLOC(sizeof(rt_sighandler_t) * RT_SIG_MAX);
    RT_ASSERT(vectors != RT_NULL);

    for (index = 0; index < RT_SIG_MAX; index ++)
    {
        vectors[index] = _signal_default_handler;
    }

    level = rt_spin_lock_irqsave(&_thread_signal_lock);

    if (tid->sig_vectors == RT_NULL)
    {
        tid->sig_vectors = vectors;
    }
    else
    {
        need_free = RT_TRUE;
    }

    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    if (need_free)
    {
        rt_free(vectors);
    }
}

/**
 * @brief 释放线程拥有的全部信号信息节点和处理向量表。
 *
 * 先在锁内把 si_list 与 sig_vectors 从线程控制块摘下，随后在锁外逐节点归还内存池
 * 并释放向量表，避免持自旋锁执行释放器。线程退出的僵尸回收阶段调用本函数。
 *
 * @param tid 正在销毁、不会再接收新信号的线程。
 */
void rt_thread_free_sig(rt_thread_t tid)
{
    rt_base_t level;
    struct siginfo_node *si_node;
    rt_sighandler_t *sig_vectors;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    si_node = (struct siginfo_node *)tid->si_list;
    tid->si_list = RT_NULL;

    sig_vectors = tid->sig_vectors;
    tid->sig_vectors = RT_NULL;
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    if (si_node)
    {
        struct rt_slist_node *node;
        struct rt_slist_node *node_to_free;

        LOG_D("free signal info list");
        node = &(si_node->list);
        do
        {
            node_to_free = node;
            node = node->next;
            si_node = rt_slist_entry(node_to_free, struct siginfo_node, list);
            rt_mp_free(si_node);
        } while (node);
    }

    if (sig_vectors)
    {
        RT_KERNEL_FREE(sig_vectors);
    }
}

/**
 * @ingroup group_thread_management
 *
 * @brief 向指定线程投递一个信号。
 *
 * 先构造 SI_USER 类型的 siginfo。若同编号信号已经 pending，则在锁内更新原节点，
 * 不重复占用内存池；否则在锁外从固定池分配节点，再加锁追加到目标 si_list 并置
 * pending 位。最后调用 _signal_deliver() 根据目标状态唤醒、注入或延迟处理。
 *
 * @param tid 接收线程，不得为 RT_NULL，且生命周期必须覆盖投递过程。
 *
 * @param sig 信号编号，范围为 [0, RT_SIG_MAX)。
 * @return 成功为 RT_EOK；编号无效为 -RT_EINVAL；信息池耗尽为 -RT_EEMPTY。
 */
int rt_thread_kill(rt_thread_t tid, int sig)
{
    siginfo_t si;
    rt_base_t level;
    struct siginfo_node *si_node;

    RT_ASSERT(tid != RT_NULL);
    if (!sig_valid(sig)) return -RT_EINVAL;

    LOG_I("send signal: %d", sig);
    si.si_signo = sig;
    si.si_code  = SI_USER;
    si.si_value.sival_ptr = RT_NULL;

    level = rt_spin_lock_irqsave(&_thread_signal_lock);
    if (tid->sig_pending & sig_mask(sig))
    {
        /* 标准信号按编号合并：已有 pending 时只刷新详细信息。 */
        struct rt_slist_node *node;
        struct siginfo_node  *entry;

        si_node = (struct siginfo_node *)tid->si_list;
        if (si_node)
            node = (struct rt_slist_node *)&si_node->list;
        else
            node = RT_NULL;

        /* 找到同编号节点后原地覆盖，pending 位保持不变。 */
        for (; (node) != RT_NULL; node = node->next)
        {
            entry = rt_slist_entry(node, struct siginfo_node, list);
            if (entry->si.si_signo == sig)
            {
                memcpy(&(entry->si), &si, sizeof(siginfo_t));
                rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
                return 0;
            }
        }
    }
    rt_spin_unlock_irqrestore(&_thread_signal_lock, level);

    si_node = (struct siginfo_node *) rt_mp_alloc(_siginfo_pool, 0);
    if (si_node)
    {
        rt_slist_init(&(si_node->list));
        memcpy(&(si_node->si), &si, sizeof(siginfo_t));

        level = rt_spin_lock_irqsave(&_thread_signal_lock);

        if (tid->si_list)
        {
            struct siginfo_node *si_list;

            si_list = (struct siginfo_node *)tid->si_list;
            rt_slist_append(&(si_list->list), &(si_node->list));
        }
        else
        {
            tid->si_list = si_node;
        }

        /* 链表节点完全可见后再置 pending 位。 */
        tid->sig_pending |= sig_mask(sig);

        rt_spin_unlock_irqrestore(&_thread_signal_lock, level);
    }
    else
    {
        LOG_E("The allocation of signal info node failed.");
        return -RT_EEMPTY;
    }

    /* 根据屏蔽、目标状态和 UP/SMP 模式推进实际投递。 */
    _signal_deliver(tid);

    return RT_EOK;
}

/**
 * @brief 初始化全局信号信息固定内存池。
 *
 * 池包含 RT_SIG_INFO_MAX 个等长 siginfo_node，必须在线程投递信号前由系统启动链
 * 调用。创建失败表示信号子系统无法工作，因此记录错误并触发断言。
 *
 * @return 成功返回 0。
 */
int rt_system_signal_init(void)
{
    _siginfo_pool = rt_mp_create("signal", RT_SIG_INFO_MAX, sizeof(struct siginfo_node));
    if (_siginfo_pool == RT_NULL)
    {
        LOG_E("create memory pool for signal info failed.");
        RT_ASSERT(0);
    }

    return 0;
}

#endif /* RT_USING_SIGNALS */
