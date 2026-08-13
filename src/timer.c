/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2006-03-12     Bernard      初始版本
 * 2006-04-29     Bernard      实现线程定时器
 * 2006-06-04     Bernard      实现 rt_timer_control
 * 2006-08-10     Bernard      修复周期定时器问题
 * 2006-09-03     Bernard      实现 rt_timer_detach
 * 2009-11-11     LiJin        增加软定时器
 * 2010-05-12     Bernard      修复定时器检查问题
 * 2010-11-02     Charlie      重新实现 tick 溢出处理
 * 2012-12-15     Bernard      修复软定时器的下一超时点问题
 * 2014-07-12     Bernard      调用软定时器超时函数时不锁定调度器
 * 2021-08-15     supperthomas 增加注释
 * 2022-01-07     Gabriel      将 __on_rt_xxxxx_hook 移入 timer.c
 * 2022-04-19     Stanley      修正说明
 * 2023-09-15     xqyjlj       优化 rt_hw_interrupt_disable/enable
 * 2024-01-25     Shell        增加 RT_TIMER_FLAG_THREAD_TIMER 以和调度器同步
 * 2024-05-01     wdfk-prog    合并 rt_timer_check 和 _soft_timer_check
 */

#include <rtthread.h>
#include <rthw.h>

/**
 * @file timer.c
 * @brief RT-Thread 内核定时器的创建、排序、启动、停止和超时分发实现。
 *
 * 初学者可以把本文件理解为两部分：
 *
 * 1. “闹钟登记簿”：每个已经启动的 `rt_timer` 按绝对到期 tick
 *    (`timeout_tick`) 插入跳表。跳表的最后一层包含全部定时器并保持有序，
 *    其余层是稀疏索引，用来减少寻找插入位置时需要遍历的节点数。
 * 2. “到点执行器”：系统 tick 中断调用 `rt_timer_check()`。硬定时器直接在
 *    中断上下文执行回调；软定时器只由中断唤醒专用定时器线程，随后由该线程
 *    执行回调。因而硬定时器回调必须短小、不可阻塞，软定时器回调可以使用
 *    线程上下文允许的服务，但长时间运行仍会推迟同一线程中的其他软定时器。
 *
 * 定时器对象有两种互不混用的生命周期：`rt_timer_init()` 初始化调用者提供的
 * 静态存储，最终用 `rt_timer_detach()` 脱离对象系统；`rt_timer_create()` 从堆
 * 分配动态对象，最终用 `rt_timer_delete()` 删除。启动和停止只改变“是否处于
 * 定时队列中”，并不结束对象本身的生命周期。
 *
 * 并发方面，硬定时器表和软定时器表各由独立自旋锁保护。回调函数执行前会
 * 暂时释放该锁，所以回调可以重新启动、停止或删除当前定时器；回调返回后，
 * `_timer_check()` 会借助临时链表标记判断对象是否已被回调改变，避免再次访问
 * 或错误地重启它。启用 SMP 时，公共 tick 检查只由 CPU 0 执行。
 */

#define DBG_TAG           "kernel.timer"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

#ifndef RT_USING_TIMER_ALL_SOFT
/* 硬定时器跳表及其锁；硬定时器回调由 tick 中断路径直接执行。 */
static rt_list_t _timer_list[RT_TIMER_SKIP_LIST_LEVEL];
static struct rt_spinlock _htimer_lock;
#endif

#ifdef RT_USING_TIMER_SOFT

#ifndef RT_TIMER_THREAD_STACK_SIZE
#define RT_TIMER_THREAD_STACK_SIZE     512
#endif /* RT_TIMER_THREAD_STACK_SIZE */

#ifndef RT_TIMER_THREAD_PRIO
#define RT_TIMER_THREAD_PRIO           0
#endif /* RT_TIMER_THREAD_PRIO */

/*
 * 软定时器跳表及其锁。`_soft_timer_sem` 是中断与定时器线程之间的通知门铃：
 * tick 中断只在最早软定时器已经到期时释放它，真正的回调由 `_timer_thread`
 * 执行。信号量上限随后被设为 1，避免重复 tick 累积大量无意义通知。
 */
static rt_list_t _soft_timer_list[RT_TIMER_SKIP_LIST_LEVEL];
static struct rt_spinlock _stimer_lock;
static struct rt_thread _timer_thread;
static struct rt_semaphore _soft_timer_sem;
rt_align(RT_ALIGN_SIZE)
static rt_uint8_t _timer_thread_stack[RT_TIMER_THREAD_STACK_SIZE];
#endif /* RT_USING_TIMER_SOFT */

#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
extern void (*rt_object_take_hook)(struct rt_object *object);
extern void (*rt_object_put_hook)(struct rt_object *object);
static void (*rt_timer_enter_hook)(struct rt_timer *timer);
static void (*rt_timer_exit_hook)(struct rt_timer *timer);

/**
 * @addtogroup group_hook
 */

/**@{*/

/**
 * @brief 设置“即将进入定时器回调”钩子。
 *
 * 钩子在到期定时器仍受相应定时器表自旋锁保护时调用，随后定时器才从表中
 * 移除并释放锁。硬定时器对应中断上下文，软定时器对应定时器线程上下文；
 * 钩子必须遵守所在上下文限制，并且不得递归操作会获取同一把定时器锁的 API。
 * 传入 `RT_NULL` 可清除钩子。
 *
 * @param hook 接收当前到期 `rt_timer` 的函数指针。
 */
void rt_timer_enter_sethook(void (*hook)(struct rt_timer *timer))
{
    rt_timer_enter_hook = hook;
}

/**
 * @brief 设置“定时器回调刚刚返回”钩子。
 *
 * 该钩子在用户回调返回后、重新取得定时器表锁之前调用。因此调用时没有持有
 * 定时器表锁；上下文仍与回调相同（硬定时器为中断上下文，软定时器为线程
 * 上下文）。此时回调可能已经停止、重启、脱离甚至删除了原定时器，钩子只能
 * 按调用契约谨慎观察所收到的指针。传入 `RT_NULL` 可清除钩子。
 *
 * @param hook 接收刚执行完回调的 `rt_timer` 的函数指针。
 */
void rt_timer_exit_sethook(void (*hook)(struct rt_timer *timer))
{
    rt_timer_exit_hook = hook;
}

/**@}*/
#endif /* RT_USING_HOOK */

/**
 * @brief 根据定时器类型选择保护它所在队列的自旋锁。
 *
 * `RT_USING_TIMER_ALL_SOFT` 会强制所有定时器进入软定时器表；否则设置了
 * `RT_TIMER_FLAG_SOFT_TIMER` 的对象使用 `_stimer_lock`，其余对象使用
 * `_htimer_lock`。调用者仍需自行加锁，本函数只返回锁地址。
 *
 * @param timer 要查询的定时器。
 * @return 对应硬/软定时器表的自旋锁指针。
 */
rt_inline struct rt_spinlock* _timerlock_idx(struct rt_timer *timer)
{
#ifdef RT_USING_TIMER_ALL_SOFT
    return &_stimer_lock;
#else
#ifdef RT_USING_TIMER_SOFT
    if (timer->parent.flag & RT_TIMER_FLAG_SOFT_TIMER)
    {
        return &_stimer_lock;
    }
    else
#endif /* RT_USING_TIMER_SOFT */
    {
        return &_htimer_lock;
    }
#endif
}

/**
 * @brief 初始化定时器对象中除通用对象头以外的字段（内部函数）。
 *
 * 本函数由静态初始化和动态创建路径共同调用。它只建立初始状态，不把定时器
 * 插入队列，所以调用返回后定时器仍未启动。开启 `RT_USING_TIMER_ALL_SOFT`
 * 时会强制加上软定时器标志。每一层 `row[]` 都初始化为空，方便启动、停止和
 * 回调中的重启操作统一调用 `_timer_remove()`。
 *
 * @see rt_timer_init
 *
 * @param timer 已经拥有合法通用对象头的定时器对象。
 *
 * @param timeout 到期回调函数。
 *
 * @param parameter 原样传给 `timeout` 的用户参数。
 *
 * @param time 从启动到到期的相对 tick 数，保存到 `init_tick`。
 *
 * @param flag 单次/周期、硬/软以及线程内置定时器等标志组合。
 */
static void _timer_init(rt_timer_t timer,
                        void (*timeout)(void *parameter),
                        void      *parameter,
                        rt_tick_t  time,
                        rt_uint8_t flag)
{
    int i;

#ifdef RT_USING_TIMER_ALL_SOFT
    flag               |= RT_TIMER_FLAG_SOFT_TIMER;
#endif

    /* 保存配置标志，并明确清除“已启动”这一运行时状态位。 */
    timer->parent.flag  = flag;

    /* 初始化并不等于启动；此时对象尚未进入任何定时器表。 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    timer->timeout_func = timeout;
    timer->parameter    = parameter;

    timer->timeout_tick = 0;
    timer->init_tick    = time;

    /* 一个定时器在跳表的每一层都有一个独立链表节点。 */
    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        rt_list_init(&(timer->row[i]));
    }
}

/**
 * @brief 读取指定跳表中最早一个定时器的绝对到期 tick。
 *
 * 跳表最后一层包含全部节点且按到期时间排序，因此取该层头节点即可，不需要
 * 遍历。调用者必须通过对应的定时器表锁保证链表在读取期间不被修改。
 *
 * @param timer_list 硬定时器或软定时器的跳表头数组。
 * @param timeout_tick 成功时写入最早定时器的绝对 `timeout_tick`。
 *
 * @return 表非空返回 `RT_EOK`；表为空返回 `-RT_ERROR`，输出值不更新。
 */
static rt_err_t _timer_list_next_timeout(rt_list_t timer_list[], rt_tick_t *timeout_tick)
{
    struct rt_timer *timer;

    if (!rt_list_isempty(&timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1]))
    {
        timer = rt_list_entry(timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1].next,
                              struct rt_timer, row[RT_TIMER_SKIP_LIST_LEVEL - 1]);
        *timeout_tick = timer->timeout_tick;
        return RT_EOK;
    }
    return -RT_ERROR;
}

/**
 * @brief 从跳表的所有层移除定时器（内部函数）。
 *
 * `rt_list_remove()` 会把节点恢复为自环，因此即使某一层从未插入也可调用。
 * 调用者必须持有对应定时器表的锁；本函数不修改 ACTIVATED 状态位。
 *
 * @param timer 要移除的定时器。
 */
rt_inline void _timer_remove(rt_timer_t timer)
{
    int i;

    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        rt_list_remove(&timer->row[i]);
    }
}

#if (DBG_LVL == DBG_LOG)
/**
 * @brief 统计调试输出中一个定时器实际占用的跳表层数。
 *
 * @param timer 要统计的定时器。
 *
 * @return 非空 `row[]` 节点的数量。
 */
static int _timer_count_height(struct rt_timer *timer)
{
    int i, cnt = 0;

    for (i = 0; i < RT_TIMER_SKIP_LIST_LEVEL; i++)
    {
        if (!rt_list_isempty(&timer->row[i]))
            cnt++;
    }
    return cnt;
}
/**
 * @brief 按最底层顺序输出所有定时器的跳表高度，供调试跳表分布。
 *
 * @param timer_heads 要查看的跳表头数组；调用者负责并发保护。
 */
void rt_timer_dump(rt_list_t timer_heads[])
{
    rt_list_t *list;

    for (list = timer_heads[RT_TIMER_SKIP_LIST_LEVEL - 1].next;
         list != &timer_heads[RT_TIMER_SKIP_LIST_LEVEL - 1];
         list = list->next)
    {
        struct rt_timer *timer = rt_list_entry(list,
                                               struct rt_timer,
                                               row[RT_TIMER_SKIP_LIST_LEVEL - 1]);
        rt_kprintf("%d", _timer_count_height(timer));
    }
    rt_kprintf("\n");
}
#endif /* (DBG_LVL == DBG_LOG) */

/**
 * @addtogroup group_clock_management
 */

/**@{*/

/**
 * @brief 初始化一个由调用者提供存储空间的静态定时器对象。
 *
 * 该函数先把对象注册到内核对象系统，再初始化定时器私有字段，但不会启动它。
 * 使用结束后必须调用 `rt_timer_detach()`，不可调用 `rt_timer_delete()`。
 *
 * @param timer 指向调用者长期保存的 `struct rt_timer`。
 *
 * @param name 对象名称，按对象系统规则复制或保存。
 *
 * @param timeout 非空到期回调。硬定时器回调在中断上下文运行，不能阻塞；
 *                软定时器回调在系统定时器线程运行。
 *
 * @param parameter 回调时原样传入的用户参数。
 *
 * @param time 相对超时 tick，必须小于 `RT_TICK_MAX / 2`。这个半周期限制让
 *             无符号 tick 回绕前后的时间先后关系仍可用差值安全判断。
 *
 * @param flag 单次或周期、硬或软、线程内置定时器等标志组合。
 *
 */
void rt_timer_init(rt_timer_t  timer,
                   const char *name,
                   void (*timeout)(void *parameter),
                   void       *parameter,
                   rt_tick_t   time,
                   rt_uint8_t  flag)
{
    /* 参数断言也保护后续基于“半个 tick 周期”的比较算法。 */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(timeout != RT_NULL);
    RT_ASSERT(time < RT_TICK_MAX / 2);

    /* `rt_object_init()` 将其标记为静态对象并登记到 Timer 对象链。 */
    rt_object_init(&(timer->parent), RT_Object_Class_Timer, name);

    _timer_init(timer, timeout, parameter, time, flag);
}
RTM_EXPORT(rt_timer_init);

/**
 * @brief 停止并注销一个静态定时器，但不释放其存储空间。
 *
 * 函数先选择并锁住对应的硬/软定时器表，从所有跳表层移除对象并清除
 * ACTIVATED；释放队列锁之后再从对象系统注销。调用者必须保证没有其他执行流
 * 继续使用该对象，并且对象确实来自 `rt_timer_init()`。
 *
 * @param timer 要脱离的静态定时器。
 * @return 固定返回 `RT_EOK`；无效对象由断言报告。
 */
rt_err_t rt_timer_detach(rt_timer_t timer)
{
    rt_base_t level;
    struct rt_spinlock *spinlock;

    /* 静态属性断言用于防止把动态对象交给 detach 而造成内存泄漏。 */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);
    RT_ASSERT(rt_object_is_systemobject(&timer->parent));

    spinlock = _timerlock_idx(timer);
    level = rt_spin_lock_irqsave(spinlock);

    _timer_remove(timer);
    /* 从队列移除和清除运行状态在同一临界区内完成。 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    rt_spin_unlock_irqrestore(spinlock, level);
    rt_object_detach(&(timer->parent));

    return RT_EOK;
}
RTM_EXPORT(rt_timer_detach);

#ifdef RT_USING_HEAP
/**
 * @brief 从内核堆分配并注册一个动态定时器。
 *
 * 创建成功后对象仍处于停止状态，需要显式调用 `rt_timer_start()`。使用结束时
 * 必须调用 `rt_timer_delete()`，不可调用静态对象的 `rt_timer_detach()`。
 *
 * @param name 定时器对象名称。
 *
 * @param timeout 非空到期回调；其上下文由硬/软标志决定。
 *
 * @param parameter 原样传给回调的参数。
 *
 * @param time 相对超时 tick，必须小于 `RT_TICK_MAX / 2`。
 *
 * @param flag 定时器行为标志，可按位组合：
 *
 *          `RT_TIMER_FLAG_ONE_SHOT`：到期一次后停止；
 *          `RT_TIMER_FLAG_PERIODIC`：回调未主动改变定时器时自动重新启动；
 *
 *          `RT_TIMER_FLAG_HARD_TIMER`：在 tick 中断路径执行；
 *          `RT_TIMER_FLAG_SOFT_TIMER`：在系统定时器线程执行；
 *          `RT_TIMER_FLAG_THREAD_TIMER`：该对象嵌在 `rt_thread` 中，用于线程等待。
 *
 *          可以使用按位或 `|` 组合互不冲突的标志。若启用
 *          `RT_USING_TIMER_ALL_SOFT`，内部会忽略硬定时器选择并强制使用软定时器。
 *
 * @return 成功返回动态定时器；堆分配失败返回 `RT_NULL`。
 */
rt_timer_t rt_timer_create(const char *name,
                           void (*timeout)(void *parameter),
                           void       *parameter,
                           rt_tick_t   time,
                           rt_uint8_t  flag)
{
    struct rt_timer *timer;

    /* 回调和半周期范围由断言保证；堆耗尽则通过返回值报告。 */
    RT_ASSERT(timeout != RT_NULL);
    RT_ASSERT(time < RT_TICK_MAX / 2);

    /* 对象分配器按 Timer 类登记的对象大小分配并完成通用对象注册。 */
    timer = (struct rt_timer *)rt_object_allocate(RT_Object_Class_Timer, name);
    if (timer == RT_NULL)
    {
        return RT_NULL;
    }

    _timer_init(timer, timeout, parameter, time, flag);

    return timer;
}
RTM_EXPORT(rt_timer_create);

/**
 * @brief 停止、注销并释放一个动态定时器。
 *
 * 队列移除发生在对应定时器锁内；对象注销和堆释放发生在解锁后。调用者必须
 * 保证对象来自 `rt_timer_create()`，且没有其他线程、回调或钩子继续引用它。
 *
 * @param timer 要删除的动态定时器。
 * @return 固定返回 `RT_EOK`；类型或生命周期错误由断言报告。
 */
rt_err_t rt_timer_delete(rt_timer_t timer)
{
    rt_base_t level;
    struct rt_spinlock *spinlock;

    /* 动态属性断言防止错误释放调用者提供的静态存储。 */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);
    RT_ASSERT(rt_object_is_systemobject(&timer->parent) == RT_FALSE);

    spinlock = _timerlock_idx(timer);

    level = rt_spin_lock_irqsave(spinlock);

    _timer_remove(timer);
    /* 删除时无论是否已启动，都将节点恢复为空并清除运行状态。 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
    rt_spin_unlock_irqrestore(spinlock, level);
    rt_object_delete(&(timer->parent));

    return RT_EOK;
}
RTM_EXPORT(rt_timer_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief 把定时器按到期时间插入指定跳表（已持锁的内部实现）。
 *
 * 调用步骤如下：
 *
 * 1. 先从全部层移除旧节点，因此“再次 start”具有重启语义；
 * 2. 清除 ACTIVATED 并调用 `rt_object_take_hook`；
 * 3. 用当前 tick 加 `init_tick` 计算绝对到期 tick；
 * 4. 在各层寻找稳定的插入位置，同一到期 tick 的新对象排在旧对象之后；
 * 5. 用单调计数器的低位决定节点高度，最后设置 ACTIVATED。
 *
 * 调用者必须持有该 `timer_list` 对应的自旋锁。对象 take 钩子也因此在持有
 * 定时器表锁且本地中断关闭的状态下调用，钩子不可阻塞或重入定时器操作。
 *
 * @param timer_list 目标硬/软定时器跳表。
 * @param timer 要启动或重新计时的定时器。
 * @return 当前实现固定返回 `RT_EOK`。
 */
static rt_err_t _timer_start(rt_list_t *timer_list, rt_timer_t timer)
{
    unsigned int row_lvl;
    rt_list_t *row_head[RT_TIMER_SKIP_LIST_LEVEL];
    unsigned int tst_nr;
    static unsigned int random_nr;

    /* start 可用于已启动对象：先取消旧到期位置，再按当前 tick 重新计算。 */
    _timer_remove(timer);
    /* 在队列重建期间暂时呈现为未激活。 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(timer->parent)));

    timer->timeout_tick = rt_tick_get() + timer->init_tick;

    row_head[0]  = &timer_list[0];
    for (row_lvl = 0; row_lvl < RT_TIMER_SKIP_LIST_LEVEL; row_lvl++)
    {
        for (; row_head[row_lvl] != timer_list[row_lvl].prev;
             row_head[row_lvl]  = row_head[row_lvl]->next)
        {
            struct rt_timer *t;
            rt_list_t *p = row_head[row_lvl]->next;

            /* 从本层链表节点还原其所属定时器。 */
            t = rt_list_entry(p, struct rt_timer, row[row_lvl]);

            /*
             * 到期 tick 相同就继续向后走，使先插入的定时器先回调，保持稳定顺序。
             * 第二个分支用无符号差值与“半周期”比较；这在 tick 回绕时仍能区分
             * 哪个时刻更早，也是 init_tick 被限制小于半周期的原因。
             */
            if ((t->timeout_tick - timer->timeout_tick) == 0)
            {
                continue;
            }
            else if ((t->timeout_tick - timer->timeout_tick) < RT_TICK_MAX / 2)
            {
                break;
            }
        }
        if (row_lvl != RT_TIMER_SKIP_LIST_LEVEL - 1)
            row_head[row_lvl + 1] = row_head[row_lvl] + 1;
    }

    /*
     * 使用递增计数器而不是到期 tick 决定跳表高度。到期 tick 往往有明显规律，
     * 不适合作为随机源；计数器配合掩码能以很小代价让各高度大致均匀分布。
     */
    random_nr++;
    tst_nr = random_nr;

    rt_list_insert_after(row_head[RT_TIMER_SKIP_LIST_LEVEL - 1],
                         &(timer->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
    for (row_lvl = 2; row_lvl <= RT_TIMER_SKIP_LIST_LEVEL; row_lvl++)
    {
        if (!(tst_nr & RT_TIMER_SKIP_LIST_MASK))
            rt_list_insert_after(row_head[RT_TIMER_SKIP_LIST_LEVEL - row_lvl],
                                 &(timer->row[RT_TIMER_SKIP_LIST_LEVEL - row_lvl]));
        else
            break;
        /* 丢弃已经用于本层判定的位，下一轮用新的位组决定是否继续升高。 */
        tst_nr >>= (RT_TIMER_SKIP_LIST_MASK + 1) >> 1;
    }

    timer->parent.flag |= RT_TIMER_FLAG_ACTIVATED;

    return RT_EOK;
}

/**
 * @brief 扫描一个定时器表，并执行所有已经到期的回调（内部核心函数）。
 *
 * 函数先加锁并始终查看有序底层的第一个节点。若当前 tick 已达到该节点的
 * `timeout_tick`，处理流程为：
 *
 * 1. 在持锁状态调用 enter hook；
 * 2. 从正式跳表移除定时器，单次定时器同时清除 ACTIVATED；
 * 3. 把其底层节点暂挂到局部 `list`，作为“回调尚未修改该对象”的标记；
 * 4. 解锁后执行用户回调和 exit hook；
 * 5. 重新加锁。如果局部标记已被回调中的 start/stop/detach/delete 移除，说明
 *    回调已经接管该对象，立即继续而不再访问；否则取下标记，并在周期定时器
 *    仍为 ACTIVATED 时重新入队。
 *
 * 这种“回调外解锁 + 临时标记”设计既避免用户代码占用自旋锁，又允许回调
 * 安全地控制自身定时器。硬表由中断路径调用，软表由定时器线程调用，因此
 * 用户回调与两个 hook 的上下文取决于传入的是哪张表。
 *
 * @param timer_list 要检查的硬/软定时器跳表。
 * @param lock 保护该表的自旋锁；函数自行加锁，回调期间暂时解锁。
 */
static void _timer_check(rt_list_t *timer_list, struct rt_spinlock *lock)
{
    struct rt_timer *t;
    rt_tick_t current_tick;
    rt_base_t level;
    rt_list_t list;

    level = rt_spin_lock_irqsave(lock);

    current_tick = rt_tick_get();

    rt_list_init(&list);

    while (!rt_list_isempty(&timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1]))
    {
        t = rt_list_entry(timer_list[RT_TIMER_SKIP_LIST_LEVEL - 1].next,
                          struct rt_timer, row[RT_TIMER_SKIP_LIST_LEVEL - 1]);

        /* 每处理一个回调都重新取 tick，因为上一个回调可能消耗了较长时间。 */
        current_tick = rt_tick_get();

        /*
         * 差值小于半周期表示当前时刻已经到达或越过 timeout_tick；该写法可以
         * 正确跨越无符号 tick 的回绕点，前提是定时间隔小于半个计数周期。
         */
        if ((current_tick - t->timeout_tick) < RT_TICK_MAX / 2)
        {
            RT_OBJECT_HOOK_CALL(rt_timer_enter_hook, (t));

            /* 先从正式跳表移除，防止并发检查再次发现同一对象。 */
            _timer_remove(t);
            if (!(t->parent.flag & RT_TIMER_FLAG_PERIODIC))
            {
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
            }

            /* 临时节点是回调期间探测对象是否被重新操作的“所有权标记”。 */
            rt_list_insert_after(&list, &(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));

            rt_spin_unlock_irqrestore(lock, level);

            /* 用户代码在不持有定时器表锁的状态下运行。 */
            t->timeout_func(t->parameter);

            RT_OBJECT_HOOK_CALL(rt_timer_exit_hook, (t));

            level = rt_spin_lock_irqsave(lock);

            /* 空标记说明回调已通过控制 API 改变了该定时器，不再自动处理。 */
            if (rt_list_isempty(&list))
            {
                continue;
            }
            rt_list_remove(&(t->row[RT_TIMER_SKIP_LIST_LEVEL - 1]));
            if ((t->parent.flag & RT_TIMER_FLAG_PERIODIC) &&
                (t->parent.flag & RT_TIMER_FLAG_ACTIVATED))
            {
                /* 周期对象保持 ACTIVATED，按当前 tick 重新计算下一次到期时间。 */
                t->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
                _timer_start(timer_list, t);
            }
        }
        else break;
    }
    rt_spin_unlock_irqrestore(lock, level);
}

/**
 * @brief 启动或重新启动一个定时器。
 *
 * 函数根据标志选择硬/软表。线程内置定时器还会先持有调度器锁并通知调度器，
 * 使“线程状态改变”和“等待超时定时器入队”保持同步；普通用户定时器不需要
 * 这一步。随后在对应定时器锁内调用 `_timer_start()`。
 *
 * 再次启动已激活对象会取消旧截止时间，并从调用时的当前 tick 重新计时。
 * `rt_object_take_hook` 在定时器表锁内调用。
 *
 * @param timer 要启动的合法定时器。
 * @return 当前实现成功返回 `RT_EOK`。
 */
rt_err_t rt_timer_start(rt_timer_t timer)
{
    rt_sched_lock_level_t slvl;
    int is_thread_timer = 0;
    struct rt_spinlock *spinlock;
    rt_list_t *timer_list;
    rt_base_t level;
    rt_err_t err;

    /* 类型断言避免把其他内核对象误解释为 rt_timer。 */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

#ifdef RT_USING_TIMER_ALL_SOFT
    timer_list = _soft_timer_list;
    spinlock = &_stimer_lock;
#else
#ifdef RT_USING_TIMER_SOFT
    if (timer->parent.flag & RT_TIMER_FLAG_SOFT_TIMER)
    {
        timer_list = _soft_timer_list;
        spinlock = &_stimer_lock;
    }
    else
#endif /* RT_USING_TIMER_SOFT */
    {
        timer_list = _timer_list;
        spinlock = &_htimer_lock;
    }
#endif

    if (timer->parent.flag & RT_TIMER_FLAG_THREAD_TIMER)
    {
        rt_thread_t thread;
        is_thread_timer = 1;
        rt_sched_lock(&slvl);

        thread = rt_container_of(timer, struct rt_thread, thread_timer);
        RT_ASSERT(rt_object_get_type(&thread->parent) == RT_Object_Class_Thread);
        rt_sched_thread_timer_start(thread);
    }

    level = rt_spin_lock_irqsave(spinlock);

    err = _timer_start(timer_list, timer);

    rt_spin_unlock_irqrestore(spinlock, level);

    if (is_thread_timer)
    {
        rt_sched_unlock(slvl);
    }

    return err;
}
RTM_EXPORT(rt_timer_start);

/**
 * @brief 停止一个处于激活状态的定时器。
 *
 * 本函数在对应表锁内检查 ACTIVATED、调用 `rt_object_put_hook`、从跳表移除并
 * 清除状态位。put hook 因此在持自旋锁且本地中断关闭时执行，不可阻塞或重入
 * 获取同一锁的定时器 API。停止操作不会注销或释放对象。
 *
 * @param timer 要停止的定时器。
 * @return 成功返回 `RT_EOK`；对象本来就未激活时返回 `-RT_ERROR`。
 */
rt_err_t rt_timer_stop(rt_timer_t timer)
{
    rt_base_t level;
    struct rt_spinlock *spinlock;

    /* 这里只验证对象类型；静态和动态定时器都可启动、停止。 */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    spinlock = _timerlock_idx(timer);

    level = rt_spin_lock_irqsave(spinlock);

    if (!(timer->parent.flag & RT_TIMER_FLAG_ACTIVATED))
    {
        rt_spin_unlock_irqrestore(spinlock, level);
        return -RT_ERROR;
    }
    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(timer->parent)));

    _timer_remove(timer);
    /* 节点和 ACTIVATED 在同一临界区内同步更新。 */
    timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;

    rt_spin_unlock_irqrestore(spinlock, level);

    return RT_EOK;
}
RTM_EXPORT(rt_timer_stop);

/**
 * @brief 在定时器表锁保护下读取或修改定时器属性。
 *
 * `RT_TIMER_CTRL_SET_TIME` 若发现对象正在运行，会先停止它，但不会自动以新周期
 * 重启；调用者需要再次 `rt_timer_start()`。GET_REMAIN_TIME 当前返回的是保存的
 * 绝对 `timeout_tick`，不是“还剩多少 tick”，调用者若需要相对值应结合当前
 * tick 并考虑回绕。未知命令当前也返回 `RT_EOK`，不能据此判断命令有效性。
 *
 * @param timer 要控制的定时器。
 * @param cmd `RT_TIMER_CTRL_*` 控制命令。
 * @param arg 命令相关的输入/输出地址；调用者必须提供正确类型和有效存储。
 * @return 当前实现固定返回 `RT_EOK`；参数错误主要由断言发现。
 */
rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg)
{
    struct rt_spinlock *spinlock;
    rt_base_t level;

    /* 所有字段访问都在与该对象队列一致的锁下完成。 */
    RT_ASSERT(timer != RT_NULL);
    RT_ASSERT(rt_object_get_type(&timer->parent) == RT_Object_Class_Timer);

    spinlock = _timerlock_idx(timer);

    level = rt_spin_lock_irqsave(spinlock);
    switch (cmd)
    {
    case RT_TIMER_CTRL_GET_TIME:
        *(rt_tick_t *)arg = timer->init_tick;
        break;

    case RT_TIMER_CTRL_SET_TIME:
        RT_ASSERT((*(rt_tick_t *)arg) < RT_TICK_MAX / 2);
        if (timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
        {
            _timer_remove(timer);
            timer->parent.flag &= ~RT_TIMER_FLAG_ACTIVATED;
        }
        timer->init_tick = *(rt_tick_t *)arg;
        break;

    case RT_TIMER_CTRL_SET_ONESHOT:
        timer->parent.flag &= ~RT_TIMER_FLAG_PERIODIC;
        break;

    case RT_TIMER_CTRL_SET_PERIODIC:
        timer->parent.flag |= RT_TIMER_FLAG_PERIODIC;
        break;

    case RT_TIMER_CTRL_GET_STATE:
        if(timer->parent.flag & RT_TIMER_FLAG_ACTIVATED)
        {
            /* 定时器已启动并位于（或正由回调临时处理于）运行状态。 */
            *(rt_uint32_t *)arg = RT_TIMER_FLAG_ACTIVATED;
        }
        else
        {
            /* 定时器当前未激活。 */
            *(rt_uint32_t *)arg = RT_TIMER_FLAG_DEACTIVATED;
        }
        break;

    case RT_TIMER_CTRL_GET_REMAIN_TIME:
        *(rt_tick_t *)arg =  timer->timeout_tick;
        break;
    case RT_TIMER_CTRL_GET_FUNC:
        *(void **)arg = (void *)timer->timeout_func;
        break;

    case RT_TIMER_CTRL_SET_FUNC:
        timer->timeout_func = (void (*)(void*))arg;
        break;

    case RT_TIMER_CTRL_GET_PARM:
        *(void **)arg = timer->parameter;
        break;

    case RT_TIMER_CTRL_SET_PARM:
        timer->parameter = arg;
        break;

    default:
        break;
    }
    rt_spin_unlock_irqrestore(spinlock, level);

    return RT_EOK;
}
RTM_EXPORT(rt_timer_control);

/**
 * @brief 系统 tick 中断中的定时器入口。
 *
 * @note 必须在已经执行 `rt_interrupt_enter()` 的中断上下文调用，函数用断言
 *       检查中断嵌套层数。SMP 下只有 CPU 0 真正扫描公共定时器表。
 *
 * 对软定时器，本函数只查看最早截止时间并释放通知信号量，不在中断中执行
 * 回调；对硬定时器则立即调用 `_timer_check()`，故硬回调也在此中断上下文。
 */
void rt_timer_check(void)
{
    RT_ASSERT(rt_interrupt_get_nest() > 0);

#ifdef RT_USING_SMP
    /* 全局定时器队列只由 0 号 CPU 推进，其他 CPU 直接返回。 */
    if (rt_cpu_get_id() != 0)
    {
        return;
    }
#endif

#ifdef RT_USING_TIMER_SOFT
    rt_err_t ret = RT_ERROR;
    rt_tick_t next_timeout;

    ret = _timer_list_next_timeout(_soft_timer_list, &next_timeout);
    if ((ret == RT_EOK) && (next_timeout <= rt_tick_get()))
    {
        rt_sem_release(&_soft_timer_sem);
    }
#endif
#ifndef RT_USING_TIMER_ALL_SOFT
    _timer_check(_timer_list, &_htimer_lock);
#endif
}

/**
 * @brief 查询硬、软两个定时器表中最早的绝对到期 tick。
 *
 * 函数分别持有每张表的锁读取其首节点，再取较小值。若所有已编译的表都为空，
 * 返回 `RT_TICK_MAX`。返回的是绝对 tick，并且两次读表之间状态可能发生改变，
 * 因此它适合调度下一次唤醒，不构成对定时器状态的持久保证。
 *
 * @return 系统当前所见的最早绝对到期 tick，或 `RT_TICK_MAX`。
 */
rt_tick_t rt_timer_next_timeout_tick(void)
{
    rt_base_t level;
    rt_tick_t htimer_next_timeout = RT_TICK_MAX, stimer_next_timeout = RT_TICK_MAX;

#ifndef RT_USING_TIMER_ALL_SOFT
    level = rt_spin_lock_irqsave(&_htimer_lock);
    _timer_list_next_timeout(_timer_list, &htimer_next_timeout);
    rt_spin_unlock_irqrestore(&_htimer_lock, level);
#endif

#ifdef RT_USING_TIMER_SOFT
    level = rt_spin_lock_irqsave(&_stimer_lock);
    _timer_list_next_timeout(_soft_timer_list, &stimer_next_timeout);
    rt_spin_unlock_irqrestore(&_stimer_lock, level);
#endif

    return htimer_next_timeout < stimer_next_timeout ? htimer_next_timeout : stimer_next_timeout;
}

#ifdef RT_USING_TIMER_SOFT
/**
 * @brief 软定时器系统线程入口。
 *
 * 每轮先处理当前已经到期的全部软定时器，再永久等待 tick 中断释放信号量。
 * 先检查后等待也保证线程刚启动时已经到期的对象不会漏掉。所有软定时器回调
 * 串行运行在这个线程中，所以一个耗时回调会延迟其后的回调。
 *
 * @param parameter 未使用的线程入口参数。
 */
static void _timer_thread_entry(void *parameter)
{
    RT_UNUSED(parameter);

    while (1)
    {
        _timer_check(_soft_timer_list, &_stimer_lock); /* 处理目前所有已到期的软定时器。 */
        rt_sem_take(&_soft_timer_sem, RT_WAITING_FOREVER);
    }
}
#endif /* RT_USING_TIMER_SOFT */

/**
 * @ingroup group_system_init
 *
 * @brief 初始化硬定时器跳表及其自旋锁。
 *
 * @note 这是内核启动阶段调用的系统初始化函数；使用硬定时器 API 前必须完成。
 */
void rt_system_timer_init(void)
{
#ifndef RT_USING_TIMER_ALL_SOFT
    rt_size_t i;

    for (i = 0; i < sizeof(_timer_list) / sizeof(_timer_list[0]); i++)
    {
        rt_list_init(_timer_list + i);
    }

    rt_spin_lock_init(&_htimer_lock);
#endif
}

/**
 * @ingroup group_system_init
 *
 * @brief 初始化软定时器跳表、通知信号量和系统定时器线程。
 *
 * 信号量初值为 0、等待策略为优先级顺序，上限设为 1；然后以配置的栈大小和
 * 优先级创建静态线程并启动。未启用 `RT_USING_TIMER_SOFT` 时函数为空操作。
 */
void rt_system_timer_thread_init(void)
{
#ifdef RT_USING_TIMER_SOFT
    int i;

    for (i = 0;
         i < sizeof(_soft_timer_list) / sizeof(_soft_timer_list[0]);
         i++)
    {
        rt_list_init(_soft_timer_list + i);
    }
    rt_spin_lock_init(&_stimer_lock);
    rt_sem_init(&_soft_timer_sem, "stimer", 0, RT_IPC_FLAG_PRIO);
    rt_sem_control(&_soft_timer_sem, RT_IPC_CMD_SET_VLIMIT, (void*)1);
    /* 创建使用静态控制块和静态栈的软定时器服务线程。 */
    rt_thread_init(&_timer_thread,
                   "timer",
                   _timer_thread_entry,
                   RT_NULL,
                   &_timer_thread_stack[0],
                   sizeof(_timer_thread_stack),
                   RT_TIMER_THREAD_PRIO,
                   10);

    /* 加入就绪队列，之后由调度器安排其首次运行。 */
    rt_thread_startup(&_timer_thread);
#endif /* RT_USING_TIMER_SOFT */
}

/**@}*/
