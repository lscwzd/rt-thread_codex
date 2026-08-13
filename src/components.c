/*
 * Copyright (c) 2006-2022, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2012-09-20     Bernard      Change the name to components.c
 *                             And all components related header files.
 * 2012-12-23     Bernard      fix the pthread initialization issue.
 * 2013-06-23     Bernard      Add the init_call for components initialization.
 * 2013-07-05     Bernard      Remove initialization feature for MS VC++ compiler
 * 2015-02-06     Bernard      Remove the MS VC++ support and move to the kernel
 * 2015-05-04     Bernard      Rename it to components.c because compiling issue
 *                             in some IDEs.
 * 2015-07-29     Arda.Fu      Add support to use RT_USING_USER_MAIN with IAR
 * 2018-11-22     Jesven       Add secondary cpu boot up
 * 2023-09-15     xqyjlj       perf rt_hw_interrupt_disable/enable
*/

/**
 * @file components.c
 * @brief 串联系统自动初始化、用户 main 线程和调度器启动的内核启动入口。
 *
 * 启动链可以概括为：编译器入口 -> rtthread_startup() -> 板级初始化 -> 内核各
 * 子系统初始化 -> 创建 main/定时器/空闲/回收线程 -> 启动调度器。调度器启动后，
 * main_thread_entry() 在普通线程上下文中完成组件自动初始化，SMP 配置再唤醒其他
 * CPU，最后调用用户的 main()。
 *
 * 自动初始化不是运行时扫描名称，而是 INIT_xxx_EXPORT() 把函数指针放入具有排序
 * 关键字的链接段。链接器生成的起止符号界定每一级，下面的循环按地址顺序逐个调用。
 */

#include <rthw.h>
#include <rtthread.h>

#ifdef RT_USING_USER_MAIN
#ifndef RT_MAIN_THREAD_STACK_SIZE
#define RT_MAIN_THREAD_STACK_SIZE     2048
#endif /* RT_MAIN_THREAD_STACK_SIZE */
#ifndef RT_MAIN_THREAD_PRIORITY
#define RT_MAIN_THREAD_PRIORITY       (RT_THREAD_PRIORITY_MAX / 3)
#endif /* RT_MAIN_THREAD_PRIORITY */
#if (RT_MAIN_THREAD_PRIORITY >= RT_THREAD_PRIORITY_MAX)
#error "RT_MAIN_THREAD_PRIORITY must be < RT_THREAD_PRIORITY_MAX"
#elif (RT_MAIN_THREAD_PRIORITY < 0)
#error "RT_MAIN_THREAD_PRIORITY must be non-negative"
#endif /* RT_MAIN_THREAD_PRIORITY 范围检查 */
#endif /* RT_USING_USER_MAIN */

#ifdef RT_USING_COMPONENTS_INIT
/*
 * 组件自动初始化按下列级别顺序执行：
 * rti_start         --> 0
 * BOARD_EXPORT      --> 1
 * rti_board_end     --> 1.end
 *
 * DEVICE_EXPORT     --> 2
 * COMPONENT_EXPORT  --> 3
 * FS_EXPORT         --> 4
 * ENV_EXPORT        --> 5
 * APP_EXPORT        --> 6
 *
 * rti_end           --> 6.end
 *
 * 驱动或组件通过下列宏把自己的初始化函数放入对应链接段：
 * INIT_BOARD_EXPORT(fn);
 * INIT_DEVICE_EXPORT(fn);
 * ...
 * INIT_APP_EXPORT(fn);
 * 等。数字和字符串是链接排序键，不是运行时优先级。板级阶段只遍历
 * rti_board_start 与 rti_board_end 之间的项目；其余阶段稍后在 main 线程中遍历。
 */
static int rti_start(void)
{
    return 0;
}
INIT_EXPORT(rti_start, "0");

static int rti_board_start(void)
{
    return 0;
}
INIT_EXPORT(rti_board_start, "0.end");

static int rti_board_end(void)
{
    return 0;
}
INIT_EXPORT(rti_board_end, "1.end");

static int rti_end(void)
{
    return 0;
}
INIT_EXPORT(rti_end, "6.end");

/**
 * @brief 执行板级自动初始化函数。
 *
 * 该函数遍历 BOARD_EXPORT 所在区间，通常用于仍需在调度器启动前完成的片上外设、
 * 驱动早期资源等初始化。RT_DEBUGING_AUTO_INIT 配置会保留函数名描述符并打印每个
 * 函数及其返回值；普通配置只保存函数指针以减小镜像。初始化函数的返回值仅用于
 * 调试输出，本循环不会因某项失败而中止后续项目。
 */
void rt_components_board_init(void)
{
#ifdef RT_DEBUGING_AUTO_INIT
    int result;
    const struct rt_init_desc *desc;
    for (desc = &__rt_init_desc_rti_board_start; desc < &__rt_init_desc_rti_board_end; desc ++)
    {
        rt_kprintf("initialize %s\n", desc->fn_name);
        result = desc->fn();
        rt_kprintf(":%d done\n", result);
    }
#else
    volatile const init_fn_t *fn_ptr;

    for (fn_ptr = &__rt_init_rti_board_start; fn_ptr < &__rt_init_rti_board_end; fn_ptr++)
    {
        (*fn_ptr)();
    }
#endif /* RT_DEBUGING_AUTO_INIT */
}

/**
 * @brief 执行板级之后的所有组件自动初始化函数。
 *
 * 调用时已经处于 main 线程上下文且调度器可用，因此设备、组件、文件系统、环境和
 * 应用级初始化可以使用需要线程环境的内核服务。遍历范围从 rti_board_end 之后开始，
 * 到 rti_end 之前结束，顺序由 INIT_DEVICE_EXPORT 至 INIT_APP_EXPORT 的等级决定。
 */
void rt_components_init(void)
{
#ifdef RT_DEBUGING_AUTO_INIT
    int result;
    const struct rt_init_desc *desc;

    rt_kprintf("do components initialization.\n");
    for (desc = &__rt_init_desc_rti_board_end; desc < &__rt_init_desc_rti_end; desc ++)
    {
        rt_kprintf("initialize %s\n", desc->fn_name);
        result = desc->fn();
        rt_kprintf(":%d done\n", result);
    }
#else
    volatile const init_fn_t *fn_ptr;

    for (fn_ptr = &__rt_init_rti_board_end; fn_ptr < &__rt_init_rti_end; fn_ptr ++)
    {
        (*fn_ptr)();
    }
#endif /* RT_DEBUGING_AUTO_INIT */
}
#endif /* RT_USING_COMPONENTS_INIT */

#ifdef RT_USING_USER_MAIN

void rt_application_init(void);
void rt_hw_board_init(void);
int rtthread_startup(void);

#ifdef __ARMCC_VERSION
extern int $Super$$main(void);
/* ARMCC 的子/超符号机制：用包装入口先启动 RT-Thread，再由 main 线程调用原 main。 */
int $Sub$$main(void)
{
    rtthread_startup();
    return 0;
}
#elif defined(__ICCARM__)
/* IAR 启动代码会自动调用 __low_level_init。 */
extern void __iar_data_init3(void);
int __low_level_init(void)
{
    /* 先完成 IAR 数据段复制，再进入 RT-Thread 启动链。 */
    __iar_data_init3();
    rtthread_startup();
    return 0;
}
#elif defined(__GNUC__)
/* GCC 链接时通过 -eentry 指定此函数为镜像入口。 */
int entry(void)
{
    rtthread_startup();
    return 0;
}
#endif

#ifndef RT_USING_HEAP
/* 未启用堆时，main 线程控制块和栈必须由内核静态提供。 */
rt_align(RT_ALIGN_SIZE)
static rt_uint8_t main_thread_stack[RT_MAIN_THREAD_STACK_SIZE];
struct rt_thread main_thread;
#endif /* RT_USING_HEAP */

/**
 * @brief 系统 main 线程入口。
 *
 * 该线程是启动调度器后第一个承载用户初始化逻辑的普通线程：先调用
 * rt_components_init()，SMP 下再启动从核，最后调用工具链对应的用户 main()。
 * 把后期组件初始化放在线程中，意味着初始化代码可以被调度，也可以使用部分 IPC；
 * 但此时应用自身其他线程是否存在，取决于各初始化函数的创建顺序。
 *
 * @param parameter 线程参数；本入口不使用它。
 */
static void main_thread_entry(void *parameter)
{
    extern int main(void);
    RT_UNUSED(parameter);

#ifdef RT_USING_COMPONENTS_INIT
    /* 完成设备、组件、文件系统、环境和应用级自动初始化。 */
    rt_components_init();
#endif /* RT_USING_COMPONENTS_INIT */

#ifdef RT_USING_SMP
    rt_hw_secondary_cpu_up();
#endif /* RT_USING_SMP */
    /* 转入用户程序入口；不同工具链采用各自的 main 包装机制。 */
#ifdef __ARMCC_VERSION
    {
        extern int $Super$$main(void);
        $Super$$main(); /* ARMCC 的 $Super$$main 表示被包装的原始 main。 */
    }
#elif defined(__ICCARM__) || defined(__GNUC__) || defined(__TASKING__) || defined(__TI_COMPILER_VERSION__)
    main();
#endif /* __ARMCC_VERSION */
}

/**
 * @brief 创建并置为就绪态 main 线程。
 *
 * 启用堆时动态创建线程；未启用堆时使用本文件的静态线程控制块和静态栈。这里的
 * rt_thread_startup() 只是把线程加入就绪队列，因为首次调用时调度器尚未启动，
 * main_thread_entry() 要等 rt_system_scheduler_start() 选中它之后才真正执行。
 */
void rt_application_init(void)
{
    rt_thread_t tid;

#ifdef RT_USING_HEAP
    tid = rt_thread_create("main", main_thread_entry, RT_NULL,
                           RT_MAIN_THREAD_STACK_SIZE, RT_MAIN_THREAD_PRIORITY, 20);
    RT_ASSERT(tid != RT_NULL);
#else
    rt_err_t result;

    tid = &main_thread;
    result = rt_thread_init(tid, "main", main_thread_entry, RT_NULL,
                            main_thread_stack, sizeof(main_thread_stack), RT_MAIN_THREAD_PRIORITY, 20);
    RT_ASSERT(result == RT_EOK);

    /* 某些关闭断言的构建不会读取 result，显式丢弃可消除编译器警告。 */
    (void)result;
#endif /* RT_USING_HEAP */

    rt_thread_startup(tid);
}

/**
 * @brief 完成 RT-Thread 内核启动并启动第一次线程调度。
 *
 * 调用顺序经过精心安排：板级代码先建立时钟和堆；随后初始化定时器、调度器和
 * 信号子系统；再创建 main、定时器服务、空闲和僵尸回收线程。所有可运行线程都
 * 就绪后才启动调度器。SMP 的全局 CPU 锁在切换前保持锁定，由上下文切换路径恢复
 * 正确状态，从而避免多个 CPU 在启动边界同时操作调度数据。
 *
 * @return 正常情况下永不返回；若意外返回 0，表示调度器启动失败。
 */
int rtthread_startup(void)
{
#ifdef RT_USING_SMP
    rt_hw_spin_lock_init(&_cpus_lock);
#endif
    rt_hw_local_irq_disable();

    /* 板级初始化应建立硬件时钟、中断控制器，并在这里准备好内核堆。
     * 注意：后面的动态线程/对象创建可能立即依赖堆。
     */
    rt_hw_board_init();

    /* 输出版本横幅，便于确认实际运行的内核配置。 */
    rt_show_version();

    /* 初始化硬/软定时器容器。 */
    rt_system_timer_init();

    /* 初始化就绪队列、优先级位图和每 CPU 调度状态。 */
    rt_system_scheduler_init();

#ifdef RT_USING_SIGNALS
    /* 初始化线程信号子系统的全局资源。 */
    rt_system_signal_init();
#endif /* RT_USING_SIGNALS */

    /* 创建 main 线程；它承担后期组件初始化和用户 main。 */
    rt_application_init();

    /* 若启用软定时器，创建执行回调的定时器服务线程。 */
    rt_system_timer_thread_init();

    /* 每个 CPU 创建一个最低优先级的兜底空闲线程。 */
    rt_thread_idle_init();

    /* 建立退出线程的延迟回收设施。 */
    rt_thread_defunct_init();

#ifdef RT_USING_SMP
    rt_hw_spin_lock(&_cpus_lock);
#endif /* RT_USING_SMP */

    /* 选择最高优先级就绪线程并完成第一次上下文切换。 */
    rt_system_scheduler_start();

    /* 正常调度永不返回到启动栈。 */
    return 0;
}
#endif /* RT_USING_USER_MAIN */
