/*
 * Copyright (c) 2006-2024 RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期           作者         说明
 * 2006-03-18     Bernard      第一个版本
 * 2006-04-26     Bernard      添加信号量 API
 * 2006-08-10     Bernard      添加版本信息
 * 2007-01-28     Bernard      将 RT_OBJECT_Class_Static 重命名为 RT_Object_Class_Static
 * 2007-03-03     Bernard      将定义清理为 rtdef.h
 * 2010-04-11     yi.qiu       添加模块功能
 * 2013-06-24     Bernard      不使用RT_USING_CONSOLE时添加rt_kprintf重新定义。
 * 2016-08-09     ArdaFu       添加新线程和中断钩子。
 * 2018-11-22     Jesven       添加所有cpu的锁和ipi处理函数
 * 2021-02-28     Meco Man     添加RT_KSERVICE_USING_STDLIB
 * 2021-11-14     Meco Man     添加 rtlegacy.h 以实现兼容性
 * 2022-06-04     Meco Man     删除 strnlen
 * 2023-05-20     Bernard      将 rtatomic.h 头文件添加到包含文件中。
 * 2023-06-30     ChuShicheng  从 rtdebug.h 移动调试检查
 * 2023-10-16     Shell        支持新的回溯框架
 * 2023-12-10     xqyjlj       修复 UP 配置中的自旋锁
 * 2024-01-25     Shell        为 IPC 原语添加 rt_susp_list
 * 2024-03-10     Meco Man     将std libc相关函数移至rtklibc
 */

#ifndef __RT_THREAD_H__
#define __RT_THREAD_H__

/**
 * @file rtthread.h
 * @brief 公共 RT-Thread 内核 API 和跨子系统服务声明。
 *
 * 应用程序、组件、BSP 驱动程序和内核实现文件使用此总入口头文件来访问对象、时钟、定时器、线程、调度器、IPC、内存、设备、中断、控制台和诊断服务。对象布局和命令/标志值位于 rtdef.h 中；架构接口约定位于 rthw.h；侵入式容器和底层辅助工具位于 rtservice.h 中。
 *
 * 上下文规则是 API 合约的一部分。 任何可能等待的操作都需要一个正在运行的调度器、线程上下文和一个可用的调度器。仅当实现明确支持时，才可以从 ISR 调用非阻塞释放/通知操作。 钩子回调在钩子点同步执行，并继承该点的中断、锁定和重入约束。
 */

#include <rtconfig.h>
#include <rtdef.h>
#include <rtservice.h>
#include <rtm.h>
#include <rtatomic.h>
#include <rtklibc.h>
#ifdef RT_USING_LEGACY
#include <rtlegacy.h>
#endif
#ifdef RT_USING_FINSH
#include <finsh.h>
#endif /* RT_USING_FINSH */

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __GNUC__
/** 标准启动路径使用的 GCC 兼容 C 入口包装器。 */
int entry(void);
#endif

/**
 * @name 内核对象注册与生命周期管理
 * 在通用对象 API 中，rt_object_init() 使用 Static 属性标记对象并与 detach 配对，而 rt_object_allocate() 则省略该位并与 delete 配对。 该位记录初始化/生命周期路径，而不是后备存储的物理起源：类包装器可以初始化堆分配的实例，然后通过其自己的销毁 API 释放它。
 */
/** 返回内部类注册表，或者当不支持 @p type 时返回 NULL。 */
struct rt_object_information *
rt_object_get_information(enum rt_object_class_type type);
/**
 * 返回全局注册表中 @p type 的对象数。不包括保存在模块私有列表中的模块拥有的对象。
 */
int rt_object_get_length(enum rt_object_class_type type);
/**
 * 从 @p type 的全局注册表复制最多 @p maxlen 指针。 不包括可加载模块的私有对象列表上的对象。
 */
int rt_object_get_pointers(enum rt_object_class_type type, rt_object_t *pointers, int maxlen);

/** 在调用者拥有的内存中初始化并注册一个静态对象。 */
void rt_object_init(struct rt_object         *object,
                    enum rt_object_class_type type,
                    const char               *name);
/** 将静态对象从其类注册表中分离出来，而不释放其存储空间。 */
void rt_object_detach(rt_object_t object);
#ifdef RT_USING_HEAP
/** 分配、初始化和注册所请求类的动态对象。 */
rt_object_t rt_object_allocate(enum rt_object_class_type type, const char *name);
/** 取消注册并释放由 rt_object_allocate() 创建的对象。 */
void rt_object_delete(rt_object_t object);
/**
 * 为 @p data 创建动态自定义包装器。数据本身只会通过 @p data_destroy 销毁；包装器并不知道数据的实际类型或分配来源。
 */
rt_object_t rt_custom_object_create(const char *name, void *data, rt_err_t (*data_destroy)(void *));
/**
 * 调用数据析构函数（如果存在），然后删除包装器，无论析构函数的结果如何。 即使有效的包装器仍然被删除，丢失的析构函数也会留下初始的负返回状态。
 */
rt_err_t rt_custom_object_destroy(rt_object_t obj);
#endif /* RT_USING_HEAP */
/** 返回当前对象类型是否带有RT_Object_Class_Static。 */
rt_bool_t rt_object_is_systemobject(rt_object_t object);
/** 返回按 object.c 定义删除所有权位的逻辑类。 */
rt_uint8_t rt_object_get_type(rt_object_t object);
/**
 * 访问 @p type 的全局注册表，直到迭代器返回非零。仅链接在可加载模块私有链表中的对象不会被访问。迭代器在持有类注册表自旋锁时运行：它不得阻塞、改变该注册表，也不得调用会再次获取同一把锁的 API。迭代器返回正值表示正常提前结束，返回负值则把错误继续传给调用者。
 */
rt_err_t rt_object_for_each(rt_uint8_t type, rt_object_iter_t iter, void *data);
/**
 * 将 @p name 截断为 RT_NAME_MAX - 1 后查找全局注册表对象，或者返回 NULL。 仅从线程上下文调用；不搜索模块私有对象。
 */
rt_object_t rt_object_find(const char *name, rt_uint8_t type);
/** 将对象的名称复制到调用者缓冲区并保证有界访问。 */
rt_err_t rt_object_get_name(rt_object_t object, char *name, rt_uint8_t name_size);

#ifdef RT_USING_HOOK
/**
 * 安装单侦听器对象钩子。
 *
 * Attach 在基本初始化之后但在注册表插入之前运行； detach 在注册表删除之前和类型变为 Null 之前运行。 通用的 trytake/take/put 钩子是历史跟踪点，其确切的时间和成功含义取决于每个调用站点：trytake 包括非阻塞尝试，定时器 take 优先于列表插入，而 put 不保证钩子后面的操作会成功。 某些回调在持有对象锁时执行。 传递 NULL 会禁用函数指针钩子。 每个回调都会接收一个借用的指针，并且必须保持有界，避免递归使用同一对象，并且不会在对象的生命周期之外保留它。
 */
void rt_object_attach_sethook(void (*hook)(struct rt_object *object));
void rt_object_detach_sethook(void (*hook)(struct rt_object *object));
void rt_object_trytake_sethook(void (*hook)(struct rt_object *object));
void rt_object_take_sethook(void (*hook)(struct rt_object *object));
void rt_object_put_sethook(void (*hook)(struct rt_object *object));
#endif /* RT_USING_HOOK */
/** @} */

/**
 * @addtogroup group_clock_management
 * @{
 */

/**
 * @name 系统节拍与定时器服务
 * 刻度值使用无符号环绕算术。 应通过提供的助手来比较持续时间，而不是假设计数器永远不会换行。对于硬定时器，定时器回调通常在中断上下文中运行；对于软定时器，定时器回调通常在定时器服务线程中运行。 对于 RT_USING_TIMER_ALL_SOFT，工作线程调度每个定时器，无论其 HARD 标志如何。
 */
/** 返回由时钟中断维护的当前系统节拍。 */
rt_tick_t rt_tick_get(void);
/** 使用环绕安全无符号减法返回自 @p base 以来经过的刻度。 */
rt_tick_t rt_tick_get_delta(rt_tick_t base);
/** 更换系统滴答计数器；用于时钟/平台管理。 */
void rt_tick_set(rt_tick_t tick);
/**
 * 前进一刻度，更新当前时间片，并检查到期定时器。
 *
 * 常见的实现断言中断嵌套不为零，因此时钟 ISR 必须仅在 rt_interrupt_enter() 之后调用此函数。
 */
void rt_tick_increase(void);
/**
 * 将系统时钟提前指定的刻度数。
 *
 * 与rt_tick_increase()一样，常见的实现需要由rt_interrupt_enter()建立的ISR上下文。
 */
void rt_tick_increase_tick(rt_tick_t tick);
/**
 * 将毫秒转换为刻度，将非整数正持续时间向上舍入；负输入变为 RT_WAITING_FOREVER 位模式。
 */
rt_tick_t  rt_tick_from_millisecond(rt_int32_t ms);
/**
 * 返回转换为毫秒的系统正常运行时间。 仅当 RT_TICK_PER_SECOND 除 1000 时，弱默认值才是精确的；否则，它会发出构建警告并返回零，除非 BSP 提供更高精度的覆盖。
 */
rt_tick_t rt_tick_get_millisecond(void);
#ifdef RT_USING_HOOK
/**
 * 安装在 tick、时间片和定时器记账之前调用的钩子，通常运行在 ISR 上下文中。`rt_tick_increase_tick(n)` 对整个批次只调用一次钩子，而不是对 `n` 表示的每一个 tick 分别调用。
 */
void rt_tick_sethook(void (*hook)(void));
#endif /* RT_USING_HOOK */

/**
 * 在调度器启动之前初始化硬定时器排序列表；当 RT_USING_TIMER_ALL_SOFT 删除硬定时器列表时，这是一个无操作。
 */
void rt_system_timer_init(void);
/**
 * 当RT_USING_TIMER_SOFT使能时，初始化并启动软定时器工作者和信号量；否则，公共函数是无操作的。
 */
void rt_system_timer_thread_init(void);

/**
 * 在调用者拥有的存储中初始化静态定时器。
 *
 * @param timer 定时器控制块在分离之前保持有效。
 * @param name 用于诊断和查找的对象名称。
 * @param timeout 到期时调用回调。
 * @param parameter 不透明的回调参数。
 * @param time 初始相对超时（以刻度为单位）。
 * @param flag ONE_SHOT/PERIODIC 和 HARD/SOFT 策略位。
 */
void rt_timer_init(rt_timer_t  timer,
                   const char *name,
                   void (*timeout)(void *parameter),
                   void       *parameter,
                   rt_tick_t   time,
                   rt_uint8_t  flag);
/** 停止并注销静态定时器；不会释放调用者提供的存储空间。 */
rt_err_t rt_timer_detach(rt_timer_t timer);
#ifdef RT_USING_HEAP
/** 分配并初始化一个动态定时器；失败时返回 NULL。 */
rt_timer_t rt_timer_create(const char *name,
                           void (*timeout)(void *parameter),
                           void       *parameter,
                           rt_tick_t   time,
                           rt_uint8_t  flag);
/** 停止、取消注册并释放由 rt_timer_create() 创建的定时器。 */
rt_err_t rt_timer_delete(rt_timer_t timer);
#endif /* RT_USING_HEAP */
/** 相对于当前刻度线布防或重启 @p timer。 */
rt_err_t rt_timer_start(rt_timer_t timer);
/** 停止 @p timer；是否成功取决于定时器当前所处的状态。 */
rt_err_t rt_timer_stop(rt_timer_t timer);
/** 执行RT_TIMER_CTRL_*命令； @p arg 类型取决于 @p cmd。 */
rt_err_t rt_timer_control(rt_timer_t timer, int cmd, void *arg);
/** 返回活动硬定时器和软定时器中最早的绝对截止时间。 */
rt_tick_t rt_timer_next_timeout_tick(void);
/**
 * 在调用 rt_interrupt_enter() 之后，由 tick ISR 检查定时器。通常它会直接分派到期的硬定时器，并通知软定时器工作线程。启用 RT_USING_TIMER_ALL_SOFT 时，本函数只唤醒该工作线程，不会在 ISR 中直接分派带有 HARD 标志的回调。
 */
void rt_timer_check(void);
#ifdef RT_USING_HOOK
/**
 * 在每个定时器回调之前和之后立即安装回调。
 *
 * 钩子接收正在调度的定时器，并在与其 callback 相同的上下文中运行（通常为硬线程为 ISR，软线程为工作线程；所有回调都使用 RT_USING_TIMER_ALL_SOFT 下的工作线程）。 它们不得在实际中断上下文中阻塞或使调度下的定时器无效。
 */
void rt_timer_enter_sethook(void (*hook)(struct rt_timer *timer));
void rt_timer_exit_sethook(void (*hook)(struct rt_timer *timer));
#endif /* RT_USING_HOOK */
/** @} */

/**@}*/

/**
 * @name 线程生命周期与执行控制
 * 新初始化/创建的线程处于 INIT 状态，直到 rt_thread_startup() 才会执行。 数字越小，优先级越高。 @p tick是用于等优先级线程之间循环调度的时间片。静态和动态生命周期 API 不得混合。
 */
/**
 * 初始化静态 TCB 和调用者提供的栈。
 *
 * @param thread 调用者拥有的 TCB。
 * @param name 诊断对象名称。
 * @param entry 线程体；从它返回时遵循正常的退出路径。
 * @param parameter 不透明参数传递给 @p entry。
 * @param stack_start 可写栈存储的基地址。
 * @param stack_size @p stack_start 提供栈字节。
 * @param priority 配置范围内的初始调度器优先级。
 * @param tick 调度器tick 数中的初始时间片长度。
 */
rt_err_t rt_thread_init(struct rt_thread *thread,
                        const char       *name,
                        void (*entry)(void *parameter),
                        void             *parameter,
                        void             *stack_start,
                        rt_uint32_t       stack_size,
                        rt_uint8_t        priority,
                        rt_uint32_t       tick);
/**
 * 关闭静态分配的线程并安排任何所需的延迟清理； TCB 和栈存储仍然由调用者拥有。
 */
rt_err_t rt_thread_detach(rt_thread_t thread);
#ifdef RT_USING_HEAP
/** 分配一个TCB和栈，然后初始化一个处于INIT状态的动态线程。 */
rt_thread_t rt_thread_create(const char *name,
                             void (*entry)(void *parameter),
                             void       *parameter,
                             rt_uint32_t stack_size,
                             rt_uint8_t  priority,
                             rt_uint32_t tick);
/** 关闭动态线程并安排延迟的 TCB/栈回收。 */
rt_err_t rt_thread_delete(rt_thread_t thread);
#endif /* RT_USING_HEAP */
/**
 * 将 @p thread 转换为 CLOSE 状态的底层操作。
 *
 * 这会删除调度器成员身份并分离嵌入的超时定时器，但本身不会取消注册/释放 TCB 或执行完整的互斥和延迟回收路径。 普通调用者应使用匹配的 rt_thread_detach()/rt_thread_delete() 生命周期 API。 关闭当前线程需要调用者按照实现记录阻止调度。
 */
rt_err_t rt_thread_close(rt_thread_t thread);
/** 返回调度器的当前线程，或在选择线程之前返回 NULL。 */
rt_thread_t rt_thread_self(void);
/**
 * 将 @p name 截断为 RT_NAME_MAX 后找到全局注册线程 - 1. 返回的指针被借用；仅从线程上下文调用。
 */
rt_thread_t rt_thread_find(char *name);
/** 将 INIT 线程移动到就绪集中并根据需要请求调度。 */
rt_err_t rt_thread_startup(rt_thread_t thread);
/** 将当前线程的剩余切片放弃给同等优先级的对等点。 */
rt_err_t rt_thread_yield(void);
/**
 * 将当前线程延迟最多 @p tick 调度器周期；零无效。当信号处理唤醒它时，可中断等待可能会提前返回。
 */
rt_err_t rt_thread_delay(rt_tick_t tick);
/**
 * 延迟到定期计划中的下一个绝对点。
 *
 * @p tick 保存调用者上一次的周期释放点。函数先把它增加 @p inc_tick：若新的释放点仍在未来，当前线程便睡眠到该时刻；若截止点已经错过，则把 @p tick 重置为当前时刻并立即返回。这样既可避免周期任务累积漂移，也能在任务超期后重新建立时间基准。
 */
rt_err_t rt_thread_delay_until(rt_tick_t *tick, rt_tick_t inc_tick);
/** 将 @p ms 转换为刻度并延迟当前线程。 */
rt_err_t rt_thread_mdelay(rt_int32_t ms);
/** 执行RT_THREAD_CTRL_*请求； @p arg 取决于 @p cmd。 */
rt_err_t rt_thread_control(rt_thread_t thread, int cmd, void *arg);
/**
 * 使用默认的不可中断策略挂起 @p thread。普通应用通常只应挂起当前线程；若异步挂起其他线程，可能把它冻结在持锁或操作共享资源的中间状态。正在另一个 CPU 上运行的线程也不是有效目标。
 */
rt_err_t rt_thread_suspend(rt_thread_t thread);
/** 使用 RT_INTERRUPTIBLE/KILLABLE/UNINTERRUPTIBLE 策略挂起 @p thread。 */
rt_err_t rt_thread_suspend_with_flag(rt_thread_t thread, int suspend_flag);
/** 将挂起的线程从等待状态中移除并使其准备就绪。 */
rt_err_t rt_thread_resume(rt_thread_t thread);
#ifdef RT_USING_SMART
/**
 * 取出并清除线程的一次性唤醒适配器，然后调用该适配器；若未安装适配器，则回退为调用 rt_thread_resume()。
 */
rt_err_t rt_thread_wakeup(rt_thread_t thread);
/** 安装 rt_thread_wakeup() 使用的等待对象适配器和不透明对象。 */
void rt_thread_wakeup_set(struct rt_thread *thread, rt_wakeup_func_t func, void* user_data);
#endif /* RT_USING_SMART */
/** 将线程名称复制到有界的调用者缓冲区中。 */
rt_err_t rt_thread_get_name(rt_thread_t thread, char *name, rt_uint8_t name_size);
#ifdef RT_USING_CPU_USAGE_TRACER
/** 返回 @p thread 最近采样的利用率百分比。 */
rt_uint8_t rt_thread_get_usage(rt_thread_t thread);
#endif /* RT_USING_CPU_USAGE_TRACER */
#ifdef RT_USING_SIGNALS
/** 为 @p tid 分配并初始化经典信号处理函数向量。 */
void rt_thread_alloc_sig(rt_thread_t tid);
/** 发布@p tid相关经典信号资源。 */
void rt_thread_free_sig(rt_thread_t tid);
/** 将信号@p sig排队到@p tid，并在信号策略允许时将其唤醒。 */
int  rt_thread_kill(rt_thread_t tid, int sig);
#endif /* RT_USING_SIGNALS */
#ifdef RT_USING_HOOK
/**
 * 安装生命周期跟踪钩子。 挂起钩子仅在成功的挂起转换后运行；即使其就绪操作报告错误，恢复钩子也会在恢复流之后到达，因此它不是一般的成功通知。
 *
 * 回调同步运行，同时调度器/IPC 状态仍可能受到保护。 它们用于追踪；它们必须是有界的、非阻塞的，并且不能递归地挂起/恢复线程。
 */
void rt_thread_suspend_sethook(void (*hook)(rt_thread_t thread));
void rt_thread_resume_sethook (void (*hook)(rt_thread_t thread));

/**
 * @ingroup group_thread_management
 *
 * @brief 多侦听器线程初始化钩子的处理函数类型。
 *
 * @param thread 新初始化的 TCB。 线程不一定是
 * 启动并且指针是从其所有者那里借用的。
 *
 * 处理函数在初始化调用者的上下文中执行，应该用于观察/检测，而不是更改调度器可见的字段。
 */
typedef void (*rt_thread_inited_hookproto_t)(rt_thread_t thread);
RT_OBJECT_HOOKLIST_DECLARE(rt_thread_inited_hookproto_t, rt_thread_inited);

#endif /* RT_USING_HOOK */
/** @} */

/**
 * @name 空闲线程服务
 * 每个 CPU 有一个优先级最低的空闲线程。 公共钩子表由主CPU的通用空闲循环遍历；辅助 SMP 空闲循环通常直接调用架构空闲例程。 钩子可能会在附近执行电源管理或延迟清理工作。
 */
/** 在内核启动期间创建并绑定每个 CPU 的空闲线程。 */
void rt_thread_idle_init(void);
#if defined(RT_USING_HOOK) || defined(RT_USING_IDLE_HOOK)
/**
 * @ingroup group_thread_management
 *
 * @brief 注册从每个主 CPU 通用空闲迭代调用的函数。
 *
 * @param hook 函数放置在第一个空闲表槽中。 实施情况
 * 不拒绝NULL或重复注册。 NULL 返回 RT_EOK，但将所选插槽保留为空且可重用；重复的非 NULL 函数可以占用多个插槽并重复运行。
 *
 * @return `RT_EOK`：设置成功。
 * `-RT_EFULL`：钩子列表已满。
 *
 * @note 回调在主 CPU 的空闲线程循环中执行，必须是
 * 回调必须短小，绝不能阻塞或挂起，也不应长时间忙循环，因为它会直接影响节能和最低优先级维护任务。在常见的 SMP 实现中，辅助 CPU 直接执行架构空闲例程，并不遍历此钩子表。
 */
rt_err_t rt_thread_idle_sethook(void (*hook)(void));
/** 删除之前注册的空闲钩子。 */
rt_err_t rt_thread_idle_delhook(void (*hook)(void));
#endif /* defined(RT_USING_HOOK) || defined(RT_USING_IDLE_HOOK) */
/** 返回当前CPU的空闲线程。 */
rt_thread_t rt_thread_idle_gethandler(void);
/** 当 @p thread 是系统的 per-CPU 空闲线程之一时返回 true。 */
rt_bool_t rt_thread_is_idle_thread(rt_thread_t thread);
/** @} */

/**
 * @name 调度器服务
 * 本组多数函数是内核与 BSP 的集成点，而不是普通应用 API。调度临界区用于阻止线程切换，并且可以嵌套；在 SMP 系统中，它不等同于保护任意共享数据的普通锁。
 */
/** 初始化就绪队列、位图、锁和每个 CPU 的调度状态。 */
void rt_system_scheduler_init(void);
/** 选择第一个就绪线程，并执行不会返回启动代码的首次上下文切换。 */
void rt_system_scheduler_start(void);

/** 重新评估最高优先级的就绪线程并根据需要进行切换。 */
void rt_schedule(void);
/**
 * 在中断返回路径上完成延迟的 SMP 切换。 通用实现仅存在于SMP调度器中； UP 移植层不得引用此声明，除非它们提供自己的实现。
 */
void rt_scheduler_do_irq_switch(void *context);

#ifdef RT_USING_OVERFLOW_CHECK
/** 验证 @p thread 的栈哨兵/边界并在失败时调用溢出策略。 */
void rt_scheduler_stack_check(struct rt_thread *thread);

/** 当启用溢出检查时，将栈检查编译到上下文切换路径中。 */
#define RT_SCHEDULER_STACK_CHECK(thr) rt_scheduler_stack_check(thr)

#else /* !RT_USING_OVERFLOW_CHECK */

#define RT_SCHEDULER_STACK_CHECK(thr)

#endif /* RT_USING_OVERFLOW_CHECK */

/**
 * 进入可嵌套的调度器临界区并返回新的嵌套层级；在尚无当前线程的 SMP 启动阶段调用会返回 -RT_EINVAL。
 */
rt_base_t rt_enter_critical(void);
/** 退出一层调度器临界区；退出最外层时执行已经推迟的调度。 */
void rt_exit_critical(void);
/**
 * 退出一层嵌套；@p critical_level 仅用于在调试构建中验证它等于配对进入调用所返回的当前层级。该参数不能把嵌套深度恢复到任意旧值，非调试构建会忽略它。
 */
void rt_exit_critical_safe(rt_base_t critical_level);
/** 返回当前CPU的调度器临界区嵌套深度。 */
rt_uint16_t rt_critical_level(void);

#ifdef RT_USING_HOOK
/**
 * 安装调度器诊断钩子。
 *
 * 栈检查失败时会调用溢出钩子，并使用其返回值作为处理策略。调度器钩子在切换前观察选中的 from/to 线程对；switch 钩子在请求底层上下文切换时接收即将换出的当前线程。这些钩子都运行在限制严格的调度路径中，必须非阻塞、执行时间有界，并且不能调用可能再次触发调度的操作。
 */
void rt_scheduler_stack_overflow_sethook(rt_err_t (*hook)(struct rt_thread *thread));
void rt_scheduler_sethook(void (*hook)(rt_thread_t from, rt_thread_t to));
void rt_scheduler_switch_sethook(void (*hook)(struct rt_thread *tid));
#endif /* RT_USING_HOOK */

#ifdef RT_USING_SMP
/** 辅助 CPU 完成低级启动后使用的架构条目。 */
void rt_secondary_cpu_entry(void);
/** 处理与调度器相关的处理器间中断。 */
void rt_scheduler_ipi_handler(int vector, void *param);
#endif /* RT_USING_SMP */
/** @} */

/**
 * @addtogroup group_signal
 * @{
 */
#ifdef RT_USING_SIGNALS
/** 将经典信号 @p signo 块传送到当前线程。 */
void rt_signal_mask(int signo);
/** 解锁经典信号 @p signo 到当前线程的传递。 */
void rt_signal_unmask(int signo);
/** 检查待处理信号并根据需要准备架构返回上下文。 */
void *rt_signal_check(void* context);
/** 安装经典处理函数并返回之前的处理函数。 */
rt_sighandler_t rt_signal_install(int signo, rt_sighandler_t handler);
/** 等待直到@p set中的一个信号到达或@p timeout到期。 */
int rt_signal_wait(const rt_sigset_t *set, rt_siginfo_t *si, rt_int32_t timeout);
/** 在内核启动期间初始化全局经典信号支持。 */
int rt_system_signal_init(void);
#endif /* RT_USING_SIGNALS */
/**@}*/

/**
 * @addtogroup group_memory_management
 * @{
 */

/*
 * 内存 API 系列是独立的：
 * - rt_malloc() 使用单个配置的系统堆后端。
 * - rt_smem/rt_slab 管理显式分配器对象。
 * - rt_memheap 管理一个或多个可变大小的区域。
 * - rt_mempool 管理固定大小的块并可以等待返回的块。
 */
#ifdef RT_USING_MEMPOOL
/** 通过 [start, start + size) 初始化静态固定块池。 */
rt_err_t rt_mp_init(struct rt_mempool *mp,
                    const char        *name,
                    void              *start,
                    rt_size_t          size,
                    rt_size_t          block_size);
/** 在所有使用者和等待线程都不再使用静态内存池后，将其分离。 */
rt_err_t rt_mp_detach(struct rt_mempool *mp);
#ifdef RT_USING_HEAP
/** 为 @p block_size 的 @p block_count 块分配池元数据/存储。 */
rt_mp_t rt_mp_create(const char *name,
                     rt_size_t   block_count,
                     rt_size_t   block_size);
/** 当没有未完成的块时，删除动态创建的池。 */
rt_err_t rt_mp_delete(rt_mp_t mp);
#endif /* RT_USING_HEAP */
/** 获取一个内存块，最多等待 @p time 个 tick；超时或失败时返回 NULL。 */
void *rt_mp_alloc(rt_mp_t mp, rt_int32_t time);
/** 将块归还到其所属内存池，并在存在等待线程时唤醒一个等待者。 */
void rt_mp_free(void *block);
#ifdef RT_USING_HOOK
/**
 * 安装内存池分配/释放观察钩子。
 *
 * 仅在获得块并接收池和非 NULL 结果后才调用分配钩子；分配失败在钩子之前返回。 空闲钩子在重新插入之前观察所属池和返回的块。 两者都在池自旋锁之外运行，但继承调用者的上下文，并且不得递归分配/释放相同的池或块。
 */
void rt_mp_alloc_sethook(void (*hook)(struct rt_mempool *mp, void *block));
void rt_mp_free_sethook(void (*hook)(struct rt_mempool *mp, void *block));
#endif /* RT_USING_HOOK */

#endif /* RT_USING_MEMPOOL */

#ifdef RT_USING_HEAP
/** 通过[begin_addr、end_addr)初始化配置的系统堆。 */
void rt_system_heap_init(void *begin_addr, void *end_addr);
/** 默认弱包装器使用的后端中立系统堆初始值设定项。 */
void rt_system_heap_init_generic(void *begin_addr, void *end_addr);

/** 分配至少 @p size 字节，或返回 NULL。 */
void *rt_malloc(rt_size_t size);
/** 释放系统分配器返回的指针； NULL 已接受。 */
void rt_free(void *ptr);
/** 调整分配大小，同时保留旧/新有效负载大小的最小值。 */
void *rt_realloc(void *ptr, rt_size_t newsize);
/**
 * 分配 `count * size` 字节并全部清零。当前通用实现不检查乘法溢出；处理不可信尺寸的调用者必须在调用前验证乘积不会溢出。
 */
void *rt_calloc(rt_size_t count, rt_size_t size);
/** 使用所请求的二次方对齐方式分配 @p size 字节。 */
void *rt_malloc_align(rt_size_t size, rt_size_t align);
/** 释放由rt_malloc_align()专门返回的内存。 */
void rt_free_align(void *ptr);

/** 当指针为非 NULL 时，返回托管字节总数、当前使用字节数和高水位字节数。 */
void rt_memory_info(rt_size_t *total,
                    rt_size_t *used,
                    rt_size_t *max_used);

#if defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP)
/** 从 slab 页分配器中分配 @p npages 个连续页面。 */
void *rt_page_alloc(rt_size_t npages);
/** 把从页对齐 slab 地址开始的 @p npages 个页面归还给分配器。 */
void rt_page_free(void *addr, rt_size_t npages);
#endif /* defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP) */

/**
 * @ingroup group_hook
 * @{
 */

#ifdef RT_USING_HOOK
/**
 * 安装系统堆跟踪钩子。
 *
 * `void **ptr` 在钩子点公开活动指针变量：malloc 和 realloc-exit 接收结果，realloc-entry 接收旧指针，free 接收目标指针。 通过此参数写入的钩子会更改随后返回、重新分配或释放的值；因此，观察钩子应该保持不变。 通用包装器在其外部堆锁之外调用这些钩子，但它们仍然继承分配调用者的线程/ISR 上下文。 它们不得递归分配、使用分配的格式化路径或执行不安全的阻塞服务。
 */
void rt_malloc_sethook(void (*hook)(void **ptr, rt_size_t size));
void rt_realloc_set_entry_hook(void (*hook)(void **ptr, rt_size_t size));
void rt_realloc_set_exit_hook(void (*hook)(void **ptr, rt_size_t size));
void rt_free_sethook(void (*hook)(void **ptr));
#endif /* RT_USING_HOOK */
/**@}*/

#endif /* RT_USING_HEAP */

#ifdef RT_USING_SMALL_MEM
/**
 * @name 小内存分配器对象 API
 *
 * 该对象使用适合相对较小的连续区域的紧凑地址排序的首次适应分配器。 返回的指针必须由匹配的 rt_smem_free() 实现释放。
 * @{
 */
/** 在 @p begin_addr 处初始化 @p size 字节的小内存分配器。 */
rt_smem_t rt_smem_init(const char    *name,
                     void          *begin_addr,
                     rt_size_t      size);
/** 释放所有分配后，分离分配器对象。 */
rt_err_t rt_smem_detach(rt_smem_t m);
/** 从 @p m 分配 @p size 字节。 */
void *rt_smem_alloc(rt_smem_t m, rt_size_t size);
/** 在其拥有的小内存分配器中调整 @p rmem 的大小。 */
void *rt_smem_realloc(rt_smem_t m, void *rmem, rt_size_t newsize);
/** 释放小内存分配；所有权元数据标识其分配者。 */
void rt_smem_free(void *rmem);
/** @} */
#endif /* RT_USING_SMALL_MEM */

#ifdef RT_USING_MEMHEAP
/**
 * @name 内存堆对象 API
 *
 * 内存堆提供来自显式内存区域的可变大小分配，并且在被选为系统堆时可以与其他内存堆组合。
 * @{
 */
/** 在调用者提供的区域上初始化 @p memheap。 */
rt_err_t rt_memheap_init(struct rt_memheap *memheap,
                         const char        *name,
                         void              *start_addr,
                         rt_size_t         size);
/** 从对象/系统堆管理中分离出一个空内存堆。 */
rt_err_t rt_memheap_detach(struct rt_memheap *heap);
/** 从特定内存堆分配。 */
void *rt_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
/** 调整 @p heap 拥有的分配大小。 */
void *rt_memheap_realloc(struct rt_memheap *heap, void *ptr, rt_size_t newsize);
/** 释放内存堆分配；块头记录了所属堆。 */
void rt_memheap_free(void *ptr);
/** 查询一个内存堆的总字节数、已用字节数和高水位字节数。 */
void rt_memheap_info(struct rt_memheap *heap,
                     rt_size_t *total,
                     rt_size_t *used,
                     rt_size_t *max_used);
/** @} */
#endif /* RT_USING_MEMHEAP */

#ifdef RT_USING_MEMHEAP_AS_HEAP
/**
 * 在外部系统堆锁后面使用的内部未锁定内存堆操作。应用程序应使用 rt_malloc()/rt_free()/rt_realloc() 代替。
 */
void *_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
void _memheap_free(void *rmem);
void *_memheap_realloc(struct rt_memheap *heap, void *rmem, rt_size_t newsize);
#endif

#ifdef RT_USING_SLAB
/**
 * @name Slab 分配器对象 API
 *
 * 分配器将大型请求的页面分配与小型请求的大小级别区域结合起来。 所有地址必须返回到分配它们的同一个slab对象。
 * @{
 */
/** 在页面对齐的可用区域上初始化一个slab分配器。 */
rt_slab_t rt_slab_init(const char *name, void *begin_addr, rt_size_t size);
/** 分离未使用的slab分配器。 */
rt_err_t rt_slab_detach(rt_slab_t m);
/** 从特定的slab对象分配连续的页面。 */
void *rt_slab_page_alloc(rt_slab_t m, rt_size_t npages);
/** 将连续页面返回到特定的slab对象。 */
void rt_slab_page_free(rt_slab_t m, void *addr, rt_size_t npages);
/** 通过slab size-class/page策略分配字节大小的请求。 */
void *rt_slab_alloc(rt_slab_t m, rt_size_t size);
/** 调整slab分配的大小，同时保留有效负载数据。 */
void *rt_slab_realloc(rt_slab_t m, void *ptr, rt_size_t size);
/** 释放对其slab对象的分配。 */
void rt_slab_free(rt_slab_t m, void *ptr);
/** @} */
#endif /* RT_USING_SLAB */

/**@}*/

/**
 * @addtogroup group_thread_comm
 * @{
 */

/**
 * @name 内部挂起列表原语
 *
 * 这些函数构成了 IPC 对象的等待队列和调度器之间的桥梁。 排队以原子方式将线程从运行/就绪更改为挂起，并可选择按优先级对其进行排序。 出队会停止嵌入的超时定时器，删除线程，分配其唤醒错误，并使其准备就绪。 它们是内核构建块，而不是应用程序 API。
 * @{
 */
/** 打印挂起列表以进行诊断，而不更改其拓扑。 */
void rt_susp_list_print(rt_list_t *list);
/** 哨兵告诉出队/恢复助手保留线程的现有错误。 */
#define RT_THREAD_RESUME_RES_THR_ERR (-1)
/** 从等待队列移除并就绪第一个等待线程；队列为空时返回 NULL。 */
struct rt_thread *rt_susp_list_dequeue(rt_list_t *susp_list, rt_err_t thread_error);
/** 就绪全部等待线程，并为每个线程设置或保留 @p thread_error。 */
rt_err_t rt_susp_list_resume_all(rt_list_t *susp_list, rt_err_t thread_error);
/** 恢复所有等待程序，同时协调已经相关的 IPC 自旋锁。 */
rt_err_t rt_susp_list_resume_all_irq(rt_list_t *susp_list,
                                     rt_err_t thread_error,
                                     struct rt_spinlock *lock);

/** 原子挂起 @p thread 并使用等待顺序/策略将其排入队列。 */
rt_err_t rt_thread_suspend_to_list(rt_thread_t thread, rt_list_t *susp_list, int ipc_flags, int suspend_flag);
/** 将已经挂起的线程加入队列；调用者必须持有调度器锁。 */
rt_err_t rt_susp_list_enqueue(rt_list_t *susp_list, rt_thread_t thread, int ipc_flags);
/** @} */

/**
 * @addtogroup group_semaphore Semaphore
 * @{
 */

#ifdef RT_USING_SEMAPHORE
/** 使用 @p value 和 FIFO/优先级等待顺序初始化静态信号量。 */
rt_err_t rt_sem_init(rt_sem_t    sem,
                     const char *name,
                     rt_uint32_t value,
                     rt_uint8_t  flag);
/** 分离静态信号量，根据 IPC 重置的定义唤醒/使等待者失效。 */
rt_err_t rt_sem_detach(rt_sem_t sem);
#ifdef RT_USING_HEAP
/** 创建动态分配的信号量。 */
rt_sem_t rt_sem_create(const char *name, rt_uint32_t value, rt_uint8_t flag);
/** 删除动态信号量并释放其对象存储。 */
rt_err_t rt_sem_delete(rt_sem_t sem);
#endif /* RT_USING_HEAP */

/** 获取一个代币，必要时等待 @p timeout 滴答声。 */
rt_err_t rt_sem_take(rt_sem_t sem, rt_int32_t timeout);
/** 获取时需要等待，普通信号可能会中断。 */
rt_err_t rt_sem_take_interruptible(rt_sem_t sem, rt_int32_t timeout);
/** 通过等待获取，只有终止级信号可能会中断。 */
rt_err_t rt_sem_take_killable(rt_sem_t sem, rt_int32_t timeout);
/** 尝试立即获取而不阻塞。 */
rt_err_t rt_sem_trytake(rt_sem_t sem);
/** 返回一个令牌或将其直接传输到等待线程。 */
rt_err_t rt_sem_release(rt_sem_t sem);
/** 对信号量执行通用 RT_IPC_CMD_* 操作。 */
rt_err_t rt_sem_control(rt_sem_t sem, int cmd, void *arg);
#endif /* RT_USING_SEMAPHORE */

/**@}*/

/**
 * @addtogroup group_mutex Mutex
 * @{
 */

#ifdef RT_USING_MUTEX
/** 初始化静态递归互斥量；等待线程按照优先级排序。 */
rt_err_t rt_mutex_init(rt_mutex_t mutex, const char *name, rt_uint8_t flag);
/** 分离未使用的静态互斥锁。 */
rt_err_t rt_mutex_detach(rt_mutex_t mutex);
#ifdef RT_USING_HEAP
/** 创建动态分配的递归互斥体。 */
rt_mutex_t rt_mutex_create(const char *name, rt_uint8_t flag);
/** 删除未使用的动态互斥体。 */
rt_err_t rt_mutex_delete(rt_mutex_t mutex);
#endif /* RT_USING_HEAP */
/** 内部退出/恢复助手，删除 @p thread 的所有权关系。 */
void rt_mutex_drop_thread(rt_mutex_t mutex, rt_thread_t thread);
/** 设置优先级上限并返回之前的上限。 */
rt_uint8_t rt_mutex_setprioceiling(rt_mutex_t mutex, rt_uint8_t priority);
/** 返回当前配置的优先级上限。 */
rt_uint8_t rt_mutex_getprioceiling(rt_mutex_t mutex);

/** 递归获取或阻止最多 @p timeout 的优先级继承。 */
rt_err_t rt_mutex_take(rt_mutex_t mutex, rt_int32_t timeout);
/** 尝试立即获取而不阻塞。 */
rt_err_t rt_mutex_trytake(rt_mutex_t mutex);
/** 使用可被普通信号中断的等待来获取。 */
rt_err_t rt_mutex_take_interruptible(rt_mutex_t mutex, rt_int32_t time);
/** 使用仅可被终止级信号中断的等待来获取。 */
rt_err_t rt_mutex_take_killable(rt_mutex_t mutex, rt_int32_t time);
/** 递减递归保持；在最终版本上转让所有权或解锁。 */
rt_err_t rt_mutex_release(rt_mutex_t mutex);
/** 对互斥体执行通用 RT_IPC_CMD_* 操作。 */
rt_err_t rt_mutex_control(rt_mutex_t mutex, int cmd, void *arg);

/** 返回当前所有者作为借用指针；结果可以同时改变。 */
rt_inline rt_thread_t rt_mutex_get_owner(rt_mutex_t mutex)
{
    return mutex->owner;
}
/** 返回当前递归保持深度；结果可以同时改变。 */
rt_inline rt_ubase_t rt_mutex_get_hold(rt_mutex_t mutex)
{
    return mutex->hold;
}

#endif /* RT_USING_MUTEX */

/**@}*/

/**
 * @addtogroup group_event Event
 * @{
 */

#ifdef RT_USING_EVENT
/** 使用 FIFO/优先级等待顺序初始化静态 32 位事件对象。 */
rt_err_t rt_event_init(rt_event_t event, const char *name, rt_uint8_t flag);
/** 解析静态事件对象的等待者后，分离该对象。 */
rt_err_t rt_event_detach(rt_event_t event);
#ifdef RT_USING_HEAP
/** 创建动态分配的事件对象。 */
rt_event_t rt_event_create(const char *name, rt_uint8_t flag);
/** 删除动态事件对象。 */
rt_err_t rt_event_delete(rt_event_t event);
#endif /* RT_USING_HEAP */

/** OR @p set 进入事件状态并唤醒每个条件匹配的接收器。 */
rt_err_t rt_event_send(rt_event_t event, rt_uint32_t set);
/**
 * 接收 AND/OR 事件条件，可选择清除匹配位。
 * @p recved 返回满足条件的事件位。超时参数为零时不阻塞，取 RT_WAITING_FOREVER 时则无限期等待。
 */
rt_err_t rt_event_recv(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/** 其等待可能被普通信号中断的事件接收。 */
rt_err_t rt_event_recv_interruptible(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/** 其等待只能被终止级信号中断的事件接收。 */
rt_err_t rt_event_recv_killable(rt_event_t   event,
                       rt_uint32_t  set,
                       rt_uint8_t   opt,
                       rt_int32_t   timeout,
                       rt_uint32_t *recved);
/** 对事件对象执行通用 RT_IPC_CMD_* 操作。 */
rt_err_t rt_event_control(rt_event_t event, int cmd, void *arg);
#endif /* RT_USING_EVENT */

/**@}*/

/**
 * @addtogroup group_mailbox MailBox
 * @{
 */

#ifdef RT_USING_MAILBOX
/**
 * 通过 @p msgpool 中的 @p size rt_ubase_t 插槽初始化静态邮箱。
 *
 * 邮箱只复制一个指针宽度的值；该值所引用对象的所有权仍由发送者或应用程序管理。@p flag 选择 FIFO 或优先级等待顺序。
 */
rt_err_t rt_mb_init(rt_mailbox_t mb,
                    const char  *name,
                    void        *msgpool,
                    rt_size_t    size,
                    rt_uint8_t   flag);
/** 解决阻止的发件人/收件人后分离静态邮箱。 */
rt_err_t rt_mb_detach(rt_mailbox_t mb);
#ifdef RT_USING_HEAP
/** 创建邮箱并为 @p size 指针宽度消息分配存储空间。 */
rt_mailbox_t rt_mb_create(const char *name, rt_size_t size, rt_uint8_t flag);
/** 删除动态邮箱及其内部邮件存储。 */
rt_err_t rt_mb_delete(rt_mailbox_t mb);
#endif /* RT_USING_HEAP */

/** 立即发送；当邮箱已满时失败而不是等待。 */
rt_err_t rt_mb_send(rt_mailbox_t mb, rt_ubase_t value);
/**
 * 非阻塞可中断发送别名。 其固定的零超时可以防止暂停，因此可中断策略没有明显的效果。
 */
rt_err_t rt_mb_send_interruptible(rt_mailbox_t mb, rt_ubase_t value);
/**
 * 非阻塞可终止发送别名。 其固定的零超时可以防止暂停，因此可终止策略没有可观察到的效果。
 */
rt_err_t rt_mb_send_killable(rt_mailbox_t mb, rt_ubase_t value);
/** 发送，最多等待 @p timeout 时钟周期以获得空闲插槽。 */
rt_err_t rt_mb_send_wait(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/** 等待发送普通信号可能会中断。 */
rt_err_t rt_mb_send_wait_interruptible(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/** 等待发送只有kill-class信号才可能中断。 */
rt_err_t rt_mb_send_wait_killable(rt_mailbox_t mb,
                         rt_ubase_t  value,
                         rt_int32_t   timeout);
/** 在接收端插入@p value，以便它在正常消息之前返回。 */
rt_err_t rt_mb_urgent(rt_mailbox_t mb, rt_ubase_t value);
/** 将一个值接收到 @p value 中，最多等待 @p timeout 时钟周期。 */
rt_err_t rt_mb_recv(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/** 接收时会等待普通信号可能会中断的情况。 */
rt_err_t rt_mb_recv_interruptible(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/** 等待接收，只有终止级信号可能会中断。 */
rt_err_t rt_mb_recv_killable(rt_mailbox_t mb, rt_ubase_t *value, rt_int32_t timeout);
/** 对邮箱执行通用 RT_IPC_CMD_* 操作。 */
rt_err_t rt_mb_control(rt_mailbox_t mb, int cmd, void *arg);
#endif /* RT_USING_MAILBOX */

/**@}*/

/**
 * @addtogroup group_messagequeue Message Queue
 * @{
 */
#ifdef RT_USING_MESSAGEQUEUE

/**
 * @brief 私有标头前置到每个固定容量消息节点。
 *
 * next链接排队的FIFO/优先级链或自由节点链； length 记录有效负载中的有效字节。 应用程序应使用 RT_MQ_BUF_SIZE() 而不是手动构建此布局。
 */
struct rt_mq_message
{
    struct rt_mq_message *next; /**< 队列/空闲链中的下一个内部节点。 */
    rt_ssize_t length;          /**< 复制到此节点的有效负载长度。 */
#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
    rt_int32_t prio;            /**< 用于有序插入的消息优先级。 */
#endif /* RT_USING_MESSAGEQUEUE_PRIORITY */
};

/**
 * 返回 @p max_msgs 有效负载静态队列所需的字节。
 *
 * 在添加其私有标头之前，每个有效负载都会向上舍入为 RT_ALIGN_SIZE。参数在结果表达式中求值，不应产生副作用。 调用者还必须确保乘法不会溢出 rt_size_t，并且必须提供适当对齐的存储。
 */
#define RT_MQ_BUF_SIZE(msg_size, max_msgs) \
((RT_ALIGN((msg_size), RT_ALIGN_SIZE) + sizeof(struct rt_mq_message)) * (max_msgs))

/**
 * 初始化 @p pool_size 字节的静态固定消息队列。
 *
 * @p msg_size 是每条消息最多复制的负载字节数；@p pool_size 决定能从 @p msgpool 中划分出多少个对齐的“消息头 + 负载”节点。
 */
rt_err_t rt_mq_init(rt_mq_t     mq,
                    const char *name,
                    void       *msgpool,
                    rt_size_t   msg_size,
                    rt_size_t   pool_size,
                    rt_uint8_t  flag);
/** 解决阻塞的发送者/接收者后分离静态队列。 */
rt_err_t rt_mq_detach(rt_mq_t mq);
#ifdef RT_USING_HEAP
/** 创建一个具有 @p max_msgs 动态分配消息节点的队列。 */
rt_mq_t rt_mq_create(const char *name,
                     rt_size_t   msg_size,
                     rt_size_t   max_msgs,
                     rt_uint8_t  flag);
/** 删除动态消息队列及其节点存储。 */
rt_err_t rt_mq_delete(rt_mq_t mq);
#endif /* RT_USING_HEAP */

/** 立即复制一条消息并将其排入队列，当队列已满时失败。 */
rt_err_t rt_mq_send(rt_mq_t mq, const void *buffer, rt_size_t size);
/**
 * 非阻塞可中断发送别名。 它的零超时可以防止暂停，因此可中断策略没有明显的效果。
 */
rt_err_t rt_mq_send_interruptible(rt_mq_t mq, const void *buffer, rt_size_t size);
/**
 * 非阻塞可终止发送别名。 它的零超时可以防止暂停，因此可终止策略没有可观察到的效果。
 */
rt_err_t rt_mq_send_killable(rt_mq_t mq, const void *buffer, rt_size_t size);
/** 复制并发送，最多等待 @p timeout 的空闲节点。 */
rt_err_t rt_mq_send_wait(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/** 等待发送普通信号可能会中断。 */
rt_err_t rt_mq_send_wait_interruptible(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/** 等待发送只有kill-class信号才可能中断。 */
rt_err_t rt_mq_send_wait_killable(rt_mq_t     mq,
                         const void *buffer,
                         rt_size_t   size,
                         rt_int32_t  timeout);
/** 将复制的消息排在正常 FIFO 消息之前。 */
rt_err_t rt_mq_urgent(rt_mq_t mq, const void *buffer, rt_size_t size);
/** 接收到 @p buffer 并返回复制的字节或负错误。 */
rt_ssize_t rt_mq_recv(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/** 接收时会等待普通信号可能会中断的情况。 */
rt_ssize_t rt_mq_recv_interruptible(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/** 等待接收，只有终止级信号可能会中断。 */
rt_ssize_t rt_mq_recv_killable(rt_mq_t    mq,
                    void      *buffer,
                    rt_size_t  size,
                    rt_int32_t timeout);
/** 对队列执行通用 RT_IPC_CMD_* 操作。 */
rt_err_t rt_mq_control(rt_mq_t mq, int cmd, void *arg);

#ifdef RT_USING_MESSAGEQUEUE_PRIORITY
/** 使用显式消息 @p prio 和可配置的挂起策略进行排队。 */
rt_err_t rt_mq_send_wait_prio(rt_mq_t mq,
                              const void *buffer,
                              rt_size_t size,
                              rt_int32_t prio,
                              rt_int32_t timeout,
                              int suspend_flag);
/** 接收最高顺序的消息并可选择返回其优先级。 */
rt_ssize_t rt_mq_recv_prio(rt_mq_t mq,
                           void *buffer,
                           rt_size_t size,
                           rt_int32_t *prio,
                           rt_int32_t timeout,
                           int suspend_flag);
#endif /* RT_USING_MESSAGEQUEUE_PRIORITY */
#endif /* RT_USING_MESSAGEQUEUE */

/**@}*/

/**
 * @name 延迟线程回收
 * 线程不能释放退出路径当前仍在使用的栈。因此，关闭后的线程会先进入待回收队列，稍后由空闲线程（典型 UP 配置）或系统回收线程（SMP/RT-Smart 配置）完成资源释放。
 */
/** 初始化失效队列，并在需要时初始化其清理线程/信号量。 */
void rt_thread_defunct_init(void);
/** 将完全停止的线程放入队列以延迟清理。 */
void rt_thread_defunct_enqueue(rt_thread_t thread);
/** 从已失效队列中移除一个线程，或在空时返回 NULL。 */
rt_thread_t rt_thread_defunct_dequeue(void);
/** 执行回调并分离/释放每个可回收的失效线程。 */
void rt_defunct_execute(void);
/** @} */

/**
 * @name 自旋锁操作
 * 自旋锁保护短的非睡眠关键区域。 他们不允许在持有时进行阻挡。 irqsave 变体还禁用本地中断并返回必须提供给匹配恢复调用的确切先前中断级别。 锁定/解锁对必须在同一 CPU 上保持平衡。
 */
struct rt_spinlock;

/**
 * 初始化底层SMP硬件锁。 公共函数不会初始化可选的调试所有者/PC元数据，并且ZXQ​​0001QXZ实现是无操作的；零/静态初始化对于这些字段仍然很重要。
 */
void rt_spin_lock_init(struct rt_spinlock *lock);
/**
 * 进入调度器关键状态并获取 SMP 上的硬件锁。 在 UP 上，没有旋转或硬件锁：调用仅进入调度器临界区。 它不保存/恢复原始中断状态。
 */
void rt_spin_lock(struct rt_spinlock *lock);
/** 释放 rt_spin_lock() 获取的锁。 */
void rt_spin_unlock(struct rt_spinlock *lock);
/** 禁用本地中断，获取锁，并返回先前的中断状态。 */
rt_base_t rt_spin_lock_irqsave(struct rt_spinlock *lock);
/** 释放锁定并准确恢复@p level中的中断状态。 */
void rt_spin_unlock_irqrestore(struct rt_spinlock *lock, rt_base_t level);
/** @} */

/**@}*/

#ifdef RT_USING_DEVICE
/**
 * @addtogroup group_device_driver
 * @{
 */

/**
 * @name 通用设备对象 API
 * 包装器验证状态并分派到驱动程序的 rt_device_ops（或传统的直接回调）。 读/写计数和位置以设备类特定单位表示。 回调设置器安装借用的函数指针，驱动程序可以从中断上下文中调用它们。
 */
/**
 * 查找RT_NAME_MAX截断后的全局注册设备；返回借用的指针，并且必须从线程上下文中调用。
 */
rt_device_t rt_device_find(const char *name);

/** 使用能力/状态标志 @p flags 注册调用者拥有的设备。 */
rt_err_t rt_device_register(rt_device_t dev,
                            const char *name,
                            rt_uint16_t flags);
/** 从对象发现中删除设备；调用者仍然负责存储。 */
rt_err_t rt_device_unregister(rt_device_t dev);

#ifdef RT_USING_HEAP
/** 分配一个基本设备加上 @p attach_size 字节用于驱动程序私有扩展。 */
rt_device_t rt_device_create(int type, int attach_size);
/**
 * 释放rt_device_create()分配的存储空间。 该设备必须首先取消注册并满足设备核心的销毁对象先决条件；这不能替代 rt_device_unregister()。
 */
void rt_device_destroy(rt_device_t device);
#endif /* RT_USING_HEAP */

/** 安装上层接收就绪回调。 */
rt_err_t
rt_device_set_rx_indicate(rt_device_t dev,
                          rt_err_t (*rx_ind)(rt_device_t dev, rt_size_t size));
/** 安装上层异步发送完成回调。 */
rt_err_t
rt_device_set_tx_complete(rt_device_t dev,
                          rt_err_t (*tx_done)(rt_device_t dev, void *buffer));

/** 确保设备的驱动程序初始化操作已运行。 */
rt_err_t  rt_device_init (rt_device_t dev);
/**
 * 使用@p oflag打开，处理延迟初始化和引用计数。 -RT_ENOSYS 的驱动程序结果返回不变，但被视为打开的引用：内核设置 OPEN 并递增 ref_count。
 */
rt_err_t  rt_device_open (rt_device_t dev, rt_uint16_t oflag);
/** 删除打开的引用并在适当的时候调用驱动程序关闭操作。 */
rt_err_t  rt_device_close(rt_device_t dev);
/**
 * 调度读取并返回驱动程序的结果。 核心级闭设备或漏操作故障返回零并将原因放入errno；个体司机可以根据自己的合同使用负回报。
 */
rt_ssize_t rt_device_read(rt_device_t dev,
                          rt_off_t    pos,
                          void       *buffer,
                          rt_size_t   size);
/**
 * 调度写入并返回驱动程序的结果。 核心级闭设备或漏操作故障返回零并设置errno；特定于驾驶员的合同还可能返回负面状态。
 */
rt_ssize_t rt_device_write(rt_device_t dev,
                          rt_off_t    pos,
                          const void *buffer,
                          rt_size_t   size);
/** 调度一个通用/特定于类的命令，其参数类型取决于 @p cmd。 */
rt_err_t  rt_device_control(rt_device_t dev, int cmd, void *arg);
/** @} */

/**@}*/
#endif /* RT_USING_DEVICE */

/**
 * @name 中断与 Per-CPU 集成服务
 * rt_interrupt_enter()/leave() 由 BSP/libcpu ISR 包装器调用，而不是由应用程序代码调用。 他们更新嵌套和钩子；该架构的异常返回路径仍然负责完成延迟的上下文切换。 每一次进入都必须与同一个 CPU 上的一次离开配对。
 */
/** 增加当前 CPU 的中断嵌套计数，并运行中断进入钩子。 */
void rt_interrupt_enter(void);
/** 在异常返回之前运行离开钩子并减少中断嵌套。 */
void rt_interrupt_leave(void);

/**
 * 当 ARCH_USING_IRQ_CTX_LIST 启用时推送异常帧描述符。如果没有配置的实现，这些无条件声明本身不会提供可链接的服务。
 */
void rt_interrupt_context_push(rt_interrupt_context_t this_ctx);
/** 弹出最近推送的上下文；需要一个匹配的非空栈。 */
void rt_interrupt_context_pop(void);
/** 返回顶帧指针；仅当配置的列表非空时调用。 */
void *rt_interrupt_context_get(void);

/** 返回当前CPU的记账对象。 */
struct rt_cpu *rt_cpu_self(void);
/**
 * 返回 CPU 对象 @p index。 调用者必须传递`0 <= index < RT_CPUS_NR`； SMP 实现不执行任何边界 check（对于非零索引，UP 形式返回 NULL）。
 */
struct rt_cpu *rt_cpu_index(int index);

#ifdef RT_USING_SMP

/** 进入旧版全 CPU 调度锁定并返回恢复状态。 */
rt_base_t rt_cpus_lock(void);
/** 保留全 CPU 锁定并恢复 rt_cpus_lock() 捕获的状态。 */
void rt_cpus_unlock(rt_base_t level);
/**
 * 完成后上下文切换SMP调度器簿记；在RT-Smart MMU下，也切换到调度器post钩子之前的@p thread的地址空间。
 */
void rt_cpus_lock_status_restore(struct rt_thread *thread);

#ifdef RT_USING_DEBUG
    rt_base_t rt_cpu_get_id(void);
#else /* !RT_USING_DEBUG */
    #define rt_cpu_get_id rt_hw_cpu_id
#endif /* RT_USING_DEBUG */

#else /* !RT_USING_SMP */
#define rt_cpu_get_id()  (0)

#endif /* RT_USING_SMP */

/** 返回当前CPU的中断嵌套深度。 */
rt_uint8_t rt_interrupt_get_nest(void);

#ifdef RT_USING_HOOK
/**
 * 安装中断边界钩子。
 *
 * enter 钩子在进入 RT-Thread 中断记账之后调用；leave 钩子在仍处于中断退出路径时调用。两者都运行于 ISR 上下文，必须执行时间有界、不得阻塞或分配内存，并且在任意中断嵌套层级都应安全。
 */
void rt_interrupt_enter_sethook(void (*hook)(void));
void rt_interrupt_leave_sethook(void (*hook)(void));
#endif /* RT_USING_HOOK */

#ifdef RT_USING_COMPONENTS_INIT
/** 通过 INIT_APP/FS 导出的初始化程序执行非板 INIT_PREV。 */
void rt_components_init(void);
/** 执行板级 INIT_BOARD/CORE/SUBSYS/PLATFORM 初始化程序。 */
void rt_components_board_init(void);
#endif /* RT_USING_COMPONENTS_INIT */
/** @} */

/**
 * @addtogroup group_kernel_service
 * @{
 */

/**
 * @name 控制台输出
 * 当控制台支持不存在时，输出宏将被编译并且不会评估参数。 控制台输出是诊断性的，可以在启用 RT_USING_THREADSAFE_PRINTF 时进行序列化，并且不得假定对每个 ISR 或故障上下文都是安全的，除非所选的控制台驱动程序明确是安全的。
 */
#ifndef RT_USING_CONSOLE
/** 编译出格式化的内核控制台输出。 */
#define rt_kprintf(...)
/** 编译出未格式化的内核控制台输出。 */
#define rt_kputs(str)
#else
/** 格式化消息并将其写入当前内核控制台设备。 */
int rt_kprintf(const char *fmt, ...);
/** 将 NUL 结尾的字符串写入内核控制台，无需格式化。 */
void rt_kputs(const char *str);
#ifdef RT_USING_CONSOLE_OUTPUT_CTL
/** 全局启用或抑制正常控制台输出。 */
void rt_console_output_set_enabled(rt_bool_t enabled);
/** 返回当前是否启用正常控制台输出。 */
rt_bool_t rt_console_output_get_enabled(void);
#else
#define rt_console_output_set_enabled(enabled) ((void)0)
#define rt_console_output_get_enabled()        (RT_TRUE)
#endif /* RT_USING_CONSOLE_OUTPUT_CTL */
#endif /* RT_USING_CONSOLE */
/** @} */

/**
 * @name 栈回溯服务
 * 架构支持决定哪些框架可以展开。 缓冲区 API 存储原始返回/程序计数器地址；符号化/格式化可能是一个单独的步骤。 回溯正在运行的远程线程需要调度器和体系结构同步，而不仅仅是简单借用的 TCB 指针。
 */
/** 打印或以其他方式报告当前上下文的回溯。 */
rt_err_t rt_backtrace(void);
/** 报告从 @p thread 保存的上下文开始的回溯。 */
rt_err_t rt_backtrace_thread(rt_thread_t thread);
/** 从显式架构框架继续线程回溯。 */
rt_err_t rt_backtrace_frame(rt_thread_t thread, struct rt_hw_backtrace_frame *frame);
/** 打印已存储在 @p buffer 中的 @p buflen 原始地址。 */
rt_err_t rt_backtrace_formatted_print(rt_ubase_t *buffer, long buflen);
/** 跳过 @p skip 帧后展开到 @p buffer；返回每个移植层的状态。 */
rt_err_t rt_backtrace_to_buffer(rt_thread_t thread, struct rt_hw_backtrace_frame *frame,
                                long skip, rt_ubase_t *buffer, long buflen);
/** @} */

#if defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE)
/** 按名称选择已注册的设备作为控制台并返回之前的控制台。 */
rt_device_t rt_console_set_device(const char *name);
/** 返回当前选择的控制台设备。 */
rt_device_t rt_console_get_device(void);
#ifdef RT_USING_THREADSAFE_PRINTF
    /** 返回当前保存序列化控制台输出的线程（如果有）。 */
    rt_thread_t rt_console_current_user(void);
#else
    rt_inline void *rt_console_current_user(void) { return RT_NULL; }
#endif /* RT_USING_THREADSAFE_PRINTF */
#endif /* defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE) */

/** 返回最高有效设置位的从一开始的索引，或返回零为零。 */
int __rt_fls(int val);
/** 返回最低有效设置位的从一开始的索引，或返回零为零。 */
int __rt_ffs(int value);
/** 无符号长最低有效设置位帮助器。 */
unsigned long __rt_ffsl(unsigned long value);
/** 根据实施合同计算无符号长整型中的前导零位。 */
unsigned long __rt_clz(unsigned long value);

/** 将 RT-Thread 版本横幅打印到配置的控制台。 */
void rt_show_version(void);

#ifdef RT_DEBUGING_ASSERT
/** 由 rt_assert_handler() 使用表达式和源位置调用的可选观察者。 */
extern void (*rt_assert_hook)(const char *ex, const char *func, rt_size_t line);
/** 安装或清除断言观察器。 */
void rt_assert_set_hook(void (*hook)(const char *ex, const char *func, rt_size_t line));
/**
 * RT_ASSERT() 使用的断言策略。 默认打印回溯和 halts （或退出断言模块）；安装的断言钩子会替换默认路径，并可以选择是否返回控制权。
 */
void rt_assert_handler(const char *ex, const char *func, rt_size_t line);

/**
 * 评估 @p EX 一次并将故障文本/位置路由到 rt_assert_handler()。禁用的形式还通过 RT_UNUSED 计算 EX 一次，但丢弃其值。 将状态更改表达式保留在断言之外，并将宏用作花括号/独立语句，因为启用的扩展是一个裸 `if`。
 */
#define RT_ASSERT(EX)                                                         \
if (!(EX))                                                                    \
{                                                                             \
    rt_assert_handler(#EX, __FUNCTION__, __LINE__);                           \
}
#else
#define RT_ASSERT(EX) {RT_UNUSED(EX);}
#endif /* RT_DEBUGING_ASSERT */

#ifdef RT_DEBUGING_CONTEXT
/**
 * 断言调用者不在中断上下文中执行。这是仅调试的合同检查，否则会编译掉。
 */
#define RT_DEBUG_NOT_IN_INTERRUPT                                             \
do                                                                            \
{                                                                             \
    if (rt_interrupt_get_nest() != 0)                                         \
    {                                                                         \
        rt_kprintf("Function[%s] shall not be used in ISR\n", __FUNCTION__);  \
        RT_ASSERT(0)                                                          \
    }                                                                         \
}                                                                             \
while (0)

/*
 * 1) 调度器已经启动
 * 2) 不在中断上下文中。
 */
#define RT_DEBUG_IN_THREAD_CONTEXT                                            \
do                                                                            \
{                                                                             \
    if (rt_thread_self() == RT_NULL)                                          \
    {                                                                         \
        rt_kprintf("Function[%s] shall not be used before scheduler start\n", \
                   __FUNCTION__);                                             \
        RT_ASSERT(0)                                                          \
    }                                                                         \
    RT_DEBUG_NOT_IN_INTERRUPT;                                                \
}                                                                             \
while (0)

#if defined(RT_USING_SMP)
/**
 * @brief 检查禁用的中断是否导致调度器不可用。
 *
 * 在 SMP 版本中，某些内核内部无锁等待路径可能会禁用本地中断，同时仍然合法地使用与调度器相关的操作。仅为 UP 版本保留此 IRQ 禁用上下文断言。
 */
#define RT_DEBUG_SCHEDULER_IRQ_DISABLED() (RT_FALSE)
#else
/**
 * @brief 检查禁用的中断是否导致调度器不可用。
 *
 * 在 UP 版本中，全局禁用的中断会阻止正常调度和超时进程，因此阻塞调度器路径必须拒绝此上下文。
 */
#define RT_DEBUG_SCHEDULER_IRQ_DISABLED() rt_hw_interrupt_is_disabled()
#endif /* defined(RT_USING_SMP) */

/*
 * 1) 调度器已启动。
 * 2) 不在中断上下文中。
 * 3) 调度器未锁定。
 * 4) UP 上未禁用中断。
 */
#define RT_DEBUG_SCHEDULER_AVAILABLE(need_check)                              \
do                                                                            \
{                                                                             \
    if (need_check)                                                           \
    {                                                                         \
        if ((rt_critical_level() != 0) || RT_DEBUG_SCHEDULER_IRQ_DISABLED())  \
        {                                                                     \
            rt_kprintf("Function[%s]: scheduler is not available\n",          \
                    __FUNCTION__);                                            \
            RT_ASSERT(0)                                                      \
        }                                                                     \
        RT_DEBUG_IN_THREAD_CONTEXT;                                           \
    }                                                                         \
}                                                                             \
while (0)
#else
#define RT_DEBUG_NOT_IN_INTERRUPT
#define RT_DEBUG_IN_THREAD_CONTEXT
#define RT_DEBUG_SCHEDULER_AVAILABLE(need_check)
#endif /* RT_DEBUGING_CONTEXT */

/** 当在中断上下文之外执行时，调度器启动后返回 true。 */
rt_inline rt_bool_t rt_in_thread_context(void)
{
    return rt_thread_self() != RT_NULL && rt_interrupt_get_nest() == 0;
}

/**
 * 测试调度器的公共可用性谓词：关键嵌套必须为零，并且执行必须在线程上下文中。 该助手不执行单独的原始中断屏蔽测试。
 */
rt_inline rt_bool_t rt_scheduler_is_available(void)
{
    return rt_critical_level() == 0 && rt_in_thread_context();
}

#ifdef RT_USING_SMP
/**
 * 当 @p thread 绑定到 CPU 时返回 true。传递NULL查询当前线程。 在调度器启动之前，兼容性逻辑将丢失的当前线程视为可接受/正确。
 */
rt_inline rt_bool_t rt_sched_thread_is_binding(rt_thread_t thread)
{
    if (thread == RT_NULL)
    {
        thread = rt_thread_self();
    }
    return !thread || RT_SCHED_CTX(thread).bind_cpu != RT_CPUS_NR;
}

#else
#define rt_sched_thread_is_binding(thread) (RT_TRUE)
#endif

/**@}*/

#ifdef __cplusplus
}
#endif

#endif /* __RT_THREAD_H__ */
