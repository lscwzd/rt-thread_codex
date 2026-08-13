/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2006-03-14     Bernard      初始版本
 * 2006-04-25     Bernard      实现信号量
 * 2006-05-03     Bernard      增加 RT_IPC_DEBUG
 *                             将 IPC 等待时间类型改为 rt_int32_t
 * 2006-05-10     Bernard      修复信号量获取问题并增加 IPC 对象
 * 2006-05-12     Bernard      实现邮箱和消息队列
 * 2006-05-20     Bernard      实现互斥量
 * 2006-05-23     Bernard      实现快速事件
 * 2006-05-24     Bernard      实现事件
 * 2006-06-03     Bernard      修复线程定时器初始化问题
 * 2006-06-05     Bernard      修复互斥量释放问题
 * 2006-06-07     Bernard      修复消息队列发送问题
 * 2006-08-04     Bernard      增加 hook 支持
 * 2009-05-21     Yi.qiu       修复信号量释放问题
 * 2009-07-18     Bernard      修复事件清除问题
 * 2009-09-09     Bernard      移除快速事件并修复 IPC 释放问题
 * 2009-10-10     Bernard      将信号量和互斥量计数改为无符号类型
 * 2009-10-25     Bernard      重算后的剩余 tick 为负时，将 mb/mq 接收超时改为 0
 * 2009-12-16     Bernard      修复 PRIO 模式下 rt_ipc_object_suspend 的问题
 * 2010-01-20     mbbill       移除 rt_ipc_object_decrease 函数
 * 2010-04-20     Bernard      将 mq 的 memcpy 移到关中断区外
 * 2010-10-26     yi.qiu       为 rt_mp_delete 和 rt_mq_delete 增加模块支持
 * 2010-11-10     Bernard      实现 IPC reset 命令
 * 2011-12-18     Bernard      为消息队列增加参数检查
 * 2013-09-14     Grissiom     为 rt_event_recv 增加 option 检查
 * 2018-10-02     Bernard      为邮箱增加 64 位支持
 * 2019-09-16     tyx          为消息队列增加等待发送支持
 * 2020-07-29     Meco Man     修复无需挂起即收到事件时的 event_set/event_info
 * 2020-10-11     Meco Man     增加数值溢出检查
 * 2021-01-03     Meco Man     实现 rt_mb_urgent()
 * 2021-05-30     Meco Man     实现 rt_mutex_trytake()
 * 2022-01-07     Gabriel      将 __on_rt_xxxxx_hook 移入 ipc.c
 * 2022-01-24     THEWON       使用信号时让 rt_mutex_take 返回 thread->error
 * 2022-04-08     Stanley      修正说明
 * 2022-10-15     Bernard      增加嵌套互斥量功能
 * 2022-10-16     Bernard      增加优先级天花板功能
 * 2023-04-16     Xin-zheqi    重新设计队列收发接口，使其返回真实消息长度
 * 2023-09-15     xqyjlj       优化 rt_hw_interrupt_disable/enable
 */

#include <rtthread.h>
#include <rthw.h>

/**
 * @file ipc.c
 * @brief RT-Thread 内核 IPC 对象及其公共“等待—唤醒”协议实现。
 *
 * 本文件实现五类常用线程间通信对象：
 *
 * - 信号量：一个有上限的资源计数；有计数就减一，没有就等待。
 * - 互斥量：带所有者和递归计数的独占锁，并实现优先级继承/优先级天花板。
 * - 事件：一个 32 位标志集合；线程可等待任意位或全部位，并可选择收到后清除。
 * - 邮箱：固定槽位的 `rt_ubase_t` 值环形队列，适合传整数或指针。
 * - 消息队列：固定大小消息块池，发送时复制数据，支持普通、紧急和优先级消息。
 *
 * 它们共享 `struct rt_ipc_object` 中的等待线程链。资源暂不可用时，线程在对象
 * 自旋锁保护下调用 `rt_thread_suspend_to_list()`：线程进入挂起态，并按对象的
 * FIFO 或 PRIO 策略上链；有限等待还会启动线程内置定时器。释放资源、超时、
 * 信号或对象销毁中最先“抢到”该线程的一方，会在调度器锁保护下把它从等待链
 * 转为就绪。醒来的线程通过 `thread->error` 区分正常通知、超时、中断和销毁；
 * 许多等待函数内部保存正错误码，向 API 调用者返回时再规范为负错误值。
 *
 * 每个具体 IPC 对象另有一把自旋锁，保护资源计数、环形队列游标、消息块链、
 * 所有者等私有状态。自旋锁临界区会关闭本地中断，必须短小且不能阻塞。需要
 * 睡眠时，实现会先在锁内完成“检查条件 + 挂入等待链”，再解锁并调度，从而
 * 避免资源释放恰好发生在检查与睡眠之间所导致的丢失唤醒。
 *
 * `rt_object_trytake_hook` 观察获取尝试，`rt_object_take_hook` 观察成功获取，
 * `rt_object_put_hook` 观察释放或发送。它们的确切持锁状态随 API 不同，下面在
 * 复杂函数旁逐一说明；通用原则是 hook 必须短小、不可阻塞，也不可重入操作
 * 同一个 IPC 对象。
 */

#define DBG_TAG           "kernel.ipc"
#define DBG_LVL           DBG_INFO
#include <rtdbg.h>

/*
 * 消息块内存布局为 `[struct rt_mq_message 头][对齐后的数据区]`。把头指针加 1
 * 就得到紧随其后的数据起始地址；宏参数只求值一次，但必须确实指向消息块头。
 */
#define GET_MESSAGEBYTE_ADDR(msg)               ((struct rt_mq_message *) msg + 1)
#if defined(RT_USING_HOOK) && defined(RT_HOOK_USING_FUNC_PTR)
extern void (*rt_object_trytake_hook)(struct rt_object *object);
extern void (*rt_object_take_hook)(struct rt_object *object);
extern void (*rt_object_put_hook)(struct rt_object *object);
#endif /* RT_USING_HOOK */

/**
 * @addtogroup group_thread_comm
 * @{
 */

/**
 * @brief 初始化所有 IPC 对象共有的等待线程链（内部函数）。
 *
 * 具体类型的初始化函数还必须设置计数、缓冲区、标志和自己的自旋锁；本函数
 * 只把 `suspend_thread` 建成空的双向循环链表。
 *
 * @param ipc 要初始化的嵌入式 IPC 基类。
 *
 * @return 当前实现固定返回 `RT_EOK`。
 */
rt_inline rt_err_t _ipc_object_init(struct rt_ipc_object *ipc)
{
    /* 空链表表示当前没有线程因等待这个对象而挂起。 */
    rt_list_init(&(ipc->suspend_thread));

    return RT_EOK;
}


/**
 * @brief 原子地从等待链取出第一个仍可唤醒的线程，并将其放回就绪队列。
 *
 * 函数自行获取调度器锁，使“取链首 + 转为 ready”相对超时和信号唤醒保持原子。
 * `rt_sched_thread_ready()` 失败通常说明其他异步路径已经先处理该线程，此时返回
 * `RT_NULL`，调用者不能把资源交给它。成功时，非负 `thread_error` 写入线程供
 * 醒来后的等待 API 判断原因；传入负值表示保留线程原有错误码。
 *
 * @param susp_list 非空等待链。链首体现 FIFO 或优先级策略。
 * @param thread_error 要交给被唤醒线程的内部错误码；负值表示不覆盖。
 * @return 成功返回刚转为 ready 的线程；链空或竞争失败返回 `RT_NULL`。
 */
struct rt_thread *rt_susp_list_dequeue(rt_list_t *susp_list, rt_err_t thread_error)
{
    rt_sched_lock_level_t slvl;
    rt_thread_t thread;
    rt_err_t error;

    RT_SCHED_DEBUG_IS_UNLOCKED;
    RT_ASSERT(susp_list != RT_NULL);

    rt_sched_lock(&slvl);
    if (!rt_list_isempty(susp_list))
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(susp_list->next);
        error = rt_sched_thread_ready(thread);

        if (error)
        {
            LOG_D("%s [error:%d] failed to resume thread:%p from suspended list",
                  __func__, error, thread);

            thread = RT_NULL;
        }
        else
        {
            /* 等待路径内部通常使用非负错误码，API 返回前再转为负值。 */
            if (thread_error >= 0)
            {
                /* `RT_EOK` 表示资源生产者正常唤醒，其他值说明异常结束等待。 */
                thread->error = thread_error;
            }
        }
    }
    else
    {
        thread = RT_NULL;
    }
    rt_sched_unlock(slvl);

    LOG_D("resume thread:%s\n", thread->parent.name);

    return thread;
}


/**
 * @brief 唤醒指定等待链中当前所有可唤醒线程。
 *
 * 反复调用 `rt_susp_list_dequeue()`，每次内部短暂获取调度器锁。调用者通常已经
 * 持有具体 IPC 对象锁，以阻止新的等待者同时加入。函数只把线程置为 ready，
 * 不主动调用 `rt_schedule()`；外层在释放对象锁后决定是否调度。
 *
 * @param susp_list 要清空的等待线程链。
 * @param thread_error 写给每个成功唤醒线程的非负内部状态；负值表示不覆盖。
 *
 * @return 当前实现固定返回 `RT_EOK`。
 */
rt_err_t rt_susp_list_resume_all(rt_list_t *susp_list, rt_err_t thread_error)
{
    struct rt_thread *thread;

    RT_SCHED_DEBUG_IS_UNLOCKED;

    /* dequeue 会把成功处理的线程从链上移除，直至链空或没有可处理者。 */
    thread = rt_susp_list_dequeue(susp_list, thread_error);
    while (thread)
    {
        /*
         * 处理下一个等待者；ready 操作同时负责从原挂起链摘除线程节点。
         */
        thread = rt_susp_list_dequeue(susp_list, thread_error);
    }

    return RT_EOK;
}

/**
 * @brief 在调用者提供的对象锁保护下逐个唤醒等待链中的全部线程。
 *
 * 每轮获取 `lock`、尝试唤醒一个线程、再释放 `lock`。这种形式适合调用者当前
 * 没有持续持锁而又需要保护等待链的场景；调度器锁仍由 dequeue 内部管理。
 *
 * @param susp_list 要清空的等待线程链。
 * @param thread_error 交给醒来线程的内部状态；负值表示不覆盖。
 * @param lock 每次操作等待链时要获取的对象自旋锁。
 *
 * @return 当前实现固定返回 `RT_EOK`。
 */
rt_err_t rt_susp_list_resume_all_irq(rt_list_t *susp_list,
                                     rt_err_t thread_error,
                                     struct rt_spinlock *lock)
{
    struct rt_thread *thread;
    rt_base_t level;

    RT_SCHED_DEBUG_IS_UNLOCKED;

    do
    {
        level = rt_spin_lock_irqsave(lock);

        /*
         * 每轮只处理一个线程，以便在两次唤醒之间短暂释放对象锁。
         */
        thread = rt_susp_list_dequeue(susp_list, thread_error);

        rt_spin_unlock_irqrestore(lock, level);
    }
    while (thread);

    return RT_EOK;
}

/**
 * @brief 按 IPC 等待策略把已挂起线程插入等待链。
 *
 * @note 调用者必须已持有调度器锁；线程状态的改变通常由
 *       `rt_thread_suspend_to_list()` 与本函数配套完成。
 *
 * FIFO 把线程追加到链尾；PRIO 按“数值越小优先级越高”升序插入，同优先级线程
 * 仍追加在已有同级线程之后，保持同级 FIFO。未知标志触发断言。
 *
 * @param susp_list 目标等待链。
 * @param thread 已处于挂起转换过程的线程。
 * @param ipc_flags `RT_IPC_FLAG_FIFO` 或 `RT_IPC_FLAG_PRIO`。
 * @return 当前合法分支固定返回 `RT_EOK`。
 */
rt_err_t rt_susp_list_enqueue(rt_list_t *susp_list, rt_thread_t thread, int ipc_flags)
{
    RT_SCHED_DEBUG_IS_LOCKED;

    switch (ipc_flags)
    {
    case RT_IPC_FLAG_FIFO:
        rt_list_insert_before(susp_list, &RT_THREAD_LIST_NODE(thread));
        break; /* RT_IPC_FLAG_FIFO */

    case RT_IPC_FLAG_PRIO:
        {
            struct rt_list_node *n;
            struct rt_thread *sthread;

            /* 从最高优先级一端开始寻找第一个优先级低于新线程的节点。 */
            for (n = susp_list->next; n != susp_list; n = n->next)
            {
                sthread = RT_THREAD_LIST_NODE_ENTRY(n);

                /* 优先级数值更小，说明新线程应当排在现有线程前面。 */
                if (rt_sched_thread_get_curr_prio(thread) < rt_sched_thread_get_curr_prio(sthread))
                {
                    /* 插到第一个较低优先级等待者之前。 */
                    rt_list_insert_before(&RT_THREAD_LIST_NODE(sthread), &RT_THREAD_LIST_NODE(thread));
                    break;
                }
            }

            /*
             * 没有更低优先级节点，说明新线程优先级最低或与末尾相同，追加到链尾。
             */
            if (n == susp_list)
                rt_list_insert_before(susp_list, &RT_THREAD_LIST_NODE(thread));
        }
        break;/* RT_IPC_FLAG_PRIO */

    default:
        RT_ASSERT(0);
        break;
    }

    return RT_EOK;
}

/**
 * @brief 按等待顺序把挂起链中的线程名称输出到系统控制台。
 *
 * 为防止调度状态并发改变，遍历期间持有调度器锁。名称之间用 `/` 分隔；未启用
 * 控制台时参数仅被忽略，不产生输出。本函数主要用于调试，不应放在实时关键路径。
 *
 * @param list 要打印的等待线程链。
 */
void rt_susp_list_print(rt_list_t *list)
{
#ifdef RT_USING_CONSOLE
    rt_sched_lock_level_t slvl;
    struct rt_thread *thread;
    struct rt_list_node *node;

    rt_sched_lock(&slvl);

    for (node = list->next; node != list; node = node->next)
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(node);
        rt_kprintf("%.*s", RT_NAME_MAX, thread->parent.name);

        if (node->next != list)
            rt_kprintf("/");
    }

    rt_sched_unlock(slvl);
#else
    (void)list;
#endif
}


#ifdef RT_USING_SEMAPHORE
/**
 * @addtogroup group_semaphore Semaphore
 * @{
 */

static void _sem_object_init(rt_sem_t       sem,
                             rt_uint16_t    value,
                             rt_uint8_t     flag,
                             rt_uint16_t    max_value)
{
    /* 初始化公共等待链，再设置计数上限、当前计数、等待策略和私有锁。 */
    _ipc_object_init(&(sem->parent));

    sem->max_value = max_value;
    /* value 表示当前可立即获取的资源份数。 */
    sem->value = value;

    /* 通用对象 flag 保存 FIFO/PRIO 等待者排序策略。 */
    sem->parent.parent.flag = flag;
    rt_spin_lock_init(&(sem->spinlock));
}

/**
 * @brief 初始化一个使用调用者自备存储的静态信号量。
 *
 * 信号量可理解为“资源票数”：获取成功减一，释放且无人等待时加一；值为 0 时
 * 获取者按 `flag` 排入等待链。静态对象存储必须在其整个使用期保持有效，最终
 * 用 `rt_sem_detach()` 注销，不能用动态对象的 delete。
 *
 * @see      rt_sem_create()
 *
 * @param sem 指向调用者提供的 `struct rt_semaphore`。
 *
 * @param name 对象名称。
 *
 * @param value 初始资源数，必须小于 65536。保护 N 个同类资源时设为 N；仅用于
 *              事件通知时通常设为 0。默认最大值为 `RT_SEM_VALUE_MAX`。
 *
 * @param flag 等待策略：`RT_IPC_FLAG_PRIO` 让高优先级线程先得到资源；
 *             `RT_IPC_FLAG_FIFO` 严格按到达顺序。实时场景通常选 PRIO，FIFO 可能
 *             让高优先级线程排在早到的低优先级线程之后。
 *
 * @return 成功返回 `RT_EOK`；非法参数由断言报告。
 *
 * @warning 仅在线程上下文初始化，并保证没有并发用户。
 */
rt_err_t rt_sem_init(rt_sem_t    sem,
                     const char *name,
                     rt_uint32_t value,
                     rt_uint8_t  flag)
{
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(value < 0x10000U);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* 先登记静态通用对象，再初始化信号量私有状态。 */
    rt_object_init(&(sem->parent.parent), RT_Object_Class_Semaphore, name);

    _sem_object_init(sem, value, flag, RT_SEM_VALUE_MAX);

    return RT_EOK;
}
RTM_EXPORT(rt_sem_init);


/**
 * @brief 注销静态信号量并让全部等待者以错误结束等待。
 *
 * 在信号量锁内把等待链所有线程置为 ready，并写入 `RT_ERROR`；解锁后从对象系统
 * 注销，但不释放 `sem` 的存储。函数本身不显式调度，调用者必须确保注销期间
 * 不再有新的获取/释放操作，并在之后按系统调度点让等待者运行。
 *
 * @see      rt_sem_delete()
 *
 * @param sem 由 `rt_sem_init()` 初始化的静态信号量。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 不能用于 `rt_sem_create()` 创建的动态对象。
 */
rt_err_t rt_sem_detach(rt_sem_t sem)
{
    rt_base_t level;

    /* 生命周期断言确保只注销静态对象。 */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);
    RT_ASSERT(rt_object_is_systemobject(&sem->parent.parent));

    level = rt_spin_lock_irqsave(&(sem->spinlock));
    /* 等待者醒来后会把 RT_ERROR 规范为负错误返回。 */
    rt_susp_list_resume_all(&(sem->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(sem->spinlock), level);

    /* 不释放调用者拥有的结构体存储。 */
    rt_object_detach(&(sem->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_sem_detach);

#ifdef RT_USING_HEAP
/**
 * @brief 从内核堆创建一个动态信号量。
 *
 * 行为参数与 `rt_sem_init()` 相同；区别是对象存储由内核分配，最终必须用
 * `rt_sem_delete()` 释放。
 *
 * @see      rt_sem_init()
 *
 * @param name 对象名称。
 *
 * @param value 初始资源计数，必须小于 65536。
 *
 * @param flag `RT_IPC_FLAG_PRIO` 或 `RT_IPC_FLAG_FIFO` 等待策略。
 *
 * @return 成功返回信号量；内存不足返回 `RT_NULL`。
 *
 * @warning 包含堆分配，只能在线程上下文调用。
 */
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag)
{
    rt_sem_t sem;

    RT_ASSERT(value < 0x10000U);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 分配器同时完成动态通用对象的登记。 */
    sem = (rt_sem_t)rt_object_allocate(RT_Object_Class_Semaphore, name);
    if (sem == RT_NULL)
        return sem;

    _sem_object_init(sem, value, flag, RT_SEM_VALUE_MAX);

    return sem;
}
RTM_EXPORT(rt_sem_create);


/**
 * @brief 删除动态信号量、唤醒全部等待者并释放对象内存。
 *
 * 等待者在对象锁内以 `RT_ERROR` 转为 ready，解锁后对象立即注销并释放。调用者
 * 必须在更高层保证没有并发访问；被唤醒线程不能再解引用已经删除的 `sem`。
 *
 * @see      rt_sem_detach()
 *
 * @param sem 由 `rt_sem_create()` 创建的动态信号量。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 静态信号量必须使用 `rt_sem_detach()`，不能交给本函数释放。
 */
rt_err_t rt_sem_delete(rt_sem_t sem)
{
    rt_ubase_t level;

    /* 生命周期断言确保对象确实归内核堆管理。 */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);
    RT_ASSERT(rt_object_is_systemobject(&sem->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    level = rt_spin_lock_irqsave(&(sem->spinlock));
    /* 先结束所有睡眠，避免仍有线程挂在即将释放的链表上。 */
    rt_susp_list_resume_all(&(sem->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(sem->spinlock), level);

    /* 注销通用对象并释放结构体。 */
    rt_object_delete(&(sem->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_sem_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief 获取信号量的公共内部实现，支持三种可中断等级。
 *
 * try-take hook 在参数检查后、尚未获取信号量锁时调用。加锁后若 `value > 0`，
 * 直接减一并解锁；若为 0 且 timeout 为 0，立即返回超时；否则在同一锁周期中
 * 把当前线程挂到等待链，有限等待再配置并启动线程内置定时器。解锁调度后，
 * 线程可能因 release、超时、信号或对象销毁醒来，依据 `thread->error` 返回。
 * 只有真正取得资源时才在解锁后调用 take hook。
 *
 * @see      rt_sem_trytake()
 *
 * @param sem 目标信号量。
 *
 * @param timeout 等待 tick 数；`RT_WAITING_NO`/0 表示不等待，正数表示有限等待，
 *                `RT_WAITING_FOREVER` 表示不启动超时定时器。
 * @param suspend_flag `RT_UNINTERRUPTIBLE`、`RT_INTERRUPTIBLE` 或 `RT_KILLABLE`，
 *                     决定哪些信号可以中断挂起。
 *
 * @return 取得资源返回 `RT_EOK`；立即/定时超时返回 `-RT_ETIMEOUT`；也可能返回
 *         挂起、信号中断或对象销毁路径提供的其他负错误码。
 *
 * @warning 需要当前线程和调度器，只能在线程上下文调用，即使 timeout 为 0。
 */
static rt_err_t _rt_sem_take(rt_sem_t sem, rt_int32_t timeout, int suspend_flag)
{
    rt_base_t level;
    struct rt_thread *thread;
    rt_err_t ret;

    /* 类型断言防止用其他 IPC 对象冒充信号量。 */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(sem->parent.parent)));

    /* 获取路径可能睡眠，因此要求调度器处于可用状态。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(1);

    level = rt_spin_lock_irqsave(&(sem->spinlock));

    LOG_D("thread %s take sem:%s, which value is: %d",
          rt_thread_self()->parent.name,
          sem->parent.parent.name,
          sem->value);

    if (sem->value > 0)
    {
        /* 快速路径：在锁内原子消费一个资源计数。 */
        sem->value --;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level);
    }
    else
    {
        /* 非阻塞调用没有资源，直接报告超时语义。 */
        if (timeout == 0)
        {
            rt_spin_unlock_irqrestore(&(sem->spinlock), level);
            return -RT_ETIMEOUT;
        }
        else
        {
            /* 慢路径：资源不可用，在仍持对象锁时取得当前线程并准备挂起。 */
            thread = rt_thread_self();

            /* 先设为 EINTR；真正的唤醒者或超时路径会按原因覆盖。 */
            thread->error = RT_EINTR;

            LOG_D("sem take: suspend thread - %s", thread->parent.name);

            /* 状态改变和等待链插入由调度器协同完成，防止丢失唤醒。 */
            ret = rt_thread_suspend_to_list(thread, &(sem->parent.suspend_thread),
                                            sem->parent.parent.flag, suspend_flag);
            if (ret != RT_EOK)
            {
                rt_spin_unlock_irqrestore(&(sem->spinlock), level);
                return ret;
            }

            /* 正数表示有限等待；FOREVER 不启动线程定时器。 */
            if (timeout > 0)
            {
                rt_tick_t timeout_tick = timeout;
                LOG_D("set thread:%s to timer list", thread->parent.name);

                /* 重设线程内置定时器，本次等待到期时由它把线程转为 ready。 */
                rt_timer_control(&(thread->thread_timer),
                                 RT_TIMER_CTRL_SET_TIME,
                                 &timeout_tick);
                rt_timer_start(&(thread->thread_timer));
            }

            /* 挂起登记完成后才能释放对象锁。 */
            rt_spin_unlock_irqrestore(&(sem->spinlock), level);

            /* 当前线程已不可运行，切换到其他就绪线程，直到某路径唤醒自己。 */
            rt_schedule();

            if (thread->error != RT_EOK)
            {
                return thread->error > 0 ? -thread->error : thread->error;
            }
        }
    }

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(sem->parent.parent)));

    return RT_EOK;
}

/**
 * @brief 以不可被信号打断的模式获取信号量。
 * @param sem 目标信号量。
 * @param time 0、有限 tick 或 `RT_WAITING_FOREVER`。
 * @return 语义见 `_rt_sem_take()`。
 */
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_sem_take);

/** @brief 以可被普通信号中断的方式获取信号量；其余语义同 `rt_sem_take()`。 */
rt_err_t rt_sem_take_interruptible(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_sem_take_interruptible);

/** @brief 以仅可被致命信号中断的方式获取信号量；其余语义同 `rt_sem_take()`。 */
rt_err_t rt_sem_take_killable(rt_sem_t sem, rt_int32_t time)
{
    return _rt_sem_take(sem, time, RT_KILLABLE);
}
RTM_EXPORT(rt_sem_take_killable);

/**
 * @brief 非阻塞地尝试获取一次信号量。
 *
 * 完全等价于 `rt_sem_take(sem, RT_WAITING_NO)`；无资源时返回
 * `-RT_ETIMEOUT`，不会把线程挂入等待链。
 *
 * @see      rt_sem_take()
 *
 * @param sem 目标信号量。
 *
 * @return 取得资源返回 `RT_EOK`，否则返回负错误码。
 */
rt_err_t rt_sem_trytake(rt_sem_t sem)
{
    return rt_sem_take(sem, RT_WAITING_NO);
}
RTM_EXPORT(rt_sem_trytake);


/**
 * @brief 释放一个信号量资源，优先直接交给等待链首线程。
 *
 * put hook 在尚未获取信号量锁时调用。锁内若存在等待者，不增加 `value`，而是
 * 直接以 `RT_EOK` 唤醒链首，相当于把刚释放的资源所有权交给它；若无人等待，
 * 才把计数加一。解锁后如有线程醒来则请求调度。该设计避免“先加计数、再由
 * 其他线程抢走”而饿死已经排队的等待者。
 *
 * @param sem 目标信号量。
 *
 * @return 成功返回 `RT_EOK`；无人等待且计数已达 `max_value` 返回 `-RT_EFULL`。
 */
rt_err_t rt_sem_release(rt_sem_t sem)
{
    rt_base_t level;
    rt_bool_t need_schedule;

    /* release 不需要资源所有者，因此可用于线程间通知。 */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(sem->parent.parent)));

    need_schedule = RT_FALSE;

    level = rt_spin_lock_irqsave(&(sem->spinlock));

    LOG_D("thread %s releases sem:%s, which value is: %d",
          rt_thread_self()->parent.name,
          sem->parent.parent.name,
          sem->value);

    if (!rt_list_isempty(&sem->parent.suspend_thread))
    {
        /* 等待策略已体现在链表顺序，只需取链首。 */
        rt_susp_list_dequeue(&(sem->parent.suspend_thread), RT_EOK);
        need_schedule = RT_TRUE;
    }
    else
    {
        if(sem->value < sem->max_value)
        {
            sem->value ++; /* 无等待者时把可用资源数增加一。 */
        }
        else
        {
            rt_spin_unlock_irqrestore(&(sem->spinlock), level);
            return -RT_EFULL; /* 达到配置上限，拒绝溢出。 */
        }
    }

    rt_spin_unlock_irqrestore(&(sem->spinlock), level);

    /* 必须在释放对象锁后调度，避免新线程带锁运行。 */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK;
}
RTM_EXPORT(rt_sem_release);


/**
 * @brief 重置信号量或修改其允许的最大计数。
 *
 * `RT_IPC_CMD_RESET` 把 `arg` 的整数值直接解释为新当前计数，并让全部等待者以
 * `RT_ERROR` 醒来，随后调度。`RT_IPC_CMD_SET_VLIMIT` 设置 1..最大硬限制内的新
 * `max_value`；若新上限低于当前值且存在等待者，也以错误唤醒等待者，但当前
 * `value` 本身不会被截断。`arg` 是整数经 `void *` 传递，并非指向整数的地址。
 *
 * @param sem 目标信号量。
 *
 * @param cmd `RT_IPC_CMD_RESET` 或 `RT_IPC_CMD_SET_VLIMIT`。
 *
 * @param arg 通过指针宽整数强制转换传入的数值。
 *
 * @return 成功返回 `RT_EOK`；上限非法返回 `-RT_EINVAL`；命令未知返回
 *         `-RT_ERROR`。
 */
rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg)
{
    rt_base_t level;

    /* 修改和等待链清理均由信号量私有锁保护。 */
    RT_ASSERT(sem != RT_NULL);
    RT_ASSERT(rt_object_get_type(&sem->parent.parent) == RT_Object_Class_Semaphore);

    if (cmd == RT_IPC_CMD_RESET)
    {
        rt_ubase_t value;

        /* 此接口沿用把小整数编码进 void * 的历史约定。 */
        value = (rt_uintptr_t)arg;
        level = rt_spin_lock_irqsave(&(sem->spinlock));

        /* reset 使旧等待条件失效，所有等待者以错误离开。 */
        rt_susp_list_resume_all(&sem->parent.suspend_thread, RT_ERROR);

        /* 设置新的当前计数。 */
        sem->value = (rt_uint16_t)value;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level);
        rt_schedule();

        return RT_EOK;
    }
    else if (cmd == RT_IPC_CMD_SET_VLIMIT)
    {
        rt_ubase_t max_value;
        rt_bool_t need_schedule = RT_FALSE;

        max_value = (rt_uint16_t)((rt_uintptr_t)arg);
        if (max_value > RT_SEM_VALUE_MAX || max_value < 1)
        {
            return -RT_EINVAL;
        }

        level = rt_spin_lock_irqsave(&(sem->spinlock));
        if (max_value < sem->value)
        {
            if (!rt_list_isempty(&sem->parent.suspend_thread))
            {
                /* 上限收紧且已有等待者时，结束这些旧等待。 */
                rt_susp_list_resume_all(&sem->parent.suspend_thread, RT_ERROR);
                need_schedule = RT_TRUE;
            }
        }
        /* 只更新上限，不强制降低已经存在的 value。 */
        sem->max_value = max_value;
        rt_spin_unlock_irqrestore(&(sem->spinlock), level);

        if (need_schedule)
        {
            rt_schedule();
        }

        return RT_EOK;
    }

    return -RT_ERROR;
}
RTM_EXPORT(rt_sem_control);

/**@}*/
#endif /* RT_USING_SEMAPHORE */

#ifdef RT_USING_MUTEX
/**
 * @name 互斥量优先级协议内部辅助函数
 *
 * RT-Thread 的优先级数值越小，调度优先级越高。`mutex->priority` 缓存该互斥量
 * 等待链中的最高优先级；`thread->taken_object_list` 串起线程当前持有的所有
 * 互斥量。线程的有效优先级取其初始优先级、所持各锁最高等待者优先级、以及
 * 各锁优先级天花板中的最小数值。若锁的所有者又在等待另一把锁，提升会沿
 * `pending_object` 形成的链继续传播，解决嵌套锁中的传递式优先级反转。
 * @{
 */

/**
 * @brief 用等待链首重新计算互斥量缓存的最高等待优先级。
 *
 * 互斥等待链强制采用 PRIO 排序，所以非空时链首就是最高优先级等待者；空链
 * 用 0xff 表示“没有继承需求”。调用者负责持有互斥量锁和必要的调度器锁。
 */
rt_inline rt_uint8_t _mutex_update_priority(struct rt_mutex *mutex)
{
    struct rt_thread *thread;

    if (!rt_list_isempty(&mutex->parent.suspend_thread))
    {
        thread = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
        mutex->priority = rt_sched_thread_get_curr_prio(thread);
    }
    else
    {
        mutex->priority = 0xff;
    }

    return mutex->priority;
}

/**
 * @brief 计算一个线程根据初始优先级和全部已持互斥量应具有的有效优先级。
 *
 * 每把锁先在“最高等待者”和“天花板”中取较高者（较小数值），再与线程初始
 * 优先级比较。调用者必须稳定 `taken_object_list` 及相关优先级字段。
 */
rt_inline rt_uint8_t _thread_get_mutex_priority(struct rt_thread* thread)
{
    rt_list_t *node = RT_NULL;
    struct rt_mutex *mutex = RT_NULL;
    rt_uint8_t priority = rt_sched_thread_get_init_prio(thread);

    rt_list_for_each(node, &(thread->taken_object_list))
    {
        mutex = rt_list_entry(node, struct rt_mutex, taken_list);
        rt_uint8_t mutex_prio = mutex->priority;
        /* 锁对所有者施加的优先级至少达到其天花板。 */
        mutex_prio = mutex_prio < mutex->ceiling_priority ? mutex_prio : mutex->ceiling_priority;

        if (priority > mutex_prio)
        {
            priority = mutex_prio;
        }
    }

    return priority;
}

/**
 * @brief 修改目标线程优先级，并沿嵌套互斥等待链向上游所有者传播。
 *
 * 若目标线程正挂起在另一把互斥量上，优先级改变后必须先从该锁等待链摘下并
 * 按 PRIO 重新插入；随后更新该锁缓存的最高等待优先级，并按需要提升其 owner。
 * 循环直到线程未挂在互斥量上、上游优先级无需改变或调度操作失败。
 * 调用者应已持有调度器锁；`suspend_flag` 是为等待策略保留的参数，当前实现
 * 没有在函数体内读取它。
 */
rt_inline void _thread_update_priority(struct rt_thread *thread, rt_uint8_t priority, int suspend_flag)
{
    rt_err_t ret = -RT_ERROR;
    struct rt_object* pending_obj = RT_NULL;

    LOG_D("thread:%s priority -> %d", thread->parent.name, priority);

    /* 先改变当前目标；成功且它仍挂起时才继续沿 pending_object 传播。 */
    ret = rt_sched_thread_change_priority(thread, priority);

    while ((ret == RT_EOK) && rt_sched_thread_is_suspended(thread))
    {
        /* pending_object 只在互斥等待期间参与优先级继承传播。 */
        pending_obj = thread->pending_object;

        if (pending_obj && rt_object_get_type(pending_obj) == RT_Object_Class_Mutex)
        {
            rt_uint8_t mutex_priority = 0xff;
            struct rt_mutex* pending_mutex = (struct rt_mutex *)pending_obj;

            /* 有效优先级改变后，旧链表位置不再有序，必须摘下并重新插入。 */
            rt_list_remove(&RT_THREAD_LIST_NODE(thread));

            ret = rt_susp_list_enqueue(
                &(pending_mutex->parent.suspend_thread), thread,
                pending_mutex->parent.parent.flag);
            if (ret == RT_EOK)
            {
                /* 链首可能变化，刷新互斥量的最高等待优先级缓存。 */
                _mutex_update_priority(pending_mutex);
                /* 若 owner 还不够高，则下一轮继续向该 owner 及其上游传播。 */
                LOG_D("mutex: %s priority -> %d", pending_mutex->parent.parent.name,
                        pending_mutex->priority);

                mutex_priority = _thread_get_mutex_priority(pending_mutex->owner);
                if (mutex_priority != rt_sched_thread_get_curr_prio(pending_mutex->owner))
                {
                    thread = pending_mutex->owner;

                    ret = rt_sched_thread_change_priority(thread, mutex_priority);
                }
                else
                {
                    ret = -RT_ERROR;
                }
            }
        }
        else
        {
            ret = -RT_ERROR;
        }
    }
}

/**
 * @brief 释放/移除一把互斥量后，按需重算线程优先级并报告是否应重调度。
 *
 * 只有该锁启用了天花板，或线程当前优先级恰好等于该锁缓存的最高等待优先级
 * 时，这把锁才可能是当前提升来源，需要扫描线程剩余持锁。调用者必须持有
 * 调度器锁；返回 true 只表示解锁时应检查调度，并不在本函数内切换上下文。
 */
static rt_bool_t _check_and_update_prio(rt_thread_t thread, rt_mutex_t mutex)
{
    RT_SCHED_DEBUG_IS_LOCKED;
    rt_bool_t do_sched = RT_FALSE;

    if ((mutex->ceiling_priority != 0xFF) || (rt_sched_thread_get_curr_prio(thread) == mutex->priority))
    {
        rt_uint8_t priority = 0xff;

        /* 释放一把锁后，从剩余持锁集合和初始优先级重新计算。 */
        priority = _thread_get_mutex_priority(thread);

        rt_sched_thread_change_priority(thread, priority);

        /**
         * 优先级可能降低，标记调用者需要在解开调度器锁时检查重调度；此处仍
         * 持调度器锁，不会立即发生上下文切换。
         */
        do_sched = RT_TRUE;
    }
    return do_sched;
}

/**
 * @brief detach/delete 前清理互斥量等待者、所有者链和继承优先级。
 *
 * 先持互斥量锁，以 `RT_ERROR` 唤醒全部等待者；再持调度器锁把互斥量从 owner
 * 的 `taken_object_list` 摘下并重新计算 owner 优先级。若优先级变化需要调度，
 * 使用带重调度请求的解锁形式。全过程仍持互斥量锁，最后才释放。
 */
static void _mutex_before_delete_detach(rt_mutex_t mutex)
{
    rt_sched_lock_level_t slvl;
    rt_bool_t need_schedule = RT_FALSE;

    rt_spin_lock(&(mutex->spinlock));
    /* 销毁使所有未完成的 take 以错误结束。 */
    rt_susp_list_resume_all(&(mutex->parent.suspend_thread), RT_ERROR);

    rt_sched_lock(&slvl);

    /* taken_list 即便从未被 owner 持有也已初始化为自环，可安全 remove。 */
    rt_list_remove(&mutex->taken_list);

    /* 仍记录 owner 时，撤销这把锁施加的继承或天花板优先级。 */
    if (mutex->owner)
    {
        need_schedule = _check_and_update_prio(mutex->owner, mutex);
    }

    if (need_schedule)
    {
        rt_sched_unlock_n_resched(slvl);
    }
    else
    {
        rt_sched_unlock(slvl);
    }

    /* 最后释放互斥量私有锁。 */
    rt_spin_unlock(&(mutex->spinlock));
}

/** @} */

/**
 * @addtogroup group_mutex Mutex
 * @{
 */

/**
 * @brief 初始化一个可递归获取的静态互斥量。
 *
 * 初始 owner 为空、递归层数 `hold` 为 0、最高等待优先级和天花板均为 0xff。
 * 等待策略强制为 PRIO，因为 FIFO 无法为优先级继承提供有序的最高等待者。
 *
 * @see      rt_mutex_create()
 *
 * @param mutex 调用者提供且长期有效的互斥量存储。
 *
 * @param name 对象名称。
 *
 * @param flag 已废弃，仅为 API 兼容而保留，实际总是使用 `RT_IPC_FLAG_PRIO`。
 *
 * @return 成功返回 `RT_EOK`。
 *
 * @warning 只能在线程上下文且没有并发使用者时调用；结束时用 `rt_mutex_detach()`。
 */
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag)
{
    /* flag 只为兼容旧调用者保留，不参与行为选择。 */
    RT_UNUSED(flag);

    /* 调用者必须提供真实结构体存储。 */
    RT_ASSERT(mutex != RT_NULL);

    /* 登记为静态 Mutex 对象。 */
    rt_object_init(&(mutex->parent.parent), RT_Object_Class_Mutex, name);

    /* 初始化公共等待链和互斥量私有运行状态。 */
    _ipc_object_init(&(mutex->parent));

    mutex->owner    = RT_NULL;
    mutex->priority = 0xFF;
    mutex->hold     = 0;
    mutex->ceiling_priority = 0xFF;
    rt_list_init(&(mutex->taken_list));

    /* 互斥等待链固定按优先级排序，供继承算法快速读取链首。 */
    mutex->parent.parent.flag = RT_IPC_FLAG_PRIO;
    rt_spin_lock_init(&(mutex->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_init);


/**
 * @brief 清理并注销静态互斥量，但不释放其存储。
 *
 * 公共清理会让等待者以错误醒来、从原 owner 持锁链摘除对象，并撤销相应的
 * 优先级继承/天花板效果，随后从对象系统注销。调用者应先确保不会再并发使用。
 *
 * @see      rt_mutex_delete()
 *
 * @param mutex 由 `rt_mutex_init()` 初始化的静态互斥量。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 动态互斥量必须使用 `rt_mutex_delete()`。
 */
rt_err_t rt_mutex_detach(rt_mutex_t mutex)
{
    /* 验证对象类型和静态生命周期。 */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);
    RT_ASSERT(rt_object_is_systemobject(&mutex->parent.parent));

    _mutex_before_delete_detach(mutex);

    /* 只注销通用对象，不释放调用者存储。 */
    rt_object_detach(&(mutex->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_detach);

/**
 * @brief 因超时/信号等原因把指定线程从互斥量等待链移除，并回退优先级继承。
 *
 * 函数验证 `thread->pending_object` 确实指向该锁，在互斥量锁和调度器锁保护下
 * 摘除等待节点，重算 `mutex->priority`。若被移除者正是促使 owner 提升到当前
 * 优先级的等待者，还会从 owner 所持全部锁重新计算优先级并沿嵌套链传播。
 * 线程的 ready 状态和 error 由发起 drop 的超时/信号路径负责。
 *
 * @param mutex 线程原先等待的互斥量。
 * @param thread 要从该等待链移除的线程。
 * @warning 只能在线程上下文调用。
 */
void rt_mutex_drop_thread(rt_mutex_t mutex, rt_thread_t thread)
{
    rt_uint8_t priority;
    rt_bool_t need_update = RT_FALSE;
    rt_sched_lock_level_t slvl;

    /* pending_object 是互斥等待关系的真实性检查。 */
    RT_DEBUG_IN_THREAD_CONTEXT;
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(thread != RT_NULL);

    rt_spin_lock(&(mutex->spinlock));

    RT_ASSERT(thread->pending_object == &mutex->parent.parent);

    rt_sched_lock(&slvl);

    /* 调度器锁保证链表与线程调度状态同步更新。 */
    rt_list_remove(&RT_THREAD_LIST_NODE(thread));

    /**
     * 若 owner 当前优先级与离队线程相同，它可能正是继承来源，需要重算。
     * 并发释放可能已把 owner 清为 NULL；该释放路径也已经恢复优先级，此处跳过。
     */
    if (mutex->owner && rt_sched_thread_get_curr_prio(mutex->owner) ==
                            rt_sched_thread_get_curr_prio(thread))
    {
        need_update = RT_TRUE;
    }

    /* 从新的链首更新这把锁的最高等待优先级。 */
    if (!rt_list_isempty(&mutex->parent.suspend_thread))
    {
        /* 仍有等待者，PRIO 链首即最高优先级。 */
        struct rt_thread *th;

        th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
        /* 缓存链首优先级，供 owner 的综合优先级计算。 */
        mutex->priority = rt_sched_thread_get_curr_prio(th);
    }
    else
    {
        /* 0xff 哨兵表示已经没有等待者。 */
        mutex->priority = 0xff;
    }

    /* 撤销离队等待者不再需要的继承，并处理可能的嵌套传播。 */
    if (need_update)
    {
        /* 综合 owner 的初始优先级和它仍持有的所有互斥量。 */
        priority = _thread_get_mutex_priority(mutex->owner);
        if (priority != rt_sched_thread_get_curr_prio(mutex->owner))
        {
            _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE);
        }
    }

    rt_sched_unlock(slvl);
    rt_spin_unlock(&(mutex->spinlock));
}


/**
 * @brief 设置互斥量优先级天花板，并立即更新当前 owner 的有效优先级。
 *
 * 数值越小优先级越高。若互斥量已有 owner，函数在互斥量锁和调度器锁保护下
 * 重新计算 owner，必要时沿嵌套等待链传播。非法参数不修改对象，而把全局/线程
 * errno 设为 `-RT_EINVAL`。
 *
 * @param mutex 目标互斥量。
 * @param priority 新天花板，必须小于 `RT_THREAD_PRIORITY_MAX`；0xff 用作内部
 *                 “未设置”值，不能通过合法优先级分支设置。
 * @return 成功返回旧天花板；参数非法返回 0xff。
 */
rt_uint8_t rt_mutex_setprioceiling(rt_mutex_t mutex, rt_uint8_t priority)
{
    rt_uint8_t ret_priority = 0xFF;
    rt_uint8_t highest_prio;
    rt_sched_lock_level_t slvl;

    RT_DEBUG_IN_THREAD_CONTEXT;

    if ((mutex) && (priority < RT_THREAD_PRIORITY_MAX))
    {
        /* 私有锁串行化同一互斥量的并发天花板更新。 */
        rt_spin_lock(&(mutex->spinlock));
        ret_priority = mutex->ceiling_priority;
        mutex->ceiling_priority = priority;
        if (mutex->owner)
        {
            rt_sched_lock(&slvl);
            highest_prio = _thread_get_mutex_priority(mutex->owner);
            if (highest_prio != rt_sched_thread_get_curr_prio(mutex->owner))
            {
                _thread_update_priority(mutex->owner, highest_prio, RT_UNINTERRUPTIBLE);
            }
            rt_sched_unlock(slvl);
        }
        rt_spin_unlock(&(mutex->spinlock));
    }
    else
    {
        rt_set_errno(-RT_EINVAL);
    }

    return ret_priority;
}
RTM_EXPORT(rt_mutex_setprioceiling);


/**
 * @brief 在互斥量锁保护下读取当前优先级天花板。
 *
 * @param mutex 非空目标互斥量。
 *
 * @return 当前天花板；默认 0xff 表示未启用。
 */
rt_uint8_t rt_mutex_getprioceiling(rt_mutex_t mutex)
{
    rt_uint8_t prio = 0xFF;

    /* 该 API 明确要求线程上下文。 */
    RT_DEBUG_IN_THREAD_CONTEXT;
    RT_ASSERT(mutex != RT_NULL);

    if (mutex)
    {
        rt_spin_lock(&(mutex->spinlock));
        prio = mutex->ceiling_priority;
        rt_spin_unlock(&(mutex->spinlock));
    }

    return prio;
}
RTM_EXPORT(rt_mutex_getprioceiling);


#ifdef RT_USING_HEAP
/**
 * @brief 从内核堆创建一个动态递归互斥量。
 *
 * 私有字段初值和优先级协议与 `rt_mutex_init()` 相同，最终用
 * `rt_mutex_delete()` 清理和释放。
 *
 * @see      rt_mutex_init()
 *
 * @param name 对象名称。
 *
 * @param flag 已废弃，实际总是 PRIO 等待，仅为源代码兼容保留。
 *
 * @return 成功返回互斥量；堆内存不足返回 `RT_NULL`。
 *
 * @warning 只能在线程上下文调用。
 */
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag)
{
    struct rt_mutex *mutex;

    /* 兼容参数，不影响实际等待顺序。 */
    RT_UNUSED(flag);

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 分配并登记动态 Mutex 对象。 */
    mutex = (rt_mutex_t)rt_object_allocate(RT_Object_Class_Mutex, name);
    if (mutex == RT_NULL)
        return mutex;

    /* 初始化公共等待链以及 owner/递归/优先级状态。 */
    _ipc_object_init(&(mutex->parent));

    mutex->owner    = RT_NULL;
    mutex->priority = 0xFF;
    mutex->hold     = 0;
    mutex->ceiling_priority = 0xFF;
    rt_list_init(&(mutex->taken_list));

    /* 只允许 PRIO，避免无法确定最高优先级等待者的无界优先级反转。 */
    mutex->parent.parent.flag = RT_IPC_FLAG_PRIO;
    rt_spin_lock_init(&(mutex->spinlock));

    return mutex;
}
RTM_EXPORT(rt_mutex_create);


/**
 * @brief 清理并释放动态互斥量。
 *
 * 删除前会唤醒等待者、从 owner 持锁链摘除并恢复 owner 优先级，然后注销对象
 * 并释放结构体。调用者必须保证不存在新的并发操作。
 *
 * @see      rt_mutex_detach()
 *
 * @param mutex 由 `rt_mutex_create()` 创建的动态互斥量。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 静态对象必须调用 `rt_mutex_detach()`。
 */
rt_err_t rt_mutex_delete(rt_mutex_t mutex)
{
    /* 验证动态生命周期后再执行公共清理。 */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);
    RT_ASSERT(rt_object_is_systemobject(&mutex->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    _mutex_before_delete_detach(mutex);

    /* 注销并释放动态通用对象。 */
    rt_object_delete(&(mutex->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief 获取互斥量的内部实现，包含递归、等待和优先级继承完整流程。
 *
 * 三条主要路径：
 *
 * - 当前线程已经是 owner：只增加递归计数 `hold`；达到上限则失败。
 * - 没有 owner：建立所有权、把锁挂到线程 `taken_object_list`，并按天花板提升线程。
 * - 被其他线程持有：timeout 为 0 时立即失败，否则挂入 PRIO 等待链，设置
 *   `pending_object`，把自己的优先级继承给 owner（可沿嵌套锁传播），有限等待
 *   再启动线程定时器，然后解锁调度。
 *
 * 醒来后重新获取互斥量锁。若 release 已把 owner 直接移交给当前线程，则成功；
 * 否则说明超时、信号或异常唤醒，需要更新该锁及原 owner 的继承优先级，清除
 * `pending_object` 并返回错误。try-take hook 在持互斥量锁时调用；成功 take hook
 * 在释放互斥量锁后调用。
 *
 * @see      rt_mutex_trytake()
 *
 * @param mutex 目标互斥量。
 *
 * @param timeout 0 表示不等待，正数表示有限 tick，`RT_WAITING_FOREVER` 表示
 *                永久等待。永久等待常用于互斥量，但仍可能被所选信号模式中断。
 * @param suspend_flag 不可中断、普通信号可中断或仅致命信号可中断的等待模式。
 *
 * @return 成功获得（含递归获得）返回 `RT_EOK`；非阻塞/定时超时返回
 *         `-RT_ETIMEOUT`；递归计数溢出返回 `-RT_EFULL`；也可能返回挂起或信号
 *         路径的其他负错误码。
 *
 * @warning 互斥量有线程所有权，只能在线程上下文调用，即使 timeout 为 0。
 */
static rt_err_t _rt_mutex_take(rt_mutex_t mutex, rt_int32_t timeout, int suspend_flag)
{
    struct rt_thread *thread;
    rt_err_t ret;

    /* 互斥所有权和优先级继承都依赖当前线程及可用的调度器。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE);

    /* 验证对象类，避免破坏其他 IPC 对象布局。 */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);

    /* 此线程将成为 owner，或作为等待者参与优先级继承。 */
    thread = rt_thread_self();

    rt_spin_lock(&(mutex->spinlock));

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mutex->parent.parent)));

    LOG_D("mutex_take: current thread %s, hold: %d",
          thread->parent.name, mutex->hold);

    /* 正常 handoff 不必再次写入；默认 EOK 即代表成功取得。 */
    thread->error = RT_EOK;

    if (mutex->owner == thread)
    {
        if (mutex->hold < RT_MUTEX_HOLD_MAX)
        {
            /* 递归互斥量允许 owner 再次获取，并记录需要匹配的 release 次数。 */
            mutex->hold ++;
        }
        else
        {
            rt_spin_unlock(&(mutex->spinlock));
            return -RT_EFULL; /* 递归层数达到类型上限。 */
        }
    }
    else
    {
        /* 非递归路径先判断锁是否空闲。 */
        if (mutex->owner == RT_NULL)
        {
            /* 快速取得：建立 owner、首层 hold，并清空等待优先级缓存。 */
            mutex->owner    = thread;
            mutex->priority = 0xff;
            mutex->hold     = 1;

            if (mutex->ceiling_priority != 0xFF)
            {
                /* 天花板高于当前优先级时立即提升 owner。 */
                if (mutex->ceiling_priority < rt_sched_thread_get_curr_prio(mutex->owner))
                    _thread_update_priority(mutex->owner, mutex->ceiling_priority, suspend_flag);
            }

            /* owner 后续释放时通过该链综合恢复有效优先级。 */
            rt_list_insert_after(&thread->taken_object_list, &mutex->taken_list);
        }
        else
        {
            /* 锁忙且调用者要求非阻塞。 */
            if (timeout == 0)
            {
                /* 同时写线程错误字段，保持等待 API 的诊断状态一致。 */
                thread->error = RT_ETIMEOUT;

                rt_spin_unlock(&(mutex->spinlock));
                return -RT_ETIMEOUT;
            }
            else
            {
                rt_sched_lock_level_t slvl;
                rt_uint8_t priority;

                /* 慢路径：仍持互斥量锁，避免 owner 在挂起登记前释放而丢失唤醒。 */
                LOG_D("mutex_take: suspend thread: %s",
                      thread->parent.name);

                /* 按固定 PRIO 策略把当前线程变为挂起并加入等待链。 */
                ret = rt_thread_suspend_to_list(thread, &(mutex->parent.suspend_thread),
                                                mutex->parent.parent.flag, suspend_flag);
                if (ret != RT_EOK)
                {
                    rt_spin_unlock(&(mutex->spinlock));
                    return ret;
                }

                /* 记录依赖边，供信号移除和传递式优先级继承使用。 */
                thread->pending_object = &(mutex->parent.parent);

                rt_sched_lock(&slvl);

                priority = rt_sched_thread_get_curr_prio(thread);

                /* 新等待者若成为最高优先级，就更新缓存并提升 owner。 */
                if (priority < mutex->priority)
                {
                    mutex->priority = priority;
                    if (mutex->priority < rt_sched_thread_get_curr_prio(mutex->owner))
                    {
                        _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE); /* 待办：确认嵌套继承的等待模式传播策略。 */
                    }
                }

                rt_sched_unlock(slvl);

                /* 有限等待使用线程内置定时器；永久等待不启动它。 */
                if (timeout > 0)
                {
                    rt_tick_t timeout_tick = timeout;
                    LOG_D("mutex_take: start the timer of thread:%s",
                          thread->parent.name);

                    /* 超时回调会尝试把线程从等待状态转为 ready。 */
                    rt_timer_control(&(thread->thread_timer),
                                     RT_TIMER_CTRL_SET_TIME,
                                     &timeout_tick);
                    rt_timer_start(&(thread->thread_timer));
                }

                rt_spin_unlock(&(mutex->spinlock));

                /* 已登记完成并解开对象锁，现在安全地让出 CPU。 */
                rt_schedule();

                rt_spin_lock(&(mutex->spinlock));

                if (mutex->owner == thread)
                {
                    /**
                     * release 路径已经把 owner 直接移交给本线程；正常 handoff 应
                     * 保持 thread->error 为 EOK，断言可捕获矛盾的异步唤醒。
                     */
                    RT_ASSERT(thread->error == RT_EOK);
                }
                else
                {
                    /* 未取得所有权：线程因超时、信号或其他原因离开了等待链。 */

                    rt_bool_t need_update = RT_FALSE;
                    RT_ASSERT(mutex->owner != thread);

                    /* 在调用可能改变线程状态的其他 API 前先保存醒来原因。 */
                    ret = thread->error;

                    /* error 仍为 EOK 却未成为 owner，按意外中断处理。 */
                    if (ret == RT_EOK)
                    {
                        ret = -RT_EINTR;
                    }

                    rt_sched_lock(&slvl);

                    /**
                     * 若原 owner 的当前优先级正等于离队线程，它可能失去继承来源，
                     * 需要重算。owner 也可能已并发释放并清为 NULL，那条路径已经
                     * 恢复优先级，此处可跳过。
                     */
                    if (mutex->owner && rt_sched_thread_get_curr_prio(mutex->owner) == rt_sched_thread_get_curr_prio(thread))
                        need_update = RT_TRUE;

                    /* 从剩余等待链刷新本锁的最高等待优先级。 */
                    if (!rt_list_isempty(&mutex->parent.suspend_thread))
                    {
                        /* PRIO 链首是新的最高优先级等待者。 */
                        struct rt_thread *th;

                        th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
                        /* 缓存供 owner 综合计算。 */
                        mutex->priority = rt_sched_thread_get_curr_prio(th);
                    }
                    else
                    {
                        /* 已无人等待。 */
                        mutex->priority = 0xff;
                    }

                    /* 撤销已经不需要的继承，并按需向嵌套链传播。 */
                    if (need_update)
                    {
                        /* 从 owner 的初始值和全部持锁重新计算。 */
                        priority = _thread_get_mutex_priority(mutex->owner);
                        if (priority != rt_sched_thread_get_curr_prio(mutex->owner))
                        {
                            _thread_update_priority(mutex->owner, priority, RT_UNINTERRUPTIBLE);
                        }
                    }

                    rt_sched_unlock(slvl);

                    rt_spin_unlock(&(mutex->spinlock));

                    /* 本线程已不再等待此锁，移除依赖边。 */
                    thread->pending_object = RT_NULL;

                    /* 内部可能保存正错误码，公开 API 统一返回负值。 */
                    return ret > 0 ? -ret : ret;
                }
            }
        }
    }

    rt_spin_unlock(&(mutex->spinlock));

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mutex->parent.parent)));

    return RT_EOK;
}

/**
 * @brief 以不可被信号打断的模式获取互斥量。
 * @param mutex 目标互斥量。
 * @param time 0、有限 tick 或 `RT_WAITING_FOREVER`。
 * @return 语义见 `_rt_mutex_take()`。
 */
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mutex_take);

/** @brief 以可被普通信号中断的方式获取互斥量；其余语义同 `rt_mutex_take()`。 */
rt_err_t rt_mutex_take_interruptible(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mutex_take_interruptible);

/** @brief 以仅可被致命信号中断的方式获取互斥量；其余语义同 `rt_mutex_take()`。 */
rt_err_t rt_mutex_take_killable(rt_mutex_t mutex, rt_int32_t time)
{
    return _rt_mutex_take(mutex, time, RT_KILLABLE);
}
RTM_EXPORT(rt_mutex_take_killable);

/**
 * @brief 非阻塞地尝试获取互斥量。
 *
 * 等价于 `rt_mutex_take(mutex, RT_WAITING_NO)`。当前线程已是 owner 时仍按递归
 * 获取成功；被其他线程持有时立即返回 `-RT_ETIMEOUT`。
 *
 * @see      rt_mutex_take()
 *
 * @param mutex 目标互斥量。
 *
 * @return 成功返回 `RT_EOK`，失败返回负错误码。
 */
rt_err_t rt_mutex_trytake(rt_mutex_t mutex)
{
    return rt_mutex_take(mutex, RT_WAITING_NO);
}
RTM_EXPORT(rt_mutex_trytake);


/**
 * @brief 释放当前线程持有的一层互斥量，必要时直接把所有权移交给等待者。
 *
 * 正常情况下只有 owner 可以释放；唯一例外是 owner 已进入 CLOSE 状态时，清理
 * 路径可由其他线程代为释放遗留锁。put hook 在持互斥量锁后、所有权检查之前
 * 调用，因此即使非法释放最终失败，hook 也已收到一次通知。
 *
 * `hold` 减一后仍非零表示只退出一层递归，owner 不变。降到零时：从旧 owner
 * 持锁链移除并恢复其优先级；从 PRIO 等待链寻找第一个仍能转为 ready 的线程
 * （已经被超时路径抢先处理的节点会跳过）；若找到，则在调度器锁内直接设置为
 * 新 owner、hold=1、加入其持锁链并清除 pending_object。这样醒来线程已经拥有
 * 锁，无需再次竞争。全部状态更新完成并释放锁后才执行调度。
 *
 * @param mutex 要释放的互斥量。
 *
 * @return 成功返回 `RT_EOK`；非 owner 且 owner 尚未关闭时返回 `-RT_ERROR`。
 */
rt_err_t rt_mutex_release(rt_mutex_t mutex)
{
    rt_sched_lock_level_t slvl;
    struct rt_thread *thread;
    rt_bool_t need_schedule;

    /* 类型验证不能替代所有权验证，所有权在锁内检查。 */
    RT_ASSERT(mutex != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mutex->parent.parent) == RT_Object_Class_Mutex);

    need_schedule = RT_FALSE;

    /* 互斥量和线程身份绑定，禁止中断上下文释放。 */
    RT_DEBUG_IN_THREAD_CONTEXT;

    /* 正常情况下它必须与 mutex->owner 相同。 */
    thread = rt_thread_self();

    rt_spin_lock(&(mutex->spinlock));

    LOG_D("mutex_release:current thread %s, hold: %d",
          thread->parent.name, mutex->hold);

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mutex->parent.parent)));

    /*
     * 通常只有 owner 可释放。若 owner 已经 CLOSED，允许线程删除清理路径代为
     * 释放孤儿互斥量，例如跨线程 `rt_thread_delete()` 的锁清理。
     */
    if (thread != mutex->owner)
    {
        rt_bool_t owner_closed = RT_FALSE;

        if (mutex->owner != RT_NULL)
        {
            rt_sched_lock(&slvl);
            owner_closed =
                (rt_sched_thread_get_stat(mutex->owner) == RT_THREAD_CLOSE);
            rt_sched_unlock(slvl);
        }

        if (!owner_closed)
        {
            thread->error = -RT_ERROR;
            rt_spin_unlock(&(mutex->spinlock));
            return -RT_ERROR;
        }
    }

    /* 每次 release 只匹配一次递归 take。 */
    mutex->hold --;
    /* 最后一层释放才撤销所有权、恢复优先级并处理等待者。 */
    if (mutex->hold == 0)
    {
        /* CLOSED owner 代释放场景中，恢复的仍必须是原 owner，而不是当前调用者。 */
        struct rt_thread *owner = mutex->owner;

        rt_sched_lock(&slvl);

        /* 旧 owner 不再持有该锁。 */
        rt_list_remove(&mutex->taken_list);

        /* 撤销本锁带来的继承/天花板，可能产生重调度需求。 */
        need_schedule = _check_and_update_prio(owner, mutex);

        /* 有等待者时尝试把所有权直接交给链首。 */
        if (!rt_list_isempty(&mutex->parent.suspend_thread))
        {
            struct rt_thread *next_thread;
            do
            {
                /* PRIO 链首代表当前最应获得锁的线程。 */
                next_thread = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);

                RT_ASSERT(rt_sched_thread_is_suspended(next_thread));

                /* 与 ready 操作一起置于调度器锁临界区。 */
                rt_list_remove(&RT_THREAD_LIST_NODE(next_thread));

                /* 若超时已经赢得竞争，ready 会失败，继续尝试下一等待者。 */
                if (rt_sched_thread_ready(next_thread) != RT_EOK)
                {
                    /**
                     * 超时定时器可能刚刚先处理了这个线程；跳过它并继续寻找仍
                     * 真正挂起的下一个等待者。
                     */
                    next_thread = RT_NULL;
                }
            } while (!next_thread && !rt_list_isempty(&mutex->parent.suspend_thread));

            if (next_thread)
            {
                LOG_D("mutex_release: resume thread: %s",
                    next_thread->parent.name);

                /* handoff：在唤醒线程实际运行前就完成所有权和递归层数设置。 */
                mutex->owner = next_thread;
                mutex->hold  = 1;
                rt_list_insert_after(&next_thread->taken_object_list, &mutex->taken_list);

                /* 新 owner 已不再等待该锁，清除依赖边。 */
                next_thread->pending_object = RT_NULL;

                /* 缓存交接后剩余等待链的最高优先级。 */
                if (!rt_list_isempty(&(mutex->parent.suspend_thread)))
                {
                    struct rt_thread *th;

                    th = RT_THREAD_LIST_NODE_ENTRY(mutex->parent.suspend_thread.next);
                    mutex->priority = rt_sched_thread_get_curr_prio(th);
                }
                else
                {
                    mutex->priority = 0xff;
                }

                need_schedule = RT_TRUE;
            }
            else
            {
                /* 所有候选均已被异步处理，互斥量变为空闲。 */
                mutex->owner = RT_NULL;
                mutex->priority = 0xff;
            }

            rt_sched_unlock(slvl);
        }
        else
        {
            rt_sched_unlock(slvl);

            /* 没有等待者，直接进入无 owner 状态。 */
            mutex->owner    = RT_NULL;
            mutex->priority = 0xff;
        }
    }

    rt_spin_unlock(&(mutex->spinlock));

    /* 解开互斥量锁后再响应优先级变化或新就绪线程。 */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK;
}
RTM_EXPORT(rt_mutex_release);


/**
 * @brief 互斥量通用 control 占位接口。
 *
 * 当前没有任何支持的命令，三个参数均被显式标记为未使用。
 *
 * @param mutex 保留的互斥量参数。
 *
 * @param cmd 保留的控制命令。
 *
 * @param arg 保留的命令参数。
 *
 * @return 固定返回 `-RT_EINVAL`。
 */
rt_err_t rt_mutex_control(rt_mutex_t mutex, int cmd, void *arg)
{
    RT_UNUSED(mutex);
    RT_UNUSED(cmd);
    RT_UNUSED(arg);

    return -RT_EINVAL;
}
RTM_EXPORT(rt_mutex_control);

/**@}*/
#endif /* RT_USING_MUTEX */

#ifdef RT_USING_EVENT
/**
 * @addtogroup group_event Event
 * @{
 */

/**
 * @brief 初始化一个静态事件对象，初始事件位集合为 0。
 *
 * 一个事件对象保存 32 个可独立置位的条件。等待线程可要求“任意一位(OR)”或
 * “全部位(AND)”满足；对象的 `flag` 只决定多个未满足线程在等待链中的顺序。
 * 静态对象最终用 `rt_event_detach()`。
 *
 * @see      rt_event_create()
 *
 * @param event 调用者提供的事件结构体。
 *
 * @param name 对象名称。
 *
 * @param flag `RT_IPC_FLAG_PRIO` 按优先级或 `RT_IPC_FLAG_FIFO` 按到达顺序排队。
 *
 * @return 成功返回 `RT_EOK`。
 *
 * @warning 只能在线程上下文、无并发用户时初始化。
 */
rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag)
{
    /* 只接受公共等待链支持的两种排序策略。 */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* 登记静态 Event 对象。 */
    rt_object_init(&(event->parent.parent), RT_Object_Class_Event, name);

    /* 保存等待线程排序策略。 */
    event->parent.parent.flag = flag;

    /* 建立空等待链。 */
    _ipc_object_init(&(event->parent));

    /* 初始没有任何事件位发生。 */
    event->set = 0;
    rt_spin_lock_init(&(event->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_event_init);


/**
 * @brief 注销静态事件对象，并让全部等待者以错误醒来。
 *
 * 等待链清理在事件锁内完成；随后只从对象系统注销，不释放调用者存储。
 *
 * @see      rt_event_delete()
 *
 * @param event 由 `rt_event_init()` 初始化的静态事件。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 动态事件必须使用 `rt_event_delete()`。
 */
rt_err_t rt_event_detach(rt_event_t event)
{
    rt_base_t level;

    /* 验证对象类和静态生命周期。 */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);
    RT_ASSERT(rt_object_is_systemobject(&event->parent.parent));

    level = rt_spin_lock_irqsave(&(event->spinlock));
    /* 销毁事件条件，旧等待者以 RT_ERROR 结束。 */
    rt_susp_list_resume_all(&(event->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(event->spinlock), level);

    /* 注销但不释放静态存储。 */
    rt_object_detach(&(event->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_event_detach);

#ifdef RT_USING_HEAP
/**
 * @brief 从内核堆创建初始位集合为 0 的动态事件。
 *
 * 行为与 `rt_event_init()` 相同，生命周期以 `rt_event_delete()` 结束。
 *
 * @see      rt_event_init()
 *
 * @param name 对象名称。
 *
 * @param flag `RT_IPC_FLAG_PRIO` 或 `RT_IPC_FLAG_FIFO`。
 *
 * @return 成功返回事件对象，堆内存不足返回 `RT_NULL`。
 *
 * @warning 只能在线程上下文调用。
 */
rt_event_t rt_event_create(const char *name, rt_uint8_t flag)
{
    rt_event_t event;

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 分配并登记动态 Event 对象。 */
    event = (rt_event_t)rt_object_allocate(RT_Object_Class_Event, name);
    if (event == RT_NULL)
        return event;

    /* 保存等待顺序。 */
    event->parent.parent.flag = flag;

    /* 建立空等待链。 */
    _ipc_object_init(&(event->parent));

    /* 事件位初始全为 0。 */
    event->set = 0;
    rt_spin_lock_init(&(event->spinlock));

    return event;
}
RTM_EXPORT(rt_event_create);


/**
 * @brief 删除动态事件、错误唤醒全部等待者并释放对象内存。
 *
 * 调用者必须先阻止新的并发发送/接收。等待者转为 ready 后对象会立即释放，醒来
 * 路径只能读取自身保存的错误信息，不能继续访问 event。
 *
 * @see      rt_event_detach()
 *
 * @param event 由 `rt_event_create()` 创建的动态事件。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 静态事件必须使用 `rt_event_detach()`。
 */
rt_err_t rt_event_delete(rt_event_t event)
{
    /* 验证动态生命周期。 */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);
    RT_ASSERT(rt_object_is_systemobject(&event->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    rt_spin_lock(&(event->spinlock));
    /* 先结束全部等待，再释放其链表所在结构体。 */
    rt_susp_list_resume_all(&(event->parent.suspend_thread), RT_ERROR);
    rt_spin_unlock(&(event->spinlock));

    /* 注销并释放动态对象。 */
    rt_object_delete(&(event->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_event_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief 原子置位事件集合，并唤醒所有条件因此满足的等待线程。
 *
 * 在事件锁内执行 `event->set |= set`，随后调用 put hook（仍持事件锁），再持调度器
 * 锁扫描整个等待链，而不是只看链首：不同线程等待的位和 AND/OR 条件可能不同。
 * AND 要求请求位全部存在；OR 只需任一位，并把 `thread->event_set` 缩小为实际
 * 命中的位。满足条件者直接转为 ready、error 设为 EOK。
 *
 * 带 CLEAR 的多个等待者仍会在本次扫描中看到相同的置位快照；函数先累计所有
 * 需要清除的位，遍历结束后一次性从对象位集合清除。因此一次 send 可能同时
 * 唤醒多个匹配者。对象锁和调度器锁释放后才执行调度。
 *
 * @param event 目标事件对象。
 *
 * @param set 要置 1 的位掩码，可按位或组合多个事件；0 被拒绝。
 *
 * @return 成功返回 `RT_EOK`；set 为 0 返回 `-RT_ERROR`；若等待者保存了非法
 *         AND/OR 选项则返回 `-RT_EINVAL`。
 */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set)
{
    struct rt_list_node *n;
    struct rt_thread *thread;
    rt_sched_lock_level_t slvl;
    rt_base_t level;
    rt_base_t status;
    rt_bool_t need_schedule;
    rt_uint32_t need_clear_set = 0;

    /* 事件类型和非零位集是基本前提。 */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);

    if (set == 0)
        return -RT_ERROR;

    need_schedule = RT_FALSE;

    level = rt_spin_lock_irqsave(&(event->spinlock));

    /* 事件采用“置位累积”语义，未清除的旧位继续保留。 */
    event->set |= set;

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(event->parent.parent)));

    rt_sched_lock(&slvl);
    if (!rt_list_isempty(&event->parent.suspend_thread))
    {
        /* 必须逐个检查，因为链表顺序与每个线程等待的位条件无关。 */
        n = event->parent.suspend_thread.next;
        while (n != &(event->parent.suspend_thread))
        {
            /* 从线程调度链节点还原线程对象。 */
            thread = RT_THREAD_LIST_NODE_ENTRY(n);

            status = -RT_ERROR;
            if (thread->event_info & RT_EVENT_FLAG_AND)
            {
                if ((thread->event_set & event->set) == thread->event_set)
                {
                    /* 请求的全部位都已经置位。 */
                    status = RT_EOK;
                }
            }
            else if (thread->event_info & RT_EVENT_FLAG_OR)
            {
                if (thread->event_set & event->set)
                {
                    /* OR 接收只向线程报告本次实际命中的请求位。 */
                    thread->event_set = thread->event_set & event->set;

                    /* 至少一个请求位已置位。 */
                    status = RT_EOK;
                }
            }
            else
            {
                rt_sched_unlock(slvl);
                rt_spin_unlock_irqrestore(&(event->spinlock), level);

                return -RT_EINVAL;
            }

            /* ready 会移除当前节点，所以必须提前保存下一节点。 */
            n = n->next;

            /* 条件满足时结束该线程的事件等待。 */
            if (status == RT_EOK)
            {
                /* 延迟到全链扫描完毕后统一清除，避免影响同批其他等待者。 */
                if (thread->event_info & RT_EVENT_FLAG_CLEAR)
                    need_clear_set |= thread->event_set;

                /* 转入就绪队列并标记为正常事件通知。 */
                rt_sched_thread_ready(thread);
                thread->error = RT_EOK;

                /* 解锁后需要让更高优先级的新就绪线程有机会运行。 */
                need_schedule = RT_TRUE;
            }
        }
        if (need_clear_set)
        {
            event->set &= ~need_clear_set;
        }
    }

    rt_sched_unlock(slvl);
    rt_spin_unlock_irqrestore(&(event->spinlock), level);

    /* 不在事件锁或调度器锁内切换上下文。 */
    if (need_schedule == RT_TRUE)
        rt_schedule();

    return RT_EOK;
}
RTM_EXPORT(rt_event_send);


/**
 * @brief 接收事件位的内部实现，支持 AND/OR、自动清除、超时和信号模式。
 *
 * try-take hook 在获取事件锁前调用。锁内先检查当前 `event->set`：AND 要求 set
 * 中所有请求位都存在，OR 要求至少一位。立即满足时保存实际命中位，按 CLEAR
 * 选择清位并成功返回。未满足且 timeout=0 则立即超时。
 *
 * 需要等待时，把请求掩码和 option 保存在当前线程的 `event_set/event_info`，
 * 然后在事件锁未释放时挂入等待链，有限等待再启动线程定时器。send 路径满足
 * 条件后会把实际位留在线程字段中并把 error 设为 EOK；醒来后本函数在事件锁
 * 内把它复制到 `recved`。成功 take hook 在所有锁释放后调用。
 *
 * @param event 目标事件对象。
 *
 * @param set 希望接收的非零位掩码。
 *
 * @param option 必须选择 `RT_EVENT_FLAG_OR` 或 `RT_EVENT_FLAG_AND` 之一，可再按位或
 *               `RT_EVENT_FLAG_CLEAR` 表示成功后消费相关位。
 *
 * @param timeout 0 不等待，正数有限等待，`RT_WAITING_FOREVER` 永久等待。
 *
 * @param recved 可选输出，成功时写入实际匹配到的位。
 * @param suspend_flag 等待可中断等级。
 *
 * @return 成功返回 `RT_EOK`；无位且不等待/等待到期返回 `-RT_ETIMEOUT`；也可能
 *         返回信号、销毁或挂起路径的错误。
 */
static rt_err_t _rt_event_recv(rt_event_t   event,
                               rt_uint32_t  set,
                               rt_uint8_t   option,
                               rt_int32_t   timeout,
                               rt_uint32_t *recved,
                               int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    rt_base_t status;
    rt_err_t ret;

    /* 仅合法 Event 对象可参与位集合操作。 */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);

    /* 接收可能挂起，必须有当前线程且调度器可用。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(RT_TRUE);

    if (set == 0)
        return -RT_ERROR;

    /* 默认视为条件未满足。 */
    status = -RT_ERROR;
    /* 等待条件暂存在当前线程字段中。 */
    thread = rt_thread_self();
    /* 默认中断错误会被立即成功或正常 send 覆盖。 */
    thread->error = -RT_EINTR;

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(event->parent.parent)));

    level = rt_spin_lock_irqsave(&(event->spinlock));

    /* 在对象锁内对位集合做条件检查，和并发 send/clear 保持原子。 */
    if (option & RT_EVENT_FLAG_AND)
    {
        if ((event->set & set) == set)
            status = RT_EOK;
    }
    else if (option & RT_EVENT_FLAG_OR)
    {
        if (event->set & set)
            status = RT_EOK;
    }
    else
    {
        /* AND/OR 必须且只能由调用者选择一种有效匹配模式。 */
        RT_ASSERT(0);
    }

    if (status == RT_EOK)
    {
        thread->error = RT_EOK;

        /* 对外报告请求掩码中当前实际为 1 的位。 */
        if (recved)
            *recved = (event->set & set);

        /* 即使没有睡眠也同步线程诊断字段。 */
        thread->event_set = (event->set & set);
        thread->event_info = option;

        /* CLEAR 在同一事件锁临界区消费请求位。 */
        if (option & RT_EVENT_FLAG_CLEAR)
            event->set &= ~set;
    }
    else if (timeout == 0)
    {
        /* 条件未满足且禁止睡眠。 */
        thread->error = -RT_ETIMEOUT;

        rt_spin_unlock_irqrestore(&(event->spinlock), level);

        return -RT_ETIMEOUT;
    }
    else
    {
        /* send 将从这两个字段读取每个等待者的条件。 */
        thread->event_set  = set;
        thread->event_info = option;

        /* 在仍持事件锁时完成挂起，避免错过紧邻发生的 send。 */
        ret = rt_thread_suspend_to_list(thread, &(event->parent.suspend_thread),
                                        event->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(event->spinlock), level);
            return ret;
        }

        /* 正 timeout 使用线程内置定时器；FOREVER 不设置截止时间。 */
        if (timeout > 0)
        {
            rt_tick_t timeout_tick = timeout;
            /* 到期路径与 send 竞争把线程转为 ready。 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(event->spinlock), level);

        /* 等待登记完毕并解锁后让出 CPU。 */
        rt_schedule();

        if (thread->error != RT_EOK)
        {
            /* 超时、信号或 reset/delete 的错误原样返回。 */
            return thread->error;
        }

        /* 正常 send 唤醒后重新锁定，稳定读取 thread->event_set。 */
        level = rt_spin_lock_irqsave(&(event->spinlock));

        /* send 已根据 OR/AND 语义准备好实际接收位。 */
        if (recved)
            *recved = thread->event_set;
    }

    rt_spin_unlock_irqrestore(&(event->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(event->parent.parent)));

    return thread->error;
}

/**
 * @brief 以不可被信号打断的模式等待事件条件。
 * @return 详细参数和返回语义见 `_rt_event_recv()`。
 */
rt_err_t rt_event_recv(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_event_recv);

/** @brief 可被普通信号中断的事件接收；其余语义同 `rt_event_recv()`。 */
rt_err_t rt_event_recv_interruptible(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_event_recv_interruptible);

/** @brief 仅可被致命信号中断的事件接收；其余语义同 `rt_event_recv()`。 */
rt_err_t rt_event_recv_killable(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   option,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved)
{
    return _rt_event_recv(event, set, option, timeout, recved, RT_KILLABLE);
}
RTM_EXPORT(rt_event_recv_killable);
/**
 * @brief 重置事件对象：清零全部事件位并错误唤醒全部等待者。
 *
 * 清理在事件锁内完成，解锁后主动调度。`arg` 在当前命令中未使用。
 *
 * @param event 目标事件对象。
 *
 * @param cmd 目前只支持 `RT_IPC_CMD_RESET`。
 *
 * @param arg 保留参数，当前忽略。
 *
 * @return reset 成功返回 `RT_EOK`，未知命令返回 `-RT_ERROR`。
 */
rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg)
{
    rt_base_t level;

    RT_UNUSED(arg);

    /* 只允许合法 Event 对象。 */
    RT_ASSERT(event != RT_NULL);
    RT_ASSERT(rt_object_get_type(&event->parent.parent) == RT_Object_Class_Event);

    if (cmd == RT_IPC_CMD_RESET)
    {
        level = rt_spin_lock_irqsave(&(event->spinlock));

        /* 旧等待条件全部失效，以 RT_ERROR 结束等待。 */
        rt_susp_list_resume_all(&event->parent.suspend_thread, RT_ERROR);

        /* 清除所有已经置位但尚未消费的事件。 */
        event->set = 0;

        rt_spin_unlock_irqrestore(&(event->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    return -RT_ERROR;
}
RTM_EXPORT(rt_event_control);

/**@}*/
#endif /* RT_USING_EVENT */

#ifdef RT_USING_MAILBOX
/**
 * @addtogroup group_mailbox MailBox
 * @{
 */

/**
 * @brief 用调用者提供的槽位数组初始化静态邮箱。
 *
 * 邮箱内部是 `rt_ubase_t` 环形数组：`in_offset` 指向下一写入槽，`out_offset`
 * 指向下一读取槽，`entry` 是当前邮件数。公共 `parent.suspend_thread` 保存等待
 * 数据的接收者，额外的 `suspend_sender_thread` 保存因邮箱已满而等待空位的
 * 发送者。静态邮箱不拥有 `msgpool`，调用者必须保证缓冲区一直有效。
 *
 * @see      rt_mb_create()
 *
 * @param mb 调用者提供的邮箱控制块。
 *
 * @param name 对象名称。
 *
 * @param msgpool 至少包含 `size` 个 `rt_ubase_t` 槽位的可写缓冲区。
 *
 * @param size 邮箱可容纳的邮件个数，不是字节数；内部保存为 16 位计数，调用者
 *             应保证取值可表示且缓冲区大小为 `size * sizeof(rt_ubase_t)`。
 *
 * @param flag 接收者和发送者等待链都采用的 PRIO 或 FIFO 排序策略。
 *
 * @return 成功返回 `RT_EOK`。
 *
 * @warning 只能在线程上下文、无并发访问时初始化；结束用 `rt_mb_detach()`。
 */
rt_err_t rt_mb_init(rt_mailbox_t mb,
                    const char  *name,
                    void        *msgpool,
                    rt_size_t    size,
                    rt_uint8_t   flag)
{
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* 登记静态 MailBox 对象。 */
    rt_object_init(&(mb->parent.parent), RT_Object_Class_MailBox, name);

    /* 两条等待链共用同一种排序策略。 */
    mb->parent.parent.flag = flag;

    /* 建立空的接收者等待链。 */
    _ipc_object_init(&(mb->parent));

    /* 保存外部槽位池，并把环形队列置为空。 */
    mb->msg_pool   = (rt_ubase_t *)msgpool;
    mb->size       = (rt_uint16_t)size;
    mb->entry      = 0;
    mb->in_offset  = 0;
    mb->out_offset = 0;

    /* 发送者需要独立等待链，因为“有数据”和“有空位”是相反条件。 */
    rt_list_init(&(mb->suspend_sender_thread));
    rt_spin_lock_init(&(mb->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mb_init);


/**
 * @brief 注销静态邮箱，并让接收者、发送者两条等待链都以错误醒来。
 *
 * 本函数不释放调用者提供的 `msgpool`。清理时调用者必须阻止新的并发收发。
 *
 * @see      rt_mb_delete()
 *
 * @param mb 由 `rt_mb_init()` 初始化的静态邮箱。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 动态邮箱必须使用 `rt_mb_delete()`。
 */
rt_err_t rt_mb_detach(rt_mailbox_t mb)
{
    rt_base_t level;

    /* 验证对象类与静态生命周期。 */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);
    RT_ASSERT(rt_object_is_systemobject(&mb->parent.parent));

    level = rt_spin_lock_irqsave(&(mb->spinlock));
    /* 接收等待者因邮箱被销毁而失败。 */
    rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);
    /* 满队列上的发送等待者也必须结束。 */
    rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    /* 只注销控制块；msg_pool 所有权仍属于调用者。 */
    rt_object_detach(&(mb->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mb_detach);

#ifdef RT_USING_HEAP
/**
 * @brief 从内核堆创建控制块和槽位池均由内核拥有的动态邮箱。
 *
 * 函数先分配邮箱对象，再分配 `size * sizeof(rt_ubase_t)` 字节的邮件池；第二次
 * 分配失败会回滚对象登记。成功对象最终用 `rt_mb_delete()`。
 *
 * @see    rt_mb_init()
 *
 * @param name 对象名称。
 *
 * @param size 邮件槽位数，内部转换为 16 位；调用者应避免截断和乘法溢出。
 *
 * @param flag PRIO 或 FIFO 等待策略。
 *
 * @return 两次分配均成功时返回邮箱；否则返回 `RT_NULL`。
 *
 * @warning 使用堆分配，只能在线程上下文调用。
 */
rt_mailbox_t rt_mb_create(const char *name, rt_size_t size, rt_uint8_t flag)
{
    rt_mailbox_t mb;

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 先分配并登记动态控制块。 */
    mb = (rt_mailbox_t)rt_object_allocate(RT_Object_Class_MailBox, name);
    if (mb == RT_NULL)
        return mb;

    /* 保存两类等待者的排序策略。 */
    mb->parent.parent.flag = flag;

    /* 初始化接收者等待链。 */
    _ipc_object_init(&(mb->parent));

    /* 分配邮箱自己拥有的槽位数组。 */
    mb->size     = (rt_uint16_t)size;
    mb->msg_pool = (rt_ubase_t *)RT_KERNEL_MALLOC(mb->size * sizeof(rt_ubase_t));
    if (mb->msg_pool == RT_NULL)
    {
        /* 槽位池失败，回滚已登记的控制块。 */
        rt_object_delete(&(mb->parent.parent));

        return RT_NULL;
    }
    mb->entry      = 0;
    mb->in_offset  = 0;
    mb->out_offset = 0;

    /* 初始化满队列时使用的发送者等待链。 */
    rt_list_init(&(mb->suspend_sender_thread));
    rt_spin_lock_init(&(mb->spinlock));

    return mb;
}
RTM_EXPORT(rt_mb_create);


/**
 * @brief 删除动态邮箱，结束全部等待并释放槽位池和控制块。
 *
 * 两条等待链都在邮箱锁内以错误唤醒；解锁后先释放池，再注销并释放对象。
 *
 * @see      rt_mb_detach()
 *
 * @param mb 由 `rt_mb_create()` 创建的动态邮箱。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 静态邮箱及其外部池必须使用 `rt_mb_detach()`，不能由本函数释放。
 */
rt_err_t rt_mb_delete(rt_mailbox_t mb)
{
    /* 验证对象类和动态生命周期。 */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);
    RT_ASSERT(rt_object_is_systemobject(&mb->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;
    rt_spin_lock(&(mb->spinlock));

    /* 结束等待数据的接收者。 */
    rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);

    /* 结束等待空位的发送者。 */
    rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);

    rt_spin_unlock(&(mb->spinlock));

    /* 动态 create 路径拥有槽位池。 */
    RT_KERNEL_FREE(mb->msg_pool);

    /* 注销并释放控制块。 */
    rt_object_delete(&(mb->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mb_delete);
#endif /* RT_USING_HEAP */


/**
 * @brief 向邮箱发送一个机器字；邮箱满时可等待空位。
 *
 * put hook 在获取邮箱锁之前调用，所以即使之后因满而失败也会被观察到。邮箱满
 * 且允许等待时，线程进入专用发送者等待链；有限等待使用线程定时器。被唤醒后
 * 会再次检查邮箱是否仍满，因为空位可能已被其他执行流占用；正 timeout 会减去
 * 每轮实际消耗 tick，保持总等待预算而不是每次重新计时。
 *
 * 有空位后在锁内写 `in_offset`，游标环绕并增加 entry。若有接收等待者，直接
 * 以 EOK 唤醒链首；解锁后调度。发送仅复制 `rt_ubase_t` 值，传指针时其指向
 * 数据的生命周期仍由应用管理。
 *
 * @see      rt_mb_send()
 *
 * @param mb 目标邮箱。
 *
 * @param value 要复制进一个邮箱槽位的机器字值。
 *
 * @param timeout 0 表示满时立即返回，正数为总等待 tick，FOREVER 为永久等待。
 * @param suspend_flag 等待的信号可中断等级。
 *
 * @return 成功返回 `RT_EOK`；非阻塞且满或计数达到实现上限返回 `-RT_EFULL`；
 *         等待还可能返回超时、信号或销毁错误。
 *
 * @warning 中断上下文只能使用 timeout=0 的非阻塞形式；会等待的形式仅限线程。
 */
static rt_err_t _rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout,
                         int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    rt_uint32_t tick_delta;
    rt_err_t ret;

    /* 验证对象类型。 */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    /* timeout 非零时可能调度，必须处于可睡眠线程上下文。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* tick_delta 用于扣除每轮实际等待时间。 */
    tick_delta = 0;
    /* 阻塞路径需要当前线程；非阻塞路径不会挂起它。 */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mb->parent.parent)));

    /* 锁住环形队列状态和两条等待链。 */
    level = rt_spin_lock_irqsave(&(mb->spinlock));

    /* 最常见的非阻塞满队列快速失败路径。 */
    if (mb->entry == mb->size && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);
        return -RT_EFULL;
    }

    /* 使用 while 而非 if：醒来只表示“应重新检查”，不保证空位仍在。 */
    while (mb->entry == mb->size)
    {
        /* 默认中断状态，正常接收者唤醒会覆盖为 EOK。 */
        thread->error = -RT_EINTR;

        /* 重试时预算可能已经扣到 0。 */
        if (timeout == 0)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);

            return -RT_EFULL;
        }

        /* 在邮箱锁仍持有时加入发送者等待链，防止错过接收产生的空位。 */
        ret = rt_thread_suspend_to_list(thread, &(mb->suspend_sender_thread),
                                        mb->parent.parent.flag, suspend_flag);

        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);
            return ret;
        }

        /* 正 timeout 表示有限总预算。 */
        if (timeout > 0)
        {
            rt_tick_t timeout_tick = timeout;
            /* 记录本轮开始时间，醒来后从剩余预算扣除。 */
            tick_delta = rt_tick_get();

            LOG_D("mb_send_wait: start timer of thread:%s",
                  thread->parent.name);

            /* 到期路径和接收者唤醒竞争处理该线程。 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        /* 挂起登记完成且对象锁已释放，当前线程让出 CPU。 */
        rt_schedule();

        /* 先检查唤醒原因，错误时不再访问队列。 */
        if (thread->error != RT_EOK)
        {
            /* 超时、信号、reset 或 delete 的错误直接返回。 */
            return thread->error;
        }

        level = rt_spin_lock_irqsave(&(mb->spinlock));

        /* 正数预算按实际经过 tick 递减，FOREVER 保持不变。 */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* 写入环形队列尾槽。 */
    mb->msg_pool[mb->in_offset] = value;
    /* 推进写游标并在末尾回绕。 */
    ++ mb->in_offset;
    if (mb->in_offset >= mb->size)
        mb->in_offset = 0;

    if(mb->entry < RT_MB_ENTRY_MAX)
    {
        /* 邮件数在实现计数类型范围内递增。 */
        mb->entry ++;
    }
    else
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);
        return -RT_EFULL; /* 防止 entry 计数溢出。 */
    }

    /* 新邮件可直接满足一个接收等待者。 */
    if (!rt_list_isempty(&mb->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mb->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    return RT_EOK;
}

/**
 * @brief 以不可被信号打断的模式发送邮件，并可等待空位。
 * @return 详细语义见 `_rt_mb_send_wait()`。
 */
rt_err_t rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_send_wait);

/** @brief 可被普通信号中断的等待发送；其余语义同 `rt_mb_send_wait()`。 */
rt_err_t rt_mb_send_wait_interruptible(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_send_wait_interruptible);

/** @brief 仅可被致命信号中断的等待发送；其余语义同 `rt_mb_send_wait()`。 */
rt_err_t rt_mb_send_wait_killable(rt_mailbox_t mb,
                         rt_ubase_t   value,
                         rt_int32_t   timeout)
{
    return _rt_mb_send_wait(mb, value, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mb_send_wait_killable);
/**
 * @brief 非阻塞地向邮箱尾部发送一个机器字。
 *
 * 等价于 `rt_mb_send_wait(mb, value, 0)`；邮箱满时立即返回 `-RT_EFULL`。
 *
 * @see      rt_mb_send_wait()
 *
 * @param mb 目标邮箱。
 *
 * @param value 要发送的机器字。
 *
 * @return 成功返回 `RT_EOK`，满时返回 `-RT_EFULL`。
 */
rt_err_t rt_mb_send(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait(mb, value, 0);
}
RTM_EXPORT(rt_mb_send);

/** @brief 非阻塞发送的兼容变体；timeout 为 0，故可中断等级不会实际进入挂起。 */
rt_err_t rt_mb_send_interruptible(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait_interruptible(mb, value, 0);
}
RTM_EXPORT(rt_mb_send_interruptible);

/** @brief 非阻塞发送的 killable 兼容变体；满时同样立即返回。 */
rt_err_t rt_mb_send_killable(rt_mailbox_t mb, rt_ubase_t value)
{
    return rt_mb_send_wait_killable(mb, value, 0);
}
RTM_EXPORT(rt_mb_send_killable);

/**
 * @brief 非阻塞地把紧急邮件插到邮箱队首。
 *
 * 函数把 `out_offset` 向前回退一个槽位并写值，所以下一次 recv 会先取到它；原有
 * 普通邮件的相对顺序不变。邮箱满时立即失败，不能等待。put hook 在加锁和容量
 * 检查之前调用；成功插入后若有接收者等待，唤醒链首并在解锁后调度。
 *
 * @see      rt_mb_send()
 *
 * @param mb 目标邮箱。
 *
 * @param value 紧急邮件的机器字值。
 *
 * @return 成功返回 `RT_EOK`；邮箱已满返回 `-RT_EFULL`。
 */
rt_err_t rt_mb_urgent(rt_mailbox_t mb, rt_ubase_t value)
{
    rt_base_t level;

    /* 类型验证。 */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mb->parent.parent)));

    level = rt_spin_lock_irqsave(&(mb->spinlock));

    if (mb->entry == mb->size)
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);
        return -RT_EFULL;
    }

    /* 在环形数组中把“下一读位置”向前移动一格。 */
    if (mb->out_offset > 0)
    {
        mb->out_offset --;
    }
    else
    {
        mb->out_offset = mb->size - 1;
    }

    /* 写入后它自然成为下一封被读取的邮件。 */
    mb->msg_pool[mb->out_offset] = value;

    /* 容量检查已经保证不会超过 size。 */
    mb->entry ++;

    /* 紧急邮件同样可满足一个等待数据的接收者。 */
    if (!rt_list_isempty(&mb->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mb->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    return RT_EOK;
}
RTM_EXPORT(rt_mb_urgent);


/**
 * @brief 从邮箱队首接收一个机器字；邮箱空时可等待数据。
 *
 * try-take hook 在获取邮箱锁之前调用。空邮箱的等待协议与等待发送相对称：线程
 * 挂入公共接收者链，有限等待启动线程定时器；醒来后用 while 重新检查，并从
 * 剩余 timeout 扣除实际等待 tick。
 *
 * 有数据后在锁内从 `out_offset` 复制值、推进读游标并减少 entry。消费产生一个
 * 空位时唤醒一个发送等待者。take hook 始终在邮箱锁释放后、成功路径上调用；
 * 若唤醒了发送者，则 hook 后再调度。
 *
 * @param mb 目标邮箱。
 *
 * @param value 非空输出指针，成功时写入一个 `rt_ubase_t` 邮件值。
 *
 * @param timeout 0 表示空时立即返回，正数为总等待 tick，FOREVER 为永久等待。
 * @param suspend_flag 等待的信号可中断等级。
 *
 * @return 成功返回 `RT_EOK`；非阻塞空邮箱或定时到期返回 `-RT_ETIMEOUT`；还可能
 *         返回信号、reset、delete 或挂起失败的错误。
 * @warning 中断上下文只能使用 timeout=0，且 `value` 必须有效。
 */
static rt_err_t _rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout, int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    rt_uint32_t tick_delta;
    rt_err_t ret;

    /* 代码直接解引用 value，调用者必须保证它非空且可写。 */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    /* 非零 timeout 允许睡眠，需要调度器可用。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* 用于维护跨多轮睡眠的总超时预算。 */
    tick_delta = 0;
    /* 等待时挂起当前线程。 */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mb->parent.parent)));

    level = rt_spin_lock_irqsave(&(mb->spinlock));

    /* 非阻塞空邮箱快速失败。 */
    if (mb->entry == 0 && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        return -RT_ETIMEOUT;
    }

    /* 醒来后必须重新验证数据确实仍可用。 */
    while (mb->entry == 0)
    {
        /* 正常发送者唤醒会改为 EOK。 */
        thread->error = -RT_EINTR;

        /* 首次或重试预算为 0，结束等待。 */
        if (timeout == 0)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);

            thread->error = -RT_ETIMEOUT;

            return -RT_ETIMEOUT;
        }

        /* 持邮箱锁完成接收者入链，避免错过并发发送。 */
        ret = rt_thread_suspend_to_list(thread, &(mb->parent.suspend_thread),
                                        mb->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mb->spinlock), level);
            return ret;
        }

        /* 正数启动有限等待定时器。 */
        if (timeout > 0)
        {
            rt_tick_t timeout_tick = timeout;
            /* 保存本轮睡眠起点。 */
            tick_delta = rt_tick_get();

            LOG_D("mb_recv: start timer of thread:%s",
                  thread->parent.name);

            /* 定时器与发送者竞争唤醒该线程。 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        /* 解开邮箱锁后进入调度等待。 */
        rt_schedule();

        /* 醒来首先检查原因。 */
        if (thread->error != RT_EOK)
        {
            /* 错误唤醒无需再次检查队列。 */
            return thread->error;
        }
        level = rt_spin_lock_irqsave(&(mb->spinlock));

        /* 从有限总预算中扣除本轮实际 tick。 */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* 复制队首邮件给调用者。 */
    *value = mb->msg_pool[mb->out_offset];

    /* 推进读游标并在数组末尾回绕。 */
    ++ mb->out_offset;
    if (mb->out_offset >= mb->size)
        mb->out_offset = 0;

    /* entry 与读写游标均在同一邮箱锁内更新。 */
    if(mb->entry > 0)
    {
        mb->entry --;
    }

    /* 消费后出现空位，可让一个满队列发送者重试。 */
    if (!rt_list_isempty(&(mb->suspend_sender_thread)))
    {
        rt_susp_list_dequeue(&(mb->suspend_sender_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mb->parent.parent)));

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mb->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mb->parent.parent)));

    return RT_EOK;
}

/**
 * @brief 以不可被信号打断的模式接收邮箱值。
 * @return 详细语义见 `_rt_mb_recv()`。
 */
rt_err_t rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_recv);

/** @brief 可被普通信号中断的邮箱接收；其余语义同 `rt_mb_recv()`。 */
rt_err_t rt_mb_recv_interruptible(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mb_recv_interruptible);

/** @brief 仅可被致命信号中断的邮箱接收；其余语义同 `rt_mb_recv()`。 */
rt_err_t rt_mb_recv_killable(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout)
{
    return _rt_mb_recv(mb, value, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mb_recv_killable);

/**
 * @brief 重置邮箱为空，并错误唤醒全部接收者和发送者。
 *
 * reset 在邮箱锁内清空逻辑计数和两个环形游标，但不会擦除 `msg_pool` 中旧字节；
 * 这些旧值因 entry=0 已不可见。解锁后主动请求调度。
 *
 * @param mb 目标邮箱。
 *
 * @param cmd 当前只支持 `RT_IPC_CMD_RESET`。
 *
 * @param arg 保留参数，当前忽略。
 *
 * @return reset 成功返回 `RT_EOK`，未知命令返回 `-RT_ERROR`。
 */
rt_err_t rt_mb_control(rt_mailbox_t mb, int cmd, void *arg)
{
    rt_base_t level;

    RT_UNUSED(arg);

    /* 类型验证。 */
    RT_ASSERT(mb != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mb->parent.parent) == RT_Object_Class_MailBox);

    if (cmd == RT_IPC_CMD_RESET)
    {
        level = rt_spin_lock_irqsave(&(mb->spinlock));

        /* 接收者等待旧数据条件，全部以错误退出。 */
        rt_susp_list_resume_all(&(mb->parent.suspend_thread), RT_ERROR);
        /* 发送者等待旧容量状态，也全部以错误退出。 */
        rt_susp_list_resume_all(&(mb->suspend_sender_thread), RT_ERROR);

        /* 逻辑清空环形队列，不执行数据区擦除。 */
        mb->entry      = 0;
        mb->in_offset  = 0;
        mb->out_offset = 0;

        rt_spin_unlock_irqrestore(&(mb->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    return -RT_ERROR;
}
RTM_EXPORT(rt_mb_control);

/**@}*/
#endif /* RT_USING_MAILBOX */

#ifdef RT_USING_MESSAGEQUEUE
/**
 * @addtogroup group_messagequeue Message Queue
 * @{
 */

/**
 * @brief 用调用者提供的连续内存池初始化静态消息队列。
 *
 * 消息池被切成若干固定块，每块布局为消息头 `struct rt_mq_message` 加上按
 * `RT_ALIGN_SIZE` 对齐后的 payload 空间。队列以单链表保存已发送消息，另一条
 * `msg_queue_free` 单链表保存空闲块；因此运行时发送不再动态分配内存。
 * 公共等待链保存接收者，`suspend_sender_thread` 保存池耗尽时等待空闲块的发送者。
 *
 * @see      rt_mq_create()
 *
 * @param mq 调用者提供的消息队列控制块。
 *
 * @param name 对象名称。
 *
 * @param msgpool 调用者拥有的连续可写内存，生命周期必须覆盖消息队列。
 *
 * @param msg_size 单条消息允许的最大 payload 字节数。
 *
 * @param pool_size `msgpool` 总字节数；完整块之外的尾部余数不会使用。
 *
 * @param flag 发送者和接收者等待链使用的 PRIO 或 FIFO 策略；它与可选的“消息
 *             自身优先级排序”是两件不同的事。
 *
 * @return 至少能切出一个完整消息块时返回 `RT_EOK`，否则返回 `-RT_EINVAL`。
 *
 * @warning 本实现先登记通用对象、后检查池能否形成消息块；调用者应预先保证
 *          `pool_size >= RT_ALIGN(msg_size, RT_ALIGN_SIZE) + sizeof(struct rt_mq_message)`，
 *          避免收到 EINVAL 时留下已登记的部分初始化对象。只能在线程上下文调用。
 */
rt_err_t rt_mq_init(rt_mq_t     mq,
                    const char *name,
                    void       *msgpool,
                    rt_size_t   msg_size,
                    rt_size_t   pool_size,
                    rt_uint8_t  flag)
{
    struct rt_mq_message *head;
    rt_base_t temp;
    register rt_size_t msg_align_size;

    /* 验证控制块和等待排序策略。 */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    /* 登记静态 MessageQueue 对象。 */
    rt_object_init(&(mq->parent.parent), RT_Object_Class_MessageQueue, name);

    /* 保存线程等待链顺序。 */
    mq->parent.parent.flag = flag;

    /* 建立空的接收者等待链。 */
    _ipc_object_init(&(mq->parent));

    /* 静态队列只借用内存池，不取得释放所有权。 */
    mq->msg_pool = msgpool;

    /* 每块 payload 按平台对齐，再加内部链表/长度/优先级头。 */
    msg_align_size = RT_ALIGN(msg_size, RT_ALIGN_SIZE);
    mq->msg_size = msg_size;
    mq->max_msgs = pool_size / (msg_align_size + sizeof(struct rt_mq_message));

    if (0 == mq->max_msgs)
    {
        return -RT_EINVAL;
    }

    /* 已排队消息链初始为空。 */
    mq->msg_queue_head = RT_NULL;
    mq->msg_queue_tail = RT_NULL;

    /* 顺序切分内存池，把所有块压入空闲单链表。 */
    mq->msg_queue_free = RT_NULL;
    for (temp = 0; temp < mq->max_msgs; temp ++)
    {
        head = (struct rt_mq_message *)((rt_uint8_t *)mq->msg_pool +
                                        temp * (msg_align_size + sizeof(struct rt_mq_message)));
        head->next = (struct rt_mq_message *)mq->msg_queue_free;
        mq->msg_queue_free = head;
    }

    /* 当前没有已发送消息。 */
    mq->entry = 0;

    /* 池耗尽的发送者使用独立等待链。 */
    rt_list_init(&(mq->suspend_sender_thread));
    rt_spin_lock_init(&(mq->spinlock));

    return RT_EOK;
}
RTM_EXPORT(rt_mq_init);


/**
 * @brief 注销静态消息队列并错误唤醒接收者、发送者两类等待线程。
 *
 * 外部 `msgpool` 不会释放。调用者须先停止并发收发。
 *
 * @see      rt_mq_delete()
 *
 * @param mq 由 `rt_mq_init()` 初始化的静态消息队列。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 动态队列必须使用 `rt_mq_delete()`。
 */
rt_err_t rt_mq_detach(rt_mq_t mq)
{
    rt_base_t level;

    /* 验证对象类和静态生命周期。 */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(rt_object_is_systemobject(&mq->parent.parent));

    level = rt_spin_lock_irqsave(&(mq->spinlock));
    /* 结束等待消息的接收者。 */
    rt_susp_list_resume_all(&mq->parent.suspend_thread, RT_ERROR);
    /* 结束等待空闲消息块的发送者。 */
    rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);
    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* 注销控制块，不释放外部消息池。 */
    rt_object_detach(&(mq->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mq_detach);

#ifdef RT_USING_HEAP
/**
 * @brief 从内核堆创建动态消息队列及其固定块消息池。
 *
 * 控制块和消息池分两次分配；消息池失败时会回滚控制块。池大小为
 * `(RT_ALIGN(msg_size, RT_ALIGN_SIZE) + header) * max_msgs`，调用者应确保乘法
 * 不溢出且至少请求一个消息块。最终必须调用 `rt_mq_delete()`。
 *
 * @see      rt_mq_init()
 *
 * @param name 对象名称。
 *
 * @param msg_size 每条消息最大 payload 字节数。
 *
 * @param max_msgs 固定块数量，也就是队列最大消息数。
 *
 * @param flag 线程等待链使用的 PRIO 或 FIFO 策略。
 *
 * @return 成功返回队列；任一堆分配失败返回 `RT_NULL`。
 *
 * @warning 只能在线程上下文调用。
 */
rt_mq_t rt_mq_create(const char *name,
                     rt_size_t   msg_size,
                     rt_size_t   max_msgs,
                     rt_uint8_t  flag)
{
    struct rt_messagequeue *mq;
    struct rt_mq_message *head;
    rt_base_t temp;
    register rt_size_t msg_align_size;

    RT_ASSERT((flag == RT_IPC_FLAG_FIFO) || (flag == RT_IPC_FLAG_PRIO));

    RT_DEBUG_NOT_IN_INTERRUPT;

    /* 首先分配并登记动态控制块。 */
    mq = (rt_mq_t)rt_object_allocate(RT_Object_Class_MessageQueue, name);
    if (mq == RT_NULL)
        return mq;

    /* 保存等待者排序策略。 */
    mq->parent.parent.flag = flag;

    /* 建立接收者等待链。 */
    _ipc_object_init(&(mq->parent));

    /* 计算当前平台上的消息块布局。 */

    /* payload 区向上对齐，公开 msg_size 仍保存原始最大长度。 */
    msg_align_size = RT_ALIGN(msg_size, RT_ALIGN_SIZE);
    mq->msg_size = msg_size;
    mq->max_msgs = max_msgs;

    /* 一次分配全部固定块，运行时收发不再分配。 */
    mq->msg_pool = RT_KERNEL_MALLOC((msg_align_size + sizeof(struct rt_mq_message)) * mq->max_msgs);
    if (mq->msg_pool == RT_NULL)
    {
        rt_object_delete(&(mq->parent.parent));

        return RT_NULL;
    }

    /* 已排队消息链初始为空。 */
    mq->msg_queue_head = RT_NULL;
    mq->msg_queue_tail = RT_NULL;

    /* 把每个块的头部串成空闲链。 */
    mq->msg_queue_free = RT_NULL;
    for (temp = 0; temp < mq->max_msgs; temp ++)
    {
        head = (struct rt_mq_message *)((rt_uint8_t *)mq->msg_pool +
                                        temp * (msg_align_size + sizeof(struct rt_mq_message)));
        head->next = (struct rt_mq_message *)mq->msg_queue_free;
        mq->msg_queue_free = head;
    }

    /* 没有已排队消息。 */
    mq->entry = 0;

    /* 建立等待空闲块的发送者链。 */
    rt_list_init(&(mq->suspend_sender_thread));
    rt_spin_lock_init(&(mq->spinlock));

    return mq;
}
RTM_EXPORT(rt_mq_create);


/**
 * @brief 删除动态消息队列，结束全部等待并释放消息池和控制块。
 *
 * 等待者在队列锁内以错误转为 ready，之后释放动态池并删除通用对象。调用者须
 * 在更高层阻止并发访问和仍在进行的锁外数据复制。
 *
 * @see      rt_mq_detach()
 *
 * @param mq 由 `rt_mq_create()` 创建的动态消息队列。
 *
 * @return 当前实现返回 `RT_EOK`。
 *
 * @warning 静态队列必须使用 `rt_mq_detach()`；本函数释放堆内存，只能在线程
 *          上下文调用。
 */
rt_err_t rt_mq_delete(rt_mq_t mq)
{
    /* 验证动态生命周期。 */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(rt_object_is_systemobject(&mq->parent.parent) == RT_FALSE);

    RT_DEBUG_NOT_IN_INTERRUPT;

    rt_spin_lock(&(mq->spinlock));
    /* 结束接收等待。 */
    rt_susp_list_resume_all(&(mq->parent.suspend_thread), RT_ERROR);
    /* 结束发送等待。 */
    rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);

    rt_spin_unlock(&(mq->spinlock));

    /* 动态 create 路径拥有整个固定块池。 */
    RT_KERNEL_FREE(mq->msg_pool);

    /* 注销并释放控制块。 */
    rt_object_delete(&(mq->parent.parent));

    return RT_EOK;
}
RTM_EXPORT(rt_mq_delete);
#endif /* RT_USING_HEAP */

/**
 * @brief 复制并发送一条消息；空闲块耗尽时可等待。
 *
 * put hook 在获取队列锁前调用。函数先在锁内从 `msg_queue_free` 独占取走一个
 * 消息块；若无块且允许等待，就把线程放入发送者等待链，使用与邮箱相同的总
 * timeout 扣减和 while 重检协议。
 *
 * 取到块后先释放队列锁，再填写长度并复制 payload，避免较大 memcpy 长时间
 * 关闭中断。该块已从 free 链摘除，因此其他发送者不会使用它；复制完成后重新
 * 加锁并链接到可见消息链。普通配置追加到尾部保持 FIFO；启用
 * `RT_USING_MESSAGEQUEUE_PRIORITY` 时按 `prio` 从大到小稳定插入，同优先级保持
 * 发送先后。最后增加 entry、唤醒一个接收者并在解锁后调度。
 *
 * @see      _rt_mq_send_wait()
 *
 * @param mq 目标消息队列。
 *
 * @param buffer 非空源数据；函数在返回前复制，返回后源缓冲区可复用。
 *
 * @param size 非零实际消息字节数，不得超过队列 `msg_size`。
 *
 * @param prio 消息优先级，数值越大越先接收；未启用消息优先级配置时忽略。
 *
 * @param timeout 0 满时立即返回，正数为总等待 tick，FOREVER 为永久等待。
 *
 * @param suspend_flag 等待的信号可中断等级。
 *
 * @return 成功返回 `RT_EOK`；消息过大返回 `-RT_ERROR`；非阻塞满队列或 entry
 *         上限溢出返回 `-RT_EFULL`；等待可能返回超时、信号或销毁错误。
 *
 * @warning 中断上下文只能使用 timeout=0；等待形式仅限线程。调用者还必须保证
 *          队列不会在锁外 memcpy 阶段被并发删除。
 */
static rt_err_t _rt_mq_send_wait(rt_mq_t mq,
                                 const void *buffer,
                                 rt_size_t size,
                                 rt_int32_t prio,
                                 rt_int32_t timeout,
                                 int suspend_flag)
{
    rt_base_t level;
    struct rt_mq_message *msg;
    rt_uint32_t tick_delta;
    struct rt_thread *thread;
    rt_err_t ret;

    RT_UNUSED(prio);

    /* 验证类型、源缓冲区和非零长度。 */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(buffer != RT_NULL);
    RT_ASSERT(size != 0);

    /* 只有 timeout 非零时才需要睡眠和调度。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* 固定块 payload 无法容纳更大的消息。 */
    if (size > mq->msg_size)
        return -RT_ERROR;

    /* 维护多轮等待的剩余总预算。 */
    tick_delta = 0;
    /* 阻塞路径使用当前线程控制块。 */
    thread = rt_thread_self();

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mq->parent.parent)));

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* 先观察空闲链首。 */
    msg = (struct rt_mq_message *)mq->msg_queue_free;
    /* 非阻塞满队列快速失败。 */
    if (msg == RT_NULL && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        return -RT_EFULL;
    }

    /* 醒来后仍需重新竞争空闲块，所以使用 while。 */
    while ((msg = (struct rt_mq_message *)mq->msg_queue_free) == RT_NULL)
    {
        /* 接收者正常唤醒时会覆盖为 EOK。 */
        thread->error = -RT_EINTR;

        /* 首次或预算耗尽后的非等待失败。 */
        if (timeout == 0)
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);

            return -RT_EFULL;
        }

        /* 持队列锁加入发送者等待链，避免与刚释放的消息块错过。 */
        ret = rt_thread_suspend_to_list(thread, &(mq->suspend_sender_thread),
                                        mq->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);
            return ret;
        }

        /* 正 timeout 启动有限等待。 */
        if (timeout > 0)
        {
            rt_tick_t timeout_tick = timeout;
            /* 保存本轮开始 tick。 */
            tick_delta = rt_tick_get();

            LOG_D("mq_send_wait: start timer of thread:%s",
                  thread->parent.name);

            /* 到期路径与接收者释放空闲块竞争唤醒。 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        /* 入链并解锁后让出 CPU。 */
        rt_schedule();

        /* 先处理非正常唤醒。 */
        if (thread->error != RT_EOK)
        {
            /* 超时、信号、reset/delete 等错误直接返回。 */
            return thread->error;
        }
        level = rt_spin_lock_irqsave(&(mq->spinlock));

        /* 正数预算扣除本轮实际等待 tick。 */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* 在锁内独占弹出一个固定块。 */
    mq->msg_queue_free = msg->next;

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* 在锁外准备尚不可见的新消息节点。 */
    msg->next = RT_NULL;

    /* 保存真实长度，接收者可据此决定复制多少字节。 */
    ((struct rt_mq_message *)msg)->length = size;
    /* memcpy 放在锁外，降低关中断临界区长度。 */
    rt_memcpy(GET_MESSAGEBYTE_ADDR(msg), buffer, size);

    /* 重新加锁，把完整消息发布到接收者可见的队列。 */
    level = rt_spin_lock_irqsave(&(mq->spinlock));
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
    /*
     * 消息优先级与线程优先级方向相反：prio 数值越大越靠近队首。遍历时在第一个
     * 较低优先级节点前插入；相等则继续，因此同优先级仍保持 FIFO。
     */
    msg->prio = prio;
    if (mq->msg_queue_head == RT_NULL)
        mq->msg_queue_head = msg;

    struct rt_mq_message *node, *prev_node = RT_NULL;
    for (node = mq->msg_queue_head; node != RT_NULL; node = node->next)
    {
        if (node->prio < msg->prio)
        {
            if (prev_node == RT_NULL)
                mq->msg_queue_head = msg;
            else
                prev_node->next = msg;
            msg->next = node;
            break;
        }
        if (node->next == RT_NULL)
        {
            if (node != msg)
                node->next = msg;
            mq->msg_queue_tail = msg;
            break;
        }
        prev_node = node;
    }
#else
    /* 无消息优先级功能时，普通发送始终追加到 FIFO 尾部。 */
    if (mq->msg_queue_tail != RT_NULL)
    {
        /* 非空链把旧尾节点指向新节点。 */
        ((struct rt_mq_message *)mq->msg_queue_tail)->next = msg;
    }

    /* 新节点成为队尾。 */
    mq->msg_queue_tail = msg;
    /* 原链为空时它同时也是队首。 */
    if (mq->msg_queue_head == RT_NULL)
        mq->msg_queue_head = msg;
#endif

    if(mq->entry < RT_MQ_ENTRY_MAX)
    {
        /* 发布成功后增加可接收消息数。 */
        mq->entry ++;
    }
    else
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);
        return -RT_EFULL; /* 防止 entry 类型溢出；正常 max_msgs 应更早限制容量。 */
    }

    /* 新消息可满足一个等待接收者。 */
    if (!rt_list_isempty(&mq->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mq->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }
    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    return RT_EOK;
}

/**
 * @brief 以不可被信号打断的模式发送普通优先级消息，并可等待空闲块。
 * @return 详细语义见 `_rt_mq_send_wait()`。
 */
rt_err_t rt_mq_send_wait(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_send_wait);

/** @brief 可被普通信号中断的等待发送；其余语义同 `rt_mq_send_wait()`。 */
rt_err_t rt_mq_send_wait_interruptible(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_send_wait_interruptible);

/** @brief 仅可被致命信号中断的等待发送；其余语义同 `rt_mq_send_wait()`。 */
rt_err_t rt_mq_send_wait_killable(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout)
{
    return _rt_mq_send_wait(mq, buffer, size, 0, timeout, RT_KILLABLE);
}
RTM_EXPORT(rt_mq_send_wait_killable);
/**
 * @brief 非阻塞地复制一条普通消息到队列。
 *
 * 等价于 `rt_mq_send_wait(mq, buffer, size, 0)`；无空闲消息块时立即返回
 * `-RT_EFULL`。普通优先级配置下使用 prio=0。
 *
 * @see      rt_mq_send_wait()
 *
 * @param mq 目标消息队列。
 *
 * @param buffer 非空消息源。
 *
 * @param size 非零消息字节数，不得超过该队列上限。
 *
 * @return 成功返回 `RT_EOK`，失败返回负错误码。
 *
 * @warning 可在线程或中断上下文使用，但 memcpy 时间应满足中断实时性要求。
 */
rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait(mq, buffer, size, 0);
}
RTM_EXPORT(rt_mq_send);

/** @brief 非阻塞发送的 interruptible 兼容变体；不会实际挂起。 */
rt_err_t rt_mq_send_interruptible(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait_interruptible(mq, buffer, size, 0);
}
RTM_EXPORT(rt_mq_send_interruptible);

/** @brief 非阻塞发送的 killable 兼容变体；不会实际挂起。 */
rt_err_t rt_mq_send_killable(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    return rt_mq_send_wait_killable(mq, buffer, size, 0);
}
RTM_EXPORT(rt_mq_send_killable);
/**
 * @brief 非阻塞地把一条复制消息插入队首，使其优先于当前所有排队消息。
 *
 * 与普通发送相同，先在锁内独占空闲块、锁外复制、再加锁发布；区别是直接链接
 * 到 `msg_queue_head`。put hook 在获取锁和容量检查之前调用。该 API 不等待空闲
 * 块，队满立即失败。启用消息优先级时，本路径仍是无条件插队，且不会给消息头
 * 写新的 `prio`；若随后通过优先级接收接口读取 prio，该字段可能保留块的旧值，
 * 应用不应把 urgent 与 prio 语义混为一谈。
 *
 * @see      rt_mq_send()
 *
 * @param mq 目标消息队列。
 *
 * @param buffer 非空源数据。
 *
 * @param size 非零消息长度，不得超过 `mq->msg_size`。
 *
 * @return 成功返回 `RT_EOK`；消息过大返回 `-RT_ERROR`；无空闲块或计数上限
 *         溢出返回 `-RT_EFULL`。
 */
rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size)
{
    rt_base_t level;
    struct rt_mq_message *msg;

    /* 验证对象、源缓冲区和长度。 */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(buffer != RT_NULL);
    RT_ASSERT(size != 0);

    /* 固定 payload 区不能容纳超长消息。 */
    if (size > mq->msg_size)
        return -RT_ERROR;

    RT_OBJECT_HOOK_CALL(rt_object_put_hook, (&(mq->parent.parent)));

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* 在锁内观察并独占空闲块。 */
    msg = (struct rt_mq_message *)mq->msg_queue_free;
    /* urgent 不提供等待语义。 */
    if (msg == RT_NULL)
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        return -RT_EFULL;
    }
    /* 从空闲链弹出后，其他发送者不会再取得该块。 */
    mq->msg_queue_free = msg->next;

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* 锁外填写内部长度。 */
    ((struct rt_mq_message *)msg)->length = size;
    /* 锁外复制 payload，缩短关中断时间。 */
    rt_memcpy(GET_MESSAGEBYTE_ADDR(msg), buffer, size);

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* 发布到队首，优先于包括高 prio 在内的现有消息。 */
    msg->next = (struct rt_mq_message *)mq->msg_queue_head;
    mq->msg_queue_head = msg;

    /* 原队列为空时，该消息也成为队尾。 */
    if (mq->msg_queue_tail == RT_NULL)
        mq->msg_queue_tail = msg;

    if(mq->entry < RT_MQ_ENTRY_MAX)
    {
        /* 增加已发布消息数。 */
        mq->entry ++;
    }
    else
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);
        return -RT_EFULL; /* 防止 entry 计数溢出。 */
    }

    /* 唤醒一个等待消息的接收者。 */
    if (!rt_list_isempty(&mq->parent.suspend_thread))
    {
        rt_susp_list_dequeue(&(mq->parent.suspend_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    return RT_EOK;
}
RTM_EXPORT(rt_mq_urgent);

/**
 * @brief 取出并复制队首消息；队列空时可等待。
 *
 * try-take hook 在获取队列锁前调用。空队列时使用公共接收等待链、线程定时器、
 * 总 timeout 扣减和 while 重检协议。消息可用后，在锁内从队首摘下并减少 entry，
 * 随即解锁；payload memcpy 在锁外进行。消息块此时既不在已排队链也不在 free 链，
 * 因而不会被并发复用。复制完再加锁把块归还 free 链，并唤醒一个发送等待者。
 * take hook 在成功复制、队列锁释放后调用。
 *
 * @param mq 目标消息队列。
 *
 * @param buffer 非空接收缓冲区。
 *
 * @param prio 可选消息优先级输出；仅启用优先级队列时写入。
 *
 * @param size 接收缓冲区容量。若小于实际消息，只复制前 `size` 字节，余下内容随
 *             消息块归还而丢弃。
 *
 * @param timeout 0 空时立即返回，正数为总等待 tick，FOREVER 为永久等待。
 *
 * @param suspend_flag 等待的信号可中断等级。
 *
 * @return 成功返回实际复制字节数 `min(消息真实长度, size)`，不是未截断的原长度；
 *         空队列不等待/超时返回 `-RT_ETIMEOUT`，其他等待异常返回相应负错误码。
 * @warning 中断上下文只能使用 timeout=0；调用者必须保证 buffer 可写。
 */
static rt_ssize_t _rt_mq_recv(rt_mq_t mq,
                              void *buffer,
                              rt_size_t size,
                              rt_int32_t *prio,
                              rt_int32_t timeout,
                              int suspend_flag)
{
    struct rt_thread *thread;
    rt_base_t level;
    struct rt_mq_message *msg;
    rt_uint32_t tick_delta;
    rt_err_t ret;
    rt_size_t len;

    RT_UNUSED(prio);

    /* 验证对象、目标缓冲区和非零容量。 */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);
    RT_ASSERT(buffer != RT_NULL);
    RT_ASSERT(size != 0);

    /* 非零 timeout 允许挂起，需要调度器可用。 */
    RT_DEBUG_SCHEDULER_AVAILABLE(timeout != 0);

    /* 维护跨多轮睡眠的剩余总预算。 */
    tick_delta = 0;
    /* 阻塞路径使用当前线程。 */
    thread = rt_thread_self();
    RT_OBJECT_HOOK_CALL(rt_object_trytake_hook, (&(mq->parent.parent)));

    level = rt_spin_lock_irqsave(&(mq->spinlock));

    /* 空队列非阻塞快速失败。 */
    if (mq->entry == 0 && timeout == 0)
    {
        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        return -RT_ETIMEOUT;
    }

    /* 正常唤醒也只表示需要重检，不保证消息还在。 */
    while (mq->entry == 0)
    {
        /* 发送者正常唤醒会覆盖为 EOK。 */
        thread->error = -RT_EINTR;

        /* 首次或剩余预算为 0。 */
        if (timeout == 0)
        {
            /* 恢复进入队列临界区前的中断状态。 */
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);

            thread->error = -RT_ETIMEOUT;

            return -RT_ETIMEOUT;
        }

        /* 持队列锁完成接收者入链，避免漏掉并发发送。 */
        ret = rt_thread_suspend_to_list(thread, &(mq->parent.suspend_thread),
                                        mq->parent.parent.flag, suspend_flag);
        if (ret != RT_EOK)
        {
            rt_spin_unlock_irqrestore(&(mq->spinlock), level);
            return ret;
        }

        /* 正 timeout 启动有限等待。 */
        if (timeout > 0)
        {
            rt_tick_t timeout_tick = timeout;
            /* 记录本轮睡眠起点。 */
            tick_delta = rt_tick_get();

            LOG_D("set thread:%s to timer list",
                  thread->parent.name);

            /* 定时器与发送路径竞争唤醒。 */
            rt_timer_control(&(thread->thread_timer),
                             RT_TIMER_CTRL_SET_TIME,
                             &timeout_tick);
            rt_timer_start(&(thread->thread_timer));
        }

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        /* 解锁后让出 CPU。 */
        rt_schedule();

        /* 先检查醒来是否因为真实消息到达。 */
        if (thread->error != RT_EOK)
        {
            /* 超时、信号、reset/delete 等直接返回。 */
            return thread->error;
        }

        level = rt_spin_lock_irqsave(&(mq->spinlock));

        /* 正数预算扣除本轮实际等待 tick。 */
        if (timeout > 0)
        {
            tick_delta = rt_tick_get() - tick_delta;
            timeout -= tick_delta;
            if (timeout < 0)
                timeout = 0;
        }
    }

    /* 锁内独占摘取当前队首消息。 */
    msg = (struct rt_mq_message *)mq->msg_queue_head;

    /* 推进队首。 */
    mq->msg_queue_head = msg->next;
    /* 摘下最后一条时同步清空队尾。 */
    if (mq->msg_queue_tail == msg)
        mq->msg_queue_tail = RT_NULL;

    /* entry 仅统计仍在公开消息链中的节点。 */
    if(mq->entry > 0)
    {
        mq->entry --;
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    /* 从消息头读取发送者记录的真实长度。 */
    len = ((struct rt_mq_message *)msg)->length;

    if (len > size)
        len = size;
    /* 缓冲区不足就截断复制；剩余数据不会留给下一次 recv。 */
    rt_memcpy(buffer, GET_MESSAGEBYTE_ADDR(msg), len);

#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
    if (prio != RT_NULL)
        *prio = msg->prio;
#endif
    level = rt_spin_lock_irqsave(&(mq->spinlock));
    /* 复制结束后才把块压回空闲链，防止发送者过早覆盖 payload。 */
    msg->next = (struct rt_mq_message *)mq->msg_queue_free;
    mq->msg_queue_free = msg;

    /* 新空闲块可满足一个等待发送者。 */
    if (!rt_list_isempty(&(mq->suspend_sender_thread)))
    {
        rt_susp_list_dequeue(&(mq->suspend_sender_thread), RT_EOK);

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mq->parent.parent)));

        rt_schedule();

        return len;
    }

    rt_spin_unlock_irqrestore(&(mq->spinlock), level);

    RT_OBJECT_HOOK_CALL(rt_object_take_hook, (&(mq->parent.parent)));

    return len;
}

/**
 * @brief 以不可被信号打断的模式接收队首消息。
 * @return 成功为复制长度，失败为负错误码；详见 `_rt_mq_recv()`。
 */
rt_ssize_t rt_mq_recv(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, 0, timeout, RT_UNINTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_recv);

/** @brief 可被普通信号中断的消息接收；其余语义同 `rt_mq_recv()`。 */
rt_ssize_t rt_mq_recv_interruptible(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, 0, timeout, RT_INTERRUPTIBLE);
}
RTM_EXPORT(rt_mq_recv_interruptible);

/** @brief 仅可被致命信号中断的消息接收；其余语义同 `rt_mq_recv()`。 */
rt_ssize_t rt_mq_recv_killable(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout)
{
    return _rt_mq_recv(mq, buffer, size, 0, timeout, RT_KILLABLE);
}
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
/**
 * @brief 发送带消息优先级的消息，并由调用者选择等待可中断等级。
 * @note prio 数值越大越靠近队首，同优先级保持 FIFO。
 */
rt_err_t rt_mq_send_wait_prio(rt_mq_t mq,
                              const void *buffer,
                              rt_size_t size,
                              rt_int32_t prio,
                              rt_int32_t timeout,
                              int suspend_flag)
{
    return _rt_mq_send_wait(mq, buffer, size, prio, timeout, suspend_flag);
}
/**
 * @brief 接收最高排序消息，并可返回其优先级。
 * @note urgent 消息虽然位于队首，但其 prio 字段不由 urgent API 初始化。
 */
rt_ssize_t rt_mq_recv_prio(rt_mq_t mq,
                           void *buffer,
                           rt_size_t size,
                           rt_int32_t *prio,
                           rt_int32_t timeout,
                           int suspend_flag)
{
    return _rt_mq_recv(mq, buffer, size, prio, timeout, suspend_flag);
}
#endif
RTM_EXPORT(rt_mq_recv_killable);
/**
 * @brief 重置消息队列为空，并错误唤醒全部发送和接收等待者。
 *
 * reset 在队列锁内把已排队消息逐个移回 free 链，并把 entry 清零；payload 不会
 * 擦除，但块重新分配后会被新消息覆盖。解锁后主动调度。`arg` 当前忽略。
 *
 * @param mq 目标消息队列。
 * @param cmd 当前只支持 `RT_IPC_CMD_RESET`。
 * @param arg 保留参数，当前忽略。
 * @return reset 成功返回 `RT_EOK`，未知命令返回 `-RT_ERROR`。
 */
rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg)
{
    rt_base_t level;
    struct rt_mq_message *msg;

    RT_UNUSED(arg);

    /* 类型验证。 */
    RT_ASSERT(mq != RT_NULL);
    RT_ASSERT(rt_object_get_type(&mq->parent.parent) == RT_Object_Class_MessageQueue);

    if (cmd == RT_IPC_CMD_RESET)
    {
        level = rt_spin_lock_irqsave(&(mq->spinlock));

        /* 等待消息的接收者以错误退出。 */
        rt_susp_list_resume_all(&mq->parent.suspend_thread, RT_ERROR);
        /* 等待空闲块的发送者也以错误退出。 */
        rt_susp_list_resume_all(&(mq->suspend_sender_thread), RT_ERROR);

        /* 把公开消息链的所有块回收到空闲链。 */
        while (mq->msg_queue_head != RT_NULL)
        {
            /* 取当前队首。 */
            msg = (struct rt_mq_message *)mq->msg_queue_head;

            /* 推进公开队首。 */
            mq->msg_queue_head = msg->next;
            /* 处理最后节点时同步清空队尾。 */
            if (mq->msg_queue_tail == msg)
                mq->msg_queue_tail = RT_NULL;

            /* 回收到 free 链首。 */
            msg->next = (struct rt_mq_message *)mq->msg_queue_free;
            mq->msg_queue_free = msg;
        }

        /* 队列现在逻辑为空。 */
        mq->entry = 0;

        rt_spin_unlock_irqrestore(&(mq->spinlock), level);

        rt_schedule();

        return RT_EOK;
    }

    return -RT_ERROR;
}
RTM_EXPORT(rt_mq_control);

/**@}*/
#endif /* RT_USING_MESSAGEQUEUE */
/**@}*/
