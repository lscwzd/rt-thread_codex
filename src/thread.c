/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2006-03-28     Bernard      first version
 * 2006-04-29     Bernard      implement thread timer
 * 2006-04-30     Bernard      added THREAD_DEBUG
 * 2006-05-27     Bernard      fixed the rt_thread_yield bug
 * 2006-06-03     Bernard      fixed the thread timer init bug
 * 2006-08-10     Bernard      fixed the timer bug in thread_sleep
 * 2006-09-03     Bernard      changed rt_timer_delete to rt_timer_detach
 * 2006-09-03     Bernard      implement rt_thread_detach
 * 2008-02-16     Bernard      fixed the rt_thread_timeout bug
 * 2010-03-21     Bernard      change the errno of rt_thread_delay/sleep to
 *                             RT_EOK.
 * 2010-11-10     Bernard      add cleanup callback function in thread exit.
 * 2011-09-01     Bernard      fixed rt_thread_exit issue when the current
 *                             thread preempted, which reported by Jiaxing Lee.
 * 2011-09-08     Bernard      fixed the scheduling issue in rt_thread_startup.
 * 2012-12-29     Bernard      fixed compiling warning.
 * 2016-08-09     ArdaFu       add thread suspend and resume hook.
 * 2017-04-10     armink       fixed the rt_thread_delete and rt_thread_detach
 *                             bug when thread has not startup.
 * 2018-11-22     Jesven       yield is same to rt_schedule
 *                             add support for tasks bound to cpu
 * 2021-02-24     Meco Man     rearrange rt_thread_control() - schedule the thread when close it
 * 2021-11-15     THEWON       Remove duplicate work between idle and _thread_exit
 * 2021-12-27     Meco Man     remove .init_priority
 * 2022-01-07     Gabriel      Moving __on_rt_xxxxx_hook to thread.c
 * 2022-01-24     THEWON       let _thread_sleep return thread->error when using signal
 * 2022-10-15     Bernard      add nested mutex feature
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
 * 2023-12-10     xqyjlj       fix thread_exit/detach/delete
 *                             fix rt_thread_delay
 */

/**
 * @file thread.c
 * @brief 线程生命周期、栈初始化、睡眠、挂起/恢复和通用控制 API。
 *
 * 初学者可按下面的状态链理解本文件：rt_thread_init/create 建立 INIT 线程和初始栈；
 * rt_thread_startup 计算调度属性并借 resume 路径进入 READY；调度器选中后为 RUNNING；
 * 延时或等待 IPC 时从就绪/运行集合移出成为不同类型的 SUSPEND；超时、资源到达或
 * resume 又使它 READY；入口函数返回后自动进入 _thread_exit，最终标记 CLOSE 并放入
 * defunct 队列，由安全的后台上下文释放栈和控制块。
 *
 * 线程内置 thread_timer 被所有有超时的阻塞操作复用。挂起链表、定时器和线程状态
 * 必须在同一个调度锁事务中变化，否则“资源唤醒”和“超时 ISR”可能重复恢复线程。
 * UP/SMP 的队列实现不同，但本文件通过 rtsched 公共接口保持相同生命周期语义。
 */

#include <rthw.h>
#include <rtthread.h>
#include <stddef.h>

#define DBG_TAG           "kernel.thread"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
static void (*rt_thread_suspend_hook)(rt_thread_t thread);
static void (*rt_thread_resume_hook) (rt_thread_t thread);

/**
 * @brief 设置线程成功进入挂起态后的钩子。
 *
 * 钩子在 rt_thread_suspend_to_list() 已完成状态/等待链/定时器更新并释放调度锁后调用。
 * 调用者上下文可能是线程或内核路径；钩子必须短小、不可阻塞，也不要递归改变同一
 * 线程状态。再次设置会覆盖，RT_NULL 取消。
 *
 * @param hook 接收被挂起线程的回调。
 */
void rt_thread_suspend_sethook(void (*hook)(rt_thread_t thread))
{
    rt_thread_suspend_hook = hook;
}

/**
 * @brief 设置一次线程恢复尝试结束后的钩子。
 *
 * 钩子在 rt_thread_resume() 的调度锁处理完成后调用，并接收目标线程。当前代码即使
 * ready 操作返回错误也会触发该钩子，所以跟踪者不能仅凭钩子调用推断恢复成功。
 * 钩子必须短小、不可阻塞；RT_NULL 取消。
 *
 * @param hook 接收目标线程的回调。
 */
void rt_thread_resume_sethook(void (*hook)(rt_thread_t thread))
{
    rt_thread_resume_hook = hook;
}

/*
 * 线程初始化钩子使用可挂多个节点的 hook-list 机制，而非单函数指针。它在
 * _thread_init() 最后、线程仍为 INIT 且尚未进入就绪队列时依次调用所有节点。
 */
RT_OBJECT_HOOKLIST_DEFINE(rt_thread_inited);
#endif /* defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR) */

#ifdef RT_USING_MUTEX
static void _thread_detach_from_mutex(rt_thread_t thread)
{
    rt_list_t *node;
    rt_list_t *tmp_list;
    struct rt_mutex *mutex;
    rt_base_t level;

    level = rt_spin_lock_irqsave(&thread->spinlock);

    /* 若线程正排在某互斥量等待队列，先撤销等待及其优先级继承关系。 */
    if ((thread->pending_object) &&
        (rt_object_get_type(thread->pending_object) == RT_Object_Class_Mutex))
    {
        /* drop_thread 负责从互斥量等待结构摘除该线程。 */
        struct rt_mutex *mutex = (struct rt_mutex*)thread->pending_object;
        rt_mutex_drop_thread(mutex, thread);
        thread->pending_object = RT_NULL;
    }

    /*
     * 再逐一释放线程仍持有的互斥量。必须先取消“正在等待”的关系，以免退出线程在
     * 竞态中刚获得某个锁却遗漏释放。safe 遍历允许 release 修改 taken_object_list。
     */
    rt_list_for_each_safe(node, tmp_list, &(thread->taken_object_list))
    {
        mutex = rt_list_entry(node, struct rt_mutex, taken_list);
        LOG_D("Thread [%s] exits while holding mutex [%s].\n", thread->parent.name, mutex->parent.parent.name);
        /* 把递归持有层数归一为 1，使一次 release 完成所有权转移。 */
        mutex->hold = 1;
        rt_mutex_release(mutex);
    }

    rt_spin_unlock_irqrestore(&thread->spinlock, level);
}

#else

static void _thread_detach_from_mutex(rt_thread_t thread) {}
#endif

static void _thread_exit(void)
{
    struct rt_thread *thread;
    rt_base_t critical_level;

    /* 线程入口函数返回时由初始栈帧自动跳转到这里。 */
    thread = rt_thread_self();

    critical_level = rt_enter_critical();

    rt_thread_close(thread);

    _thread_detach_from_mutex(thread);

    /* 当前仍使用自己的栈，不能立即释放，只能交给后台回收。 */
    rt_thread_defunct_enqueue(thread);

    rt_exit_critical_safe(critical_level);

    /* CLOSE 线程不会再次运行，切换到其他就绪线程。 */
    rt_schedule();
}

/**
 * @brief 线程阻塞等待到期时由其内置定时器调用的统一超时回调。
 *
 * 在调度锁内断言目标仍挂起，写入 -RT_ETIMEOUT，从等待链表摘除并插入就绪队列，
 * 最后解锁并请求调度。状态、链表和调度请求是同一事务，避免与资源唤醒重复操作。
 * 该回调通常运行于硬定时器 ISR 或软定时器线程，具体取决于定时器配置。
 *
 * @param parameter 创建线程定时器时保存的目标 rt_thread 指针。
 */
static void _thread_timeout(void *parameter)
{
    struct rt_thread *thread;
    rt_sched_lock_level_t slvl;

    thread = (struct rt_thread *)parameter;

    /* 定时器参数必须仍指向有效线程对象。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    rt_sched_lock(&slvl);

    /**
     * 恢复线程与停止其超时定时器必须是原子竞争。回调已经获胜时，线程理应仍挂起；
     * 若已恢复说明唤醒路径没有正确停止定时器。
     */
    RT_ASSERT(rt_sched_thread_is_suspended(thread));

    /* 阻塞 API 在重新运行后据此区分超时与正常资源唤醒。 */
    thread->error = -RT_ETIMEOUT;

    /* 同一个调度 list 节点从等待链摘下后才能复用于就绪链。 */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));
    /* 目标现在可再次被调度。 */
    rt_sched_insert_thread(thread);
    /* 若目标优先级更高，安全时机立即抢占。 */
    rt_sched_unlock_n_resched(slvl);
}

/**
 * @brief 初始化静态/动态线程共同拥有的全部运行时字段。
 *
 * 调用者已经完成对象层面的 init/allocate，本函数只建立线程语义：
 * - 调度上下文保存状态、基础/有效优先级和初始/剩余时间片；
 * - entry/parameter 决定第一次恢复后的 C 入口；stack_addr/stack_size 描述栈所有区；
 * - sp 指向 rt_hw_stack_init() 构造的初始寄存器帧，入口返回地址指向 _thread_exit；
 * - thread_timer 是各类延时和带超时等待共用的一次性定时器；
 * - taken_object_list/pending_object 分别跟踪已持有与正在等待的互斥量；
 * - cleanup/user_data、信号、Smart/LWP、pthread、module 和 CPU 统计字段按配置清零；
 * - spinlock 最后初始化，再调用线程 initialized hook-list。
 *
 * @param thread 已注册为 Thread 类型且内存有效的控制块。
 * @param name 对象名已由上层保存；此处仅保留统一内部签名。
 * @param entry 线程入口函数。
 * @param parameter 入口参数。
 * @param stack_start 连续栈内存起点。
 * @param stack_size 栈字节数。
 * @param priority 初始优先级。
 * @param tick 同优先级轮转时间片。
 * @return 成功返回 RT_EOK。
 */
static rt_err_t _thread_init(struct rt_thread *thread,
                             const char       *name,
                             void (*entry)(void *parameter),
                             void             *parameter,
                             void             *stack_start,
                             rt_uint32_t       stack_size,
                             rt_uint8_t        priority,
                             rt_uint32_t       tick)
{
    RT_UNUSED(name);

    rt_sched_thread_init_ctx(thread, tick, priority);

#ifdef RT_USING_MEM_PROTECTION
    thread->mem_regions = RT_NULL;
#endif

#ifdef RT_USING_SMART
    thread->wakeup_handle.func = RT_NULL;
#endif

    thread->entry = (void *)entry;
    thread->parameter = parameter;

    /* 记录调用者提供或动态分配的连续栈区。 */
    thread->stack_addr = stack_start;
    thread->stack_size = stack_size;

    /*
     * 整栈填充 '#' 既便于统计高水位，也让软件栈溢出检查能检测边界哨兵。随后由架构
     * 构造初始寄存器帧：第一次恢复时从 entry(parameter) 开始，entry 返回则自动
     * 转到 _thread_exit，防止落入未知地址。
     */
    rt_memset(thread->stack_addr, '#', thread->stack_size);
#ifdef RT_USING_HW_STACK_GUARD
    rt_hw_stack_guard_init(thread);
#endif
#ifdef ARCH_CPU_STACK_GROWS_UPWARD
    thread->sp = (void *)rt_hw_stack_init(thread->entry, thread->parameter,
                                          (void *)((char *)thread->stack_addr),
                                          (void *)_thread_exit);
#else
    thread->sp = (void *)rt_hw_stack_init(thread->entry, thread->parameter,
                                          (rt_uint8_t *)((char *)thread->stack_addr + thread->stack_size - sizeof(rt_ubase_t)),
                                          (void *)_thread_exit);
#endif /* ARCH_CPU_STACK_GROWS_UPWARD */

#ifdef RT_USING_MUTEX
    rt_list_init(&thread->taken_object_list);
    thread->pending_object = RT_NULL;
#endif

#ifdef RT_USING_EVENT
    thread->event_set = 0;
    thread->event_info = 0;
#endif /* RT_USING_EVENT */

    /* error 是阻塞 API 之间复用的每线程返回原因槽。 */
    thread->error = RT_EOK;

    /* SMP 全局 CPU 锁的每线程嵌套从 0 开始。 */
#ifdef RT_USING_SMP
    rt_atomic_store(&thread->cpus_lock_nest, 0);
#endif

    /* cleanup 在僵尸回收时调用；user_data 完全由用户扩展使用。 */
    thread->cleanup   = 0;
    thread->user_data = 0;

    /* 内置单次定时器统一服务延时和带超时 IPC 等待。 */
    rt_timer_init(&(thread->thread_timer),
                  thread->parent.name,
                  _thread_timeout,
                  thread,
                  0,
                  RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_THREAD_TIMER);

    /* 信号位图、返回 SP、处理向量和详细信息链初始均为空。 */
#ifdef RT_USING_SIGNALS
    thread->sig_mask    = 0x00;
    thread->sig_pending = 0x00;

#ifndef RT_USING_SMP
    thread->sig_ret     = RT_NULL;
#endif /* RT_USING_SMP */
    thread->sig_vectors = RT_NULL;
    thread->si_list     = RT_NULL;
#endif /* RT_USING_SIGNALS */

#ifdef RT_USING_SMART
    thread->tid_ref_count = 0;
    thread->lwp = RT_NULL;
    thread->susp_recycler = RT_NULL;
    thread->robust_list = RT_NULL;
    rt_list_init(&(thread->sibling));

    /* Smart/LWP 另有 POSIX 风格信号掩码与排队结构。 */
    rt_memset(&thread->signal.sigset_mask, 0, sizeof(lwp_sigset_t));
    rt_memset(&thread->signal.sig_queue.sigset_pending, 0, sizeof(lwp_sigset_t));
    rt_list_init(&thread->signal.sig_queue.siginfo_list);

    rt_memset(&thread->user_ctx, 0, sizeof thread->user_ctx);

    /* CPU 使用统计从零开始累计。 */
    thread->user_time = 0;
    thread->system_time = 0;
#endif

#ifdef RT_USING_CPU_USAGE_TRACER
    thread->user_time = 0;
    thread->system_time = 0;
    thread->total_time_prev = 0;
    thread->cpu_usage = 0;
#endif /* RT_USING_CPU_USAGE_TRACER */

#ifdef RT_USING_PTHREADS
    thread->pthread_data = RT_NULL;
#endif /* RT_USING_PTHREADS */

#ifdef RT_USING_MODULE
    thread->parent.module_id = 0;
#endif /* RT_USING_MODULE */

    rt_spin_lock_init(&thread->spinlock);

    /*
     * 初始化钩子链在所有字段和锁均可用后触发。回调运行在创建者上下文，可能持有
     * 对象系统内部状态，必须短小且不可阻塞；它可观察完整但尚未 startup 的线程。
     */
    RT_OBJECT_HOOKLIST_CALL(rt_thread_inited, (thread));

    return RT_EOK;
}

/**
 * @addtogroup group_thread_management
 * @{
 */

/**
 * @brief 使用用户提供的控制块和栈初始化一个静态线程对象。
 *
 * 该函数清零整个控制块、把它作为静态 Thread 对象注册，再由 _thread_init() 建立
 * 初始栈、调度字段、内置定时器及可选子系统字段。初始化后状态仍为 INIT，必须再
 * 调用 rt_thread_startup() 才会进入就绪队列。控制块与栈的存储期必须覆盖线程生命
 * 周期，内核不会在 detach 时释放它们。
 *
 * @param thread 用户提供的 struct rt_thread 内存。
 *
 * @param name 线程名，长度受 `RT_NAME_MAX` 限制，超出部分截断。
 *
 * @param entry 第一次运行时调用的线程入口。
 *
 * @param parameter 原样传给 @p entry 的参数。
 *
 * @param stack_start 用户提供的栈区起始地址，须满足架构对齐要求。
 *
 * @param stack_size 栈区字节数。
 *
 * @param priority 调度优先级，范围 [0, RT_THREAD_PRIORITY_MAX)，数值越小越高。
 *
 * @param tick 同优先级轮转的时间片节拍数，必须非 0。
 * @return 成功返回 `RT_EOK`。
 */
rt_err_t rt_thread_init(struct rt_thread *thread,
                        const char       *name,
                        void (*entry)(void *parameter),
                        void             *parameter,
                        void             *stack_start,
                        rt_uint32_t       stack_size,
                        rt_uint8_t        priority,
                        rt_uint32_t       tick)
{
    /* 控制块、栈和非零时间片是建立可运行现场的最低条件。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(stack_start != RT_NULL);
    RT_ASSERT(tick != 0);

    /* 清除调用者内存中的旧字段，避免残留链表/标志污染新对象。 */
    rt_memset(thread, 0x0, sizeof(struct rt_thread));

    /* 静态对象只注册，不取得控制块内存所有权。 */
    rt_object_init((rt_object_t)thread, RT_Object_Class_Thread, name);

    return _thread_init(thread,
                        name,
                        entry,
                        parameter,
                        stack_start,
                        stack_size,
                        priority,
                        tick);
}
RTM_EXPORT(rt_thread_init);

/**
 * @brief 返回当前 CPU 正在运行的线程对象。
 *
 * UP 直接读取唯一 CPU 控制块；SMP 若有硬件 current-thread 寄存器则直接读取，否则
 * 短暂关闭本地中断，防止 current_thread 在读取途中因调度改变。
 *
 * @return 当前线程；首次调度尚未建立时返回 `RT_NULL`。
 */
rt_thread_t rt_thread_self(void)
{
#ifndef RT_USING_SMP
    return rt_cpu_self()->current_thread;

#elif defined (ARCH_USING_HW_THREAD_SELF)
    return rt_hw_thread_self();

#else /* !ARCH_USING_HW_THREAD_SELF */
    rt_thread_t self;
    rt_base_t lock;

    lock = rt_hw_local_irq_disable();
    self = rt_cpu_self()->current_thread;
    rt_hw_local_irq_enable(lock);

    return self;
#endif /* ARCH_USING_HW_THREAD_SELF */
}
RTM_EXPORT(rt_thread_self);

/**
 * @brief 首次启动 INIT 线程并将其加入系统就绪队列。
 *
 * startup 先计算优先级位图属性并临时置 SUSPEND，再复用 rt_thread_resume() 完成
 * “停止超时源、入就绪队列、必要时抢占”的统一路径。线程只能 startup 一次；已经
 * 运行或关闭的线程不能用本 API 重启。
 *
 * @param thread 状态必须为 INIT 的有效线程。
 * @return 成功返回 `RT_EOK`，否则返回恢复/调度路径错误。
 */
rt_err_t rt_thread_startup(rt_thread_t thread)
{
    /* 状态断言防止同一调度节点重复入队。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT((RT_SCHED_CTX(thread).stat & RT_THREAD_STAT_MASK) == RT_THREAD_INIT);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    LOG_D("startup a thread:%s with priority:%d",
          thread->parent.name, RT_SCHED_PRIV(thread).current_priority);

    /* 生成就绪位图掩码，并准备走首次 resume。 */
    rt_sched_thread_startup(thread);

    /* 若调度器已运行且新线程优先级更高，调用中可能立即发生抢占。 */
    rt_thread_resume(thread);

    return RT_EOK;
}
RTM_EXPORT(rt_thread_startup);

/**
 * @brief 从调度系统关闭线程，但不把它加入僵尸回收队列。
 *
 * 在调度锁内，如果线程不是 INIT，则从当前调度/等待节点移除；随后分离内置定时器并
 * 把状态改为 CLOSE。它不释放互斥量、不调用 cleanup、不删除对象/栈，完整生命周期
 * 结束通常应走 detach/delete 或当前线程的 _thread_exit。
 *
 * @param thread 要关闭的线程。关闭当前线程前必须已进入禁止调度临界区。
 * @return 成功或已经关闭均返回 RT_EOK。
 */
rt_err_t rt_thread_close(rt_thread_t thread)
{
    rt_sched_lock_level_t slvl;
    rt_uint8_t thread_status;

    /* 防止当前线程在自身调度节点被移除到切换前继续被抢占。 */
    RT_ASSERT(thread != rt_thread_self() || rt_critical_level());

    /* 状态检查和队列/定时器修改必须相对于唤醒与超时原子。 */
    rt_sched_lock(&slvl);

    /* 已关闭路径保持幂等，不重复分离定时器。 */
    thread_status = rt_sched_thread_get_stat(thread);
    if (thread_status != RT_THREAD_CLOSE)
    {
        if (thread_status != RT_THREAD_INIT)
        {
            /* INIT 尚未入队，其他活动状态先摘除调度节点。 */
            rt_sched_remove_thread(thread);
        }

        /* 内置定时器对象不再参与任何超时。 */
        rt_timer_detach(&(thread->thread_timer));

        /* CLOSE 是不可再次 startup/resume 的终态。 */
        rt_sched_thread_close(thread);
    }

    /* 这里只解锁，不自行释放对象内存。 */
    rt_sched_unlock(slvl);

    return RT_EOK;
}
RTM_EXPORT(rt_thread_close);

static rt_err_t _thread_detach(rt_thread_t thread);

/**
 * @brief 结束一个由 rt_thread_init() 创建的静态线程。
 *
 * 该入口验证对象确为静态系统对象，然后关闭线程、解除互斥量关系并放入 defunct
 * 队列。后台回收会从对象系统 detach 并调用 cleanup，但不会释放用户提供的控制块
 * 和栈。
 *
 * @param thread 由 `rt_thread_init()` 初始化的线程。
 * @return 返回关闭操作状态。
 */
rt_err_t rt_thread_detach(rt_thread_t thread)
{
    /* 类型与静态对象断言防止误用动态删除语义。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);
    RT_ASSERT(rt_object_is_systemobject((rt_object_t)thread));

    return _thread_detach(thread);
}
RTM_EXPORT(rt_thread_detach);

static rt_err_t _thread_detach(rt_thread_t thread)
{
    rt_err_t error;
    rt_base_t critical_level;

    /**
     * 若目标就是当前线程，从关闭到放入僵尸队列之间不能被普通调度切走；否则会留下
     * 半结束状态。临界区退出后再由调用路径安排调度。
     */
    critical_level = rt_enter_critical();

    error = rt_thread_close(thread);

    _thread_detach_from_mutex(thread);

    /* 延迟到不使用目标栈的安全上下文执行对象分离和 cleanup。 */
    rt_thread_defunct_enqueue(thread);

    rt_exit_critical_safe(critical_level);
    return error;
}

#ifdef RT_USING_HEAP
/**
 * @brief 动态分配线程控制块和栈，并初始化为 INIT 线程。
 *
 * 先通过对象系统分配动态 Thread 对象，再从内核堆分配栈。第二步失败会回滚删除对象。
 * 成功后调用与静态线程相同的 _thread_init()，但仍需调用 rt_thread_startup() 才运行。
 * 日后 rt_thread_delete() 或入口返回后的僵尸回收会释放栈和对象。
 *
 * @param name 线程名，超过 `RT_NAME_MAX` 的部分截断。
 *
 * @param entry 线程入口。
 *
 * @param parameter 入口参数。
 *
 * @param stack_size 动态栈字节数。
 *
 * @param priority 优先级，数值越小越高。
 *
 * @param tick 同优先级时间片，必须非 0。
 * @return 成功返回新线程；对象或栈分配失败返回 `RT_NULL`。
 */
rt_thread_t rt_thread_create(const char *name,
                             void (*entry)(void *parameter),
                             void       *parameter,
                             rt_uint32_t stack_size,
                             rt_uint8_t  priority,
                             rt_uint32_t tick)
{
    /* 零时间片无法正确轮转。 */
    RT_ASSERT(tick != 0);

    struct rt_thread *thread;
    void *stack_start;

    thread = (struct rt_thread *)rt_object_allocate(RT_Object_Class_Thread,
                                                    name);
    if (thread == RT_NULL)
        return RT_NULL;

    stack_start = (void *)RT_KERNEL_MALLOC(stack_size);
    if (stack_start == RT_NULL)
    {
        /* 栈分配失败时回滚刚创建的动态对象，避免泄漏。 */
        rt_object_delete((rt_object_t)thread);

        return RT_NULL;
    }

    _thread_init(thread,
                 name,
                 entry,
                 parameter,
                 stack_start,
                 stack_size,
                 priority,
                 tick);

    return thread;
}
RTM_EXPORT(rt_thread_create);

/**
 * @brief 结束由 rt_thread_create() 创建的动态线程并安排延迟释放。
 *
 * 验证对象不是静态系统对象后，复用 _thread_detach() 完成关闭、互斥量清理和僵尸
 * 入队。后台回收最终释放动态栈和线程控制块。
 *
 * @param thread 动态线程句柄。
 * @return 返回关闭操作状态。
 */
rt_err_t rt_thread_delete(rt_thread_t thread)
{
    /* 静态线程必须使用 detach，避免错误释放用户内存。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);
    RT_ASSERT(rt_object_is_systemobject((rt_object_t)thread) == RT_FALSE);

    return _thread_detach(thread);
}
RTM_EXPORT(rt_thread_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief 当前线程主动让出本轮 CPU 使用机会。
 *
 * 在调度锁内重装时间片并设置 YIELD，随后解锁并重新调度。当前线程并非睡眠；若没有
 * 同级或更高优先级候选，它仍可能马上继续运行。
 *
 * @return 始终返回 RT_EOK。
 */
rt_err_t rt_thread_yield(void)
{
    rt_sched_lock_level_t slvl;
    rt_sched_lock(&slvl);

    rt_sched_thread_yield(rt_thread_self());

    rt_sched_unlock_n_resched(slvl);

    return RT_EOK;
}
RTM_EXPORT(rt_thread_yield);

/**
 * @brief 当前线程睡眠指定节拍数的内部实现。
 *
 * 进入可中断挂起态，重设并启动线程内置定时器，然后请求调度。先设置 error 为
 * -RT_EINTR，使信号等提前唤醒能保留中断原因；正常定时到期由 _thread_timeout 写成
 * -RT_ETIMEOUT，恢复后再转换为 RT_EOK。tick 为 0 不等同 yield，而是无效参数。
 *
 * @param tick 睡眠节拍，必须非 0。
 * @return 返回挂起操作状态。
 */
static rt_err_t _thread_sleep(rt_tick_t tick)
{
    struct rt_thread *thread;
    rt_base_t critical_level;
    int err;

    if (tick == 0)
    {
        return -RT_EINVAL;
    }

    /* 睡眠只能作用于调用者自身。 */
    thread = rt_thread_self();
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 必须已在线程上下文且调度器可用。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE);

    /* 清理上一轮阻塞留下的返回原因。 */
    thread->error = RT_EOK;

    /* 从 RUNNING 到挂起并启动超时源期间禁止调度穿插。 */
    critical_level = rt_enter_critical();

    /* 可中断睡眠允许信号提前唤醒。 */
    err = rt_thread_suspend_with_flag(thread, RT_INTERRUPTIBLE);

    /* 只有成功挂起才配置这次睡眠的超时源。 */
    if (err == RT_EOK)
    {
        rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &tick);
        rt_timer_start(&(thread->thread_timer));

        thread->error = -RT_EINTR;

        /* 当前线程已不可运行，登记切换请求。 */
        rt_schedule();

        /* 最外层退出时真正切走；日后唤醒后从此调用返回。 */
        rt_exit_critical_safe(critical_level);

        /* 定时到期是 sleep 的正常完成，不向调用者报告超时错误。 */
        if (thread->error == -RT_ETIMEOUT)
            thread->error = RT_EOK;
    }
    else
    {
        rt_exit_critical_safe(critical_level);
    }

    return err;
}

/**
 * @brief 让当前线程延时指定操作系统节拍。
 *
 * @param tick 延时 tick 数；0 返回 -RT_EINVAL。
 *
 * @return 正常到期为 RT_EOK，否则返回挂起错误。
 */
rt_err_t rt_thread_delay(rt_tick_t tick)
{
    return _thread_sleep(tick);
}
RTM_EXPORT(rt_thread_delay);

/**
 * @brief 按绝对周期基准延时到 `*tick + inc_tick`。
 *
 * 与相对 delay 不同，它用上次计划唤醒点累加周期，可避免循环体执行时间逐周期累积
 * 成漂移。若当前仍早于目标，计算 left_tick、不可中断挂起并启动定时器；若已经错过
 * 本周期，则不睡眠，把基准重置为当前 tick。无符号减法使一次自然 tick 回绕可工作。
 *
 * @param tick 输入/输出的上次周期基准。
 *
 * @param inc_tick 周期间隔节拍。
 * @return 正常到期为 RT_EOK；否则返回线程 error。
 */
rt_err_t rt_thread_delay_until(rt_tick_t *tick, rt_tick_t inc_tick)
{
    struct rt_thread *thread;
    rt_tick_t cur_tick;
    rt_base_t critical_level;

    RT_ASSERT(tick != RT_NULL);

    /* 绝对延时同样只操作当前线程。 */
    thread = rt_thread_self();
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    /* 清除前一次等待结果。 */
    thread->error = RT_EOK;

    /* 稳定“读当前 tick -> 挂起 -> 启动定时器”的决策窗口。 */
    critical_level = rt_enter_critical();

    cur_tick = rt_tick_get();
    if (cur_tick - *tick < inc_tick)
    {
        rt_tick_t left_tick;

        *tick += inc_tick;
        left_tick = *tick - cur_tick;

        /* 周期延时不可被普通信号语义中断。 */
        rt_thread_suspend_with_flag(thread, RT_UNINTERRUPTIBLE);

        /* 只等待距离绝对目标剩余的节拍。 */
        rt_timer_control(&(thread->thread_timer), RT_TIMER_CTRL_SET_TIME, &left_tick);
        rt_timer_start(&(thread->thread_timer));

        rt_exit_critical_safe(critical_level);

        rt_schedule();

        /* 到达计划唤醒点是正常成功。 */
        if (thread->error == -RT_ETIMEOUT)
        {
            thread->error = RT_EOK;
        }
    }
    else
    {
        *tick = cur_tick;
        rt_exit_critical_safe(critical_level);
    }

    return thread->error;
}
RTM_EXPORT(rt_thread_delay_until);

/**
 * @brief 以毫秒为单位让当前线程延时。
 *
 * 先用 rt_tick_from_millisecond() 按系统节拍频率向上换算，再复用 _thread_sleep()。
 * 因此实际精度受 tick 周期限制，且 0 毫秒最终作为无效零 tick 处理。
 *
 * @param ms 延时毫秒数。
 * @return 正常到期为 RT_EOK，否则返回换算后睡眠路径错误。
 */
rt_err_t rt_thread_mdelay(rt_int32_t ms)
{
    rt_tick_t tick;

    tick = rt_tick_from_millisecond(ms);

    return _thread_sleep(tick);
}
RTM_EXPORT(rt_thread_mdelay);

#ifdef RT_USING_SMP
#endif

/**
 * @brief 根据控制命令执行线程通用操作。
 *
 * CHANGE_PRIORITY 仅改变当前有效优先级，RESET_PRIORITY 同时改变基础值；STARTUP
 * 转调首次启动；CLOSE 根据静态/动态对象选择 detach/delete 并主动调度；BIND_CPU
 * 将 arg 的整数值作为 CPU 编号交给 SMP 调度器。优先级命令中的 arg 必须指向
 * rt_uint8_t，而 BIND_CPU 采用“整数经 void * 传递”的历史约定。
 *
 * @param thread 目标线程。
 *
 * @param cmd `RT_THREAD_CTRL_CHANGE_PRIORITY`、`RESET_PRIORITY`、`STARTUP`、`CLOSE`
 *            或 `BIND_CPU`。
 *
 * @param arg 命令相关参数。
 * @return 已识别命令返回对应 API 状态；未知命令当前返回 RT_EOK。
 */
rt_err_t rt_thread_control(rt_thread_t thread, int cmd, void *arg)
{
    /* 控制接口只接受仍注册为 Thread 类型的对象。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    switch (cmd)
    {
        case RT_THREAD_CTRL_CHANGE_PRIORITY:
        {
            rt_err_t error;
            rt_sched_lock_level_t slvl;
            rt_sched_lock(&slvl);
            error = rt_sched_thread_change_priority(thread, *(rt_uint8_t *)arg);
            rt_sched_unlock(slvl);
            return error;
        }

        case RT_THREAD_CTRL_RESET_PRIORITY:
        {
            rt_err_t error;
            rt_sched_lock_level_t slvl;
            rt_sched_lock(&slvl);
            error = rt_sched_thread_reset_priority(thread, *(rt_uint8_t *)arg);
            rt_sched_unlock(slvl);
            return error;
        }

        case RT_THREAD_CTRL_STARTUP:
        {
            return rt_thread_startup(thread);
        }

        case RT_THREAD_CTRL_CLOSE:
        {
            rt_err_t rt_err = -RT_EINVAL;

            if (rt_object_is_systemobject((rt_object_t)thread) == RT_TRUE)
            {
                rt_err = rt_thread_detach(thread);
            }
    #ifdef RT_USING_HEAP
            else
            {
                rt_err = rt_thread_delete(thread);
            }
    #endif /* RT_USING_HEAP */
            rt_schedule();
            return rt_err;
        }

        case RT_THREAD_CTRL_BIND_CPU:
        {
            rt_uint8_t cpu;

            cpu = (rt_uint8_t)(rt_size_t)arg;
            return rt_sched_thread_bind_cpu(thread, cpu);
        }

    default:
        break;
    }

    return RT_EOK;
}
RTM_EXPORT(rt_thread_control);

#ifdef RT_USING_SMART
#include <lwp_signal.h>
#endif

/* 将 API 的可中断性标志转换为线程 stat 中具体的挂起基本状态。 */
static rt_uint8_t _thread_get_suspend_state(int suspend_flag)
{
    switch (suspend_flag)
    {
    case RT_INTERRUPTIBLE:
        return RT_THREAD_SUSPEND_INTERRUPTIBLE;
    case RT_KILLABLE:
        return RT_THREAD_SUSPEND_KILLABLE;
    case RT_UNINTERRUPTIBLE:
    default:
        return RT_THREAD_SUSPEND_UNINTERRUPTIBLE;
    }
}

static void _thread_set_suspend_state(struct rt_thread *thread, int suspend_flag)
{
    rt_uint8_t stat;

    RT_ASSERT(thread != RT_NULL);
    stat = _thread_get_suspend_state(suspend_flag);
    RT_SCHED_CTX(thread).stat = stat | (RT_SCHED_CTX(thread).stat & ~RT_THREAD_STAT_MASK);
}

/**
 * @brief 把线程挂入指定等待链表并设置相应挂起状态。
 *
 * 这是 IPC 与延时路径的核心状态转换。调度锁内先处理“已经挂起”的幂等/升级情况；
 * 对 READY/RUNNING 线程做 Smart 信号检查，再从调度集合移除、设置可中断/可杀死/
 * 不可中断状态，并在同一临界区加入等待链，以免错过异步通知。最后停止旧线程超时
 * 定时器，解锁后调用 suspend hook。
 *
 * RUNNING 线程只能挂起自己；强制挂起其他正在运行的线程无法知道其是否持锁，会
 * 导致死锁或资源饥饿。READY 线程虽可被操作，也需要上层完整掌握生命周期。
 *
 * @param thread 要挂起的线程。
 * @param susp_list IPC/等待对象的挂起链表；RT_NULL 表示只改变线程状态。
 * @param ipc_flags `RT_IPC_FLAG_PRIO` 按优先级排队，`RT_IPC_FLAG_FIFO` 按到达顺序。
 *                  FIFO 不保证高优先级等待者先获得资源，应理解其实时性影响。
 * @param suspend_flag `RT_INTERRUPTIBLE`、`RT_KILLABLE` 或 `RT_UNINTERRUPTIBLE`。
 *
 * @return 成功为 RT_EOK；状态非法为 -RT_ERROR；Smart 信号阻止挂起为 -RT_EINTR。
 */
rt_err_t rt_thread_suspend_to_list(rt_thread_t thread, rt_list_t *susp_list, int ipc_flags, int suspend_flag)
{
    rt_base_t stat;
    rt_sched_lock_level_t slvl;

    /* 空闲线程是每 CPU 最后兜底，绝不能被挂起。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);
    RT_ASSERT(!rt_thread_is_idle_thread(thread));

    LOG_D("thread suspend: %s", thread->parent.name);

    rt_sched_lock(&slvl);

    stat = rt_sched_thread_get_stat(thread);
    if (stat & RT_THREAD_SUSPEND_MASK)
    {
        if (RT_SCHED_CTX(thread).sched_flag_ttmr_set == 1)
        {
            /* 新的挂起请求终止旧等待关联的线程超时定时器。 */
            LOG_D("Thread [%s]'s timer has been halted.\n", thread->parent.name);
            rt_sched_thread_timer_stop(thread);
        }
        /* 只允许把已有挂起升级到更严格状态，不反向放宽。 */
        if (stat < _thread_get_suspend_state(suspend_flag))
        {
            _thread_set_suspend_state(thread, suspend_flag);
        }
        rt_sched_unlock(slvl);
        /* 已挂起无需重复摘链/入链。 */
        return RT_EOK;
    }
    else if ((stat != RT_THREAD_READY) && (stat != RT_THREAD_RUNNING))
    {
        LOG_W("thread suspend: thread disorder, 0x%02x", RT_SCHED_CTX(thread).stat);
        rt_sched_unlock(slvl);
        return -RT_ERROR;
    }

    if (stat == RT_THREAD_RUNNING)
    {
        /* 不能从本核安全移除另一 CPU 正在使用的运行栈。 */
        RT_ASSERT(thread == rt_thread_self());
    }

#ifdef RT_USING_SMART
    if (thread->lwp)
    {
        rt_sched_unlock(slvl);

        /* Smart 信号可能要求此次可中断挂起立即失败。检查时不能持调度锁。 */
        if (lwp_thread_signal_suspend_check(thread, suspend_flag) == 0)
        {
            /* pending 信号赢得竞争，不进入挂起态。 */
            return -RT_EINTR;
        }

        rt_sched_lock(&slvl);
        if (stat == RT_THREAD_READY)
        {
            stat = rt_sched_thread_get_stat(thread);

            if (stat != RT_THREAD_READY)
            {
                /* 放锁检查信号期间状态被竞争者改变，放弃本次转换。 */
                rt_sched_unlock(slvl);
                return -RT_ERROR;
            }
        }
    }
#endif

    /* 先从 RUNNING/READY 调度集合摘除，再赋挂起基本状态。 */
    rt_sched_remove_thread(thread);
    _thread_set_suspend_state(thread, suspend_flag);

    if (susp_list)
    {
        /**
         * 离开调度临界区前就加入等待链；否则资源在两步之间到达会找不到等待者，
         * 造成永久睡眠的“丢失唤醒”。
         */
        rt_susp_list_enqueue(susp_list, thread, ipc_flags);
    }

    /* 清除线程此前任何等待留下的超时源，新的等待会由调用者重新设置。 */
    rt_sched_thread_timer_stop(thread);

    rt_sched_unlock(slvl);

    RT_OBJECT_HOOK_CALL(rt_thread_suspend_hook, (thread));
    return RT_EOK;
}
RTM_EXPORT(rt_thread_suspend_to_list);

/**
 * @brief 不加入外部等待链，仅按指定类型挂起线程。
 *
 * 这是 rt_thread_suspend_to_list(thread, RT_NULL, ...) 的便捷包装。通常仅让当前线程
 * 自愿挂起；对其他线程使用仍有持锁与资源饥饿风险。
 *
 * @param thread 要挂起的线程。
 * @param suspend_flag 挂起可中断性类型。
 *
 * @return 转发核心挂起函数的状态。
 */
rt_err_t rt_thread_suspend_with_flag(rt_thread_t thread, int suspend_flag)
{
    return rt_thread_suspend_to_list(thread, RT_NULL, 0, suspend_flag);
}
RTM_EXPORT(rt_thread_suspend_with_flag);

/**
 * @brief 以不可中断状态强制挂起指定线程。
 *
 * 挂起自己通常安全；挂起他人时无法知道它正执行到哪一步或是否持有互斥量、信号量
 * 等资源。除非调用者对系统状态有完整控制，否则不应将它当作线程同步机制。
 *
 * @warning 任意挂起其他线程可能造成死锁、资源饥饿、系统不稳定和不可预测行为。
 *
 * @param thread 当前线程或受控的其他线程。
 *
 * @return 转发不可中断挂起操作状态。
 */
rt_err_t rt_thread_suspend(rt_thread_t thread)
{
    return rt_thread_suspend_with_flag(thread, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_thread_suspend);

/**
 * @brief 把挂起线程恢复到系统就绪队列，并在需要时触发抢占。
 *
 * 调度锁内由 rt_sched_thread_ready() 与超时 ISR 竞争：成功停止线程定时器后，从等待
 * 链摘除并入就绪队列。解锁调度返回 -RT_ESCHEDLOCKED 仅表示当前调用者仍处于外层
 * 临界区、切换被延后，并不表示目标恢复失败，因此转换为 RT_EOK。最后无条件调用
 * resume hook，观察者应结合返回值判断结果。
 *
 * @param thread 要恢复的挂起线程。
 * @return 成功为 RT_EOK；非挂起或输掉超时竞争时返回相应错误。
 */
rt_err_t rt_thread_resume(rt_thread_t thread)
{
    rt_sched_lock_level_t slvl;
    rt_err_t error;

    /* 目标必须仍是有效线程对象。 */
    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    LOG_D("thread resume: %s", thread->parent.name);

    rt_sched_lock(&slvl);

    error = rt_sched_thread_ready(thread);

    if (!error)
    {
        error = rt_sched_unlock_n_resched(slvl);

        /**
         * ESCHEDLOCKED 只说明调用线程仍锁住调度，目标已经成功 READY；实际切换将在
         * 外层临界区退出后发生，因此对 resume 调用者按成功处理。
         */
        if (error == -RT_ESCHEDLOCKED)
        {
            error = RT_EOK;
        }
    }
    else
    {
        rt_sched_unlock(slvl);
    }

    RT_OBJECT_HOOK_CALL(rt_thread_resume_hook, (thread));

    return error;
}
RTM_EXPORT(rt_thread_resume);

#ifdef RT_USING_SMART
/**
 * @brief 使用可选的自定义唤醒处理恢复 Smart 线程。
 *
 * 在调度锁内读取并清空一次性 wakeup_handle.func，随后锁外调用它；清空可避免回调
 * 重入时被重复调用。不曾设置自定义回调则退回普通 rt_thread_resume()。
 *
 * @param thread 要唤醒的线程。
 * @return 自定义回调或 rt_thread_resume() 的状态。
 */
rt_err_t rt_thread_wakeup(rt_thread_t thread)
{
    rt_sched_lock_level_t slvl;
    rt_err_t ret;
    rt_wakeup_func_t func = RT_NULL;

    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    rt_sched_lock(&slvl);
    func = thread->wakeup_handle.func;
    thread->wakeup_handle.func = RT_NULL;
    rt_sched_unlock(slvl);

    if (func)
    {
        ret = func(thread->wakeup_handle.user_data, thread);
    }
    else
    {
        ret = rt_thread_resume(thread);
    }
    return ret;
}
RTM_EXPORT(rt_thread_wakeup);

/**
 * @brief 为 Smart 线程登记一次性自定义唤醒函数及用户数据。
 *
 * 字段在调度锁内成对更新，下一次 rt_thread_wakeup() 会原子取走并清空 func，然后
 * 在锁外调用。传入 RT_NULL 可恢复为普通 resume 行为；user_data 由回调解释。
 *
 * @param thread 目标线程。
 * @param func 自定义唤醒回调。
 * @param user_data 原样传给回调的上下文。
 */
void rt_thread_wakeup_set(struct rt_thread *thread, rt_wakeup_func_t func, void* user_data)
{
    rt_sched_lock_level_t slvl;

    RT_ASSERT(thread != RT_NULL);
    RT_ASSERT(rt_object_get_type((rt_object_t)thread) == RT_Object_Class_Thread);

    rt_sched_lock(&slvl);
    thread->wakeup_handle.func = func;
    thread->wakeup_handle.user_data = user_data;
    rt_sched_unlock(slvl);
}
RTM_EXPORT(rt_thread_wakeup_set);
#endif
/**
 * @brief 按对象名查找线程。
 *
 * @note 对象遍历与名称比较不适合中断上下文；返回裸指针后调用者还应保证线程生命期。
 *
 * @param name 要查找的线程名。
 *
 * @return 找到的线程，未找到返回 RT_NULL。
 */
rt_thread_t rt_thread_find(char *name)
{
    return (rt_thread_t)rt_object_find(name, RT_Object_Class_Thread);
}

RTM_EXPORT(rt_thread_find);

/**
 * @brief 把指定线程的对象名复制到调用者缓冲区。
 *
 * @note 不应在中断上下文调用。
 *
 * @param thread 目标线程。
 * @param name 接收以空字符结尾名称的缓冲区。
 * @param name_size 缓冲区最大字节数。
 *
 * @return 成功为 RT_EOK；thread 为 RT_NULL 时为 -RT_EINVAL；其他错误由对象 API 返回。
 */
rt_err_t rt_thread_get_name(rt_thread_t thread, char *name, rt_uint8_t name_size)
{
    return (thread == RT_NULL) ? -RT_EINVAL : rt_object_get_name(&thread->parent, name, name_size);
}
RTM_EXPORT(rt_thread_get_name);

/** @} group_thread_management */
