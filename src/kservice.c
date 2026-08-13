/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2006-03-16     Bernard      首个版本
 * 2006-05-25     Bernard      重写 vsprintf
 * 2006-08-10     Bernard      增加 rt_show_version
 * 2010-03-17     Bernard      删除 rt_strlcpy，并修复 GCC 编译问题
 * 2010-04-15     Bernard      删除 ICCM16C 编译器下的弱定义
 * 2012-07-18     Arda         增加有符号整数的对齐显示
 * 2012-11-23     Bernard      修复 IAR 编译错误
 * 2012-12-22     Bernard      修复 Grissiom 发现的 rt_kprintf 问题
 * 2013-06-24     Bernard      未启用 RT_USING_CONSOLE 时移除 rt_kprintf
 * 2013-09-24     aozima       rt_kprintf 使用设备时确保设备处于 STREAM 模式
 * 2015-07-06     Bernard      增加 rt_assert_handler
 * 2021-02-28     Meco Man     增加 RT_KSERVICE_USING_STDLIB
 * 2021-12-20     Meco Man     实现 rt_strcpy()
 * 2022-01-07     Gabriel      增加 __on_rt_assert_hook
 * 2022-06-04     Meco Man     删除 strnlen
 * 2022-08-24     Yunjie       使 rt_memset 不依赖字宽，以适配 16 位字长的 TI C28x
 * 2022-08-30     Yunjie       使 rt_vsnprintf 适配 16 位 int 的 TI C28x
 * 2023-02-02     Bernard      在版本标识中增加 Smart ID
 * 2023-10-16     Shell        为 rt_malloc 服务增加钩子点
 * 2023-10-21     Shell        支持与体系结构无关的通用回溯 API
 * 2023-12-10     xqyjlj       优化中断开关并修复 memheap 锁
 * 2024-03-10     Meco Man     将标准 libc 相关函数移至 rtklibc
 * 2026-03-16     Rbb666       将 rt_thread_get_usage 改为增量统计
 */

/**
 * @file kservice.c
 * @brief 内核通用服务：控制台、栈回溯、CPU 利用率、系统堆、位操作和断言。
 *
 * 这个文件不是一个单独算法，而是内核多个基础子系统之间的“公共服务层”：
 *
 * - 为 BSP 可覆盖的延时、复位、关机、控制台输出和回溯原语提供弱默认实现；
 * - 把 rt_kprintf() 格式化结果安全地送到控制台设备或早期硬件输出；
 * - 在架构提供逐帧展开能力时实现通用调用栈遍历；
 * - 按采样窗口计算线程 CPU 使用率；
 * - 在 small-memory、memheap、slab 三种后端之上提供统一系统堆 API；
 * - 提供 FFS/FLS 位扫描和 RT_ASSERT 失败处理。
 *
 * 这些服务的上下文要求并不相同。控制台和断言可能在异常路径执行；普通系统
 * 堆通常只能在线程上下文使用，只有 RT_USING_HEAP_ISR 后端用自旋锁支持中断；
 * 回溯能否读取另一个线程取决于 CPU 端口。阅读每个函数时应同时关注条件编译
 * 分支和锁的种类，不能把一个配置的行为推广到全部系统。
 */

#include <rtthread.h>

/* 请求 rthw.h 同时引入 CPU 端口定义的回溯辅助宏。 */
#define RT_HW_INCLUDE_CPUPORT
#include <rthw.h>

#define DBG_TAG           "kernel.service"
#ifdef RT_DEBUG_DEVICE
#define DBG_LVL           DBG_LOG
#else
#define DBG_LVL           DBG_WARNING
#endif /* defined (RT_DEBUG_DEVICE) */
#include <rtdbg.h>

#ifdef RT_USING_MODULE
#include <dlmodule.h>
#endif /* RT_USING_MODULE */

#ifdef RT_USING_SMART
#include <lwp.h>
#include <lwp_user_mm.h>
#endif

/**
 * @addtogroup group_kernel_service
 * @{
 */

#if defined(RT_USING_DEVICE) && defined(RT_USING_CONSOLE)
static rt_device_t _console_device = RT_NULL;
#endif

/**
 * @brief BSP 未实现微秒延时时使用的弱后备函数。
 *
 * 此实现只打印警告并立即返回，完全不会等待。任何依赖硬件时序的驱动都必须
 * 由 BSP 提供强符号覆盖，不能把这个函数的存在误认为平台已经支持精确延时。
 */
rt_weak void rt_hw_us_delay(rt_uint32_t us)
{
    (void) us;
    LOG_W("rt_hw_us_delay() doesn't support for this board."
        "Please consider implementing rt_hw_us_delay() in another file.");
}

/**
 * @brief BSP 未实现复位时的弱后备函数。
 *
 * 仅打印警告并返回，CPU 不会复位。真正实现通常要操作看门狗、复位控制器或
 * 体系结构系统寄存器，并处理缓存/外设状态。
 */
rt_weak void rt_hw_cpu_reset(void)
{
    LOG_W("rt_hw_cpu_reset() doesn't support for this board."
        "Please consider implementing rt_hw_cpu_reset() in another file.");
    return;
}

/**
 * @brief BSP 未实现关机时的弱后备函数。
 *
 * 函数关闭本地中断并触发必失败断言，目的是阻止系统在“关机失败”后继续运行；
 * 它不会真正切断电源。产品 BSP 应提供平台电源管理实现。
 */
rt_weak void rt_hw_cpu_shutdown(void)
{
    LOG_I("CPU shutdown...");
    LOG_W("Using default rt_hw_cpu_shutdown()."
        "Please consider implementing rt_hw_cpu_shutdown() in another file.");
    rt_hw_interrupt_disable();
    RT_ASSERT(0);
    return;
}

/**
 * CPU 端口可以在 cpuport.h 中覆盖此宏，以便用体系结构专用方式取得当前帧。
 * 通用 GCC 分支记录当前帧指针和一个本函数内标签地址；其他编译器的后备实现
 * 把二者置零，调用方随后会把它视为“不支持回溯”。
 */
#ifndef RT_HW_BACKTRACE_FRAME_GET_SELF

#ifdef __GNUC__
    #define RT_HW_BACKTRACE_FRAME_GET_SELF(frame) do {          \
        (frame)->fp = (rt_uintptr_t)__builtin_frame_address(0U);   \
        (frame)->pc = ({__label__ pc; pc: (rt_uintptr_t)&&pc;});   \
    } while (0)

#else
    #define RT_HW_BACKTRACE_FRAME_GET_SELF(frame) do {  \
        (frame)->fp = 0;                                \
        (frame)->pc = 0;                                \
    } while (0)

#endif /* __GNUC__ */

#endif /* RT_HW_BACKTRACE_FRAME_GET_SELF */

/**
 * @brief 取得目标线程最内层（当前保存点）的回溯帧。
 *
 * @param thread 目标线程。
 * @param frame 输出帧，由端口填写帧指针和程序计数器。
 * @return 成功返回 RT_EOK；通用弱实现返回 -RT_ENOSYS。
 *
 * 这是 CPU 端口扩展点。弱实现不读取线程，也不修改输出；要支持对任意线程
 * 回溯，端口必须知道该架构保存上下文和栈帧的具体布局。
 */
rt_weak rt_err_t rt_hw_backtrace_frame_get(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    RT_UNUSED(thread);
    RT_UNUSED(frame);

    LOG_W("%s is not implemented", __func__);
    return -RT_ENOSYS;
}

/**
 * @brief 把 @p frame 原地推进到调用者的上一层栈帧。
 *
 * @param thread 栈所属线程，端口可用它验证栈范围。
 * @param frame 输入当前帧，成功时改写为上一层帧。
 * @return 成功返回 RT_EOK；到达栈顶、帧非法或弱实现不支持时返回错误。
 *
 * 该接口同样是架构扩展点。通用遍历器把任何非零返回视为终止条件。
 */
rt_weak rt_err_t rt_hw_backtrace_frame_unwind(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    RT_UNUSED(thread);
    RT_UNUSED(frame);

    LOG_W("%s is not implemented", __func__);
    return -RT_ENOSYS;
}

/** @brief 返回 CPU 架构名称；弱后备返回静态字符串 `"unknown"`。 */
rt_weak const char *rt_hw_cpu_arch(void)
{
    return "unknown";
}

/**
 * @brief 打印 RT-Thread 标识、版本号以及本次构建日期和时间。
 *
 * 输出名称会根据 RT_USING_SMART/RT_USING_NANO 改变。该信息通过
 * rt_kprintf() 发往当前控制台，因此仍受控制台启用状态和输出缓冲长度影响。
 */
void rt_show_version(void)
{
    rt_kprintf("\n \\ | /\n");
#if defined(RT_USING_SMART)
    rt_kprintf("- RT -     Thread Smart Operating System\n");
#elif defined(RT_USING_NANO)
    rt_kprintf("- RT -     Thread Nano Operating System\n");
#else
    rt_kprintf("- RT -     Thread Operating System\n");
#endif
    rt_kprintf(" / | \\     %d.%d.%d build %s %s\n",
               (rt_int32_t)RT_VERSION_MAJOR, (rt_int32_t)RT_VERSION_MINOR, (rt_int32_t)RT_VERSION_PATCH, __DATE__, __TIME__);
    rt_kprintf(" 2006 - 2024 Copyright by RT-Thread team\n");
}
RTM_EXPORT(rt_show_version);

#ifdef RT_USING_CONSOLE
#ifdef RT_USING_DEVICE
/**
 * @brief 返回当前作为系统控制台的设备。
 *
 * @return 已设置的设备指针；尚未绑定设备时返回 RT_NULL，此时输出走 BSP 的
 *         rt_hw_console_output()。
 *
 * 这里只读取全局指针，不增加设备引用计数；调用者不能据此销毁设备。
 */
rt_device_t rt_console_get_device(void)
{
    return _console_device;
}
RTM_EXPORT(rt_console_get_device);

/**
 * @brief 按名称切换 rt_kprintf() 使用的控制台设备。
 *
 * @param name 已注册设备名称。
 *
 * @return 切换前的控制台设备；原先没有设备时为 RT_NULL。名称未找到时不会
 *         改变当前设备，但返回值仍是旧设备，调用者需自行判断目标是否存在。
 *
 * 若目标不同，函数先关闭旧设备，再以读写和流模式打开新设备。当前实现没有
 * 检查 rt_device_open() 返回值，也没有围绕全局指针加锁，所以通常应在启动
 * 初始化阶段或已由上层串行化的管理路径调用。
 */
rt_device_t rt_console_set_device(const char *name)
{
    rt_device_t old_device = _console_device;
    rt_device_t new_device = rt_device_find(name);

    if (new_device != RT_NULL && new_device != old_device)
    {
        if (old_device != RT_NULL)
        {
            /* 释放旧控制台持有的一次打开引用。 */
            rt_device_close(old_device);
        }

        /* 流模式避免驱动对换行等文本数据做块设备式解释。 */
        rt_device_open(new_device, RT_DEVICE_OFLAG_RDWR | RT_DEVICE_FLAG_STREAM);
        _console_device = new_device;
    }

    return old_device;
}
RTM_EXPORT(rt_console_set_device);
#endif /* RT_USING_DEVICE */

#ifdef RT_USING_CONSOLE_OUTPUT_CTL
static volatile rt_bool_t _console_output_enabled = RT_TRUE;

/**
 * @brief 全局启用或禁用控制台日志输出。
 *
 * @param enabled RT_TRUE 允许输出，RT_FALSE 让 rt_kputs/rt_kprintf 直接返回。
 */
void rt_console_output_set_enabled(rt_bool_t enabled)
{
    _console_output_enabled = enabled;
}
RTM_EXPORT(rt_console_output_set_enabled);

/**
 * @brief 查询控制台输出总开关。
 *
 * @return 当前布尔状态。
 */
rt_bool_t rt_console_output_get_enabled(void)
{
    return _console_output_enabled;
}
RTM_EXPORT(rt_console_output_get_enabled);
#endif /* RT_USING_CONSOLE_OUTPUT_CTL */

rt_weak void rt_hw_console_output(const char *str)
{
    /* 弱后备实现静默丢弃文本；实际 BSP 应覆盖它。 */
    RT_UNUSED(str);
}
RTM_EXPORT(rt_hw_console_output);

#ifdef RT_USING_THREADSAFE_PRINTF

/* 串行化一次完整控制台写入，并记录可重入拥有者。 */
static struct rt_spinlock _syscon_lock = RT_SPINLOCK_INIT;
/* 单独保护 rt_kprintf() 的静态格式化缓冲区。 */
static struct rt_spinlock _prbuf_lock = RT_SPINLOCK_INIT;
/* 当前持有控制台逻辑所有权的线程；启动/ISR 时也可能为 RT_NULL。 */
static rt_thread_t _pr_curr_user;

#ifdef RT_USING_DEBUG
static rt_base_t _pr_critical_level;
#endif /* RT_USING_DEBUG */

/* 同一线程递归打印的层数，归零时才真正释放控制台。 */
static volatile int _pr_curr_user_nested;

rt_thread_t rt_console_current_user(void)
{
    return _pr_curr_user;
}

/**
 * @brief 取得线程安全控制台的可重入逻辑所有权。
 *
 * `_syscon_lock` 只保护拥有者字段，不能在实际设备输出的整个期间一直关闭
 * 中断。因此函数在无人占用时进入调度临界区、记录当前线程为拥有者，然后
 * 释放自旋锁；其他线程发现已有拥有者后会解锁并 yield，稍后重试。同一线程
 * 可递归进入，只增加嵌套计数。
 *
 * @note 该方案依赖 `rt_thread_self()` 标识拥有者，主要面向线程上下文。异常或
 *       中断中的递归打印行为取决于当时的当前线程和端口约束。
 */
static void _console_take(void)
{
    rt_ubase_t level = rt_spin_lock_irqsave(&_syscon_lock);
    rt_thread_t self_thread = rt_thread_self();
    rt_base_t critical_level;
    RT_UNUSED(critical_level);

    while (_pr_curr_user != self_thread)
    {
        if (_pr_curr_user == RT_NULL)
        {
            /* 持有控制台期间禁止线程抢占，防止拥有者被切走后其他线程忙等死锁。 */
            critical_level = rt_enter_critical();
#ifdef RT_USING_DEBUG
            _pr_critical_level = _syscon_lock.critical_level;
            _syscon_lock.critical_level = critical_level;
#endif
            _pr_curr_user = self_thread;
            break;
        }
        else
        {
            rt_spin_unlock_irqrestore(&_syscon_lock, level);
            rt_thread_yield();
            level = rt_spin_lock_irqsave(&_syscon_lock);
        }
    }

    _pr_curr_user_nested++;

    rt_spin_unlock_irqrestore(&_syscon_lock, level);
}

/**
 * @brief 释放一次控制台递归所有权。
 *
 * 只有嵌套计数减到 0 时才清除拥有者并退出取得所有权时进入的调度临界区。
 * 调试配置使用保存在锁对象中的层级进行配对校验。调用者必须与
 * _console_take() 严格成对，且由同一当前线程执行。
 */
static void _console_release(void)
{
    rt_ubase_t level = rt_spin_lock_irqsave(&_syscon_lock);
    rt_thread_t self_thread = rt_thread_self();
    RT_UNUSED(self_thread);

    RT_ASSERT(_pr_curr_user == self_thread);

    _pr_curr_user_nested--;
    if (!_pr_curr_user_nested)
    {
        _pr_curr_user = RT_NULL;

#ifdef RT_USING_DEBUG
        rt_exit_critical_safe(_syscon_lock.critical_level);
        _syscon_lock.critical_level = _pr_critical_level;
#else
        rt_exit_critical();
#endif
    }
    rt_spin_unlock_irqrestore(&_syscon_lock, level);
}

#define CONSOLE_TAKE          _console_take()
#define CONSOLE_RELEASE       _console_release()
#define PRINTF_BUFFER_TAKE    rt_ubase_t level = rt_spin_lock_irqsave(&_prbuf_lock)
#define PRINTF_BUFFER_RELEASE rt_spin_unlock_irqrestore(&_prbuf_lock, level)
#else

#define CONSOLE_TAKE
#define CONSOLE_RELEASE
#define PRINTF_BUFFER_TAKE
#define PRINTF_BUFFER_RELEASE
#endif /* RT_USING_THREADSAFE_PRINTF */

/**
 * @brief 向当前控制台后端写出明确长度的一段文本。
 *
 * @param str 可读字符序列的起点。
 * @param len 要写出的字节数；设备路径不依赖 NUL 终止。
 *
 * 函数先取得控制台逻辑所有权。绑定设备时调用 rt_device_write()，否则调用
 * BSP 弱接口 rt_hw_console_output()；后者只接收字符串指针，所以调用者应
 * 保证缓冲区在 @p len 之后仍有 NUL。返回值被忽略，诊断输出不会向上报告
 * 部分写入或设备错误。
 */
static void _kputs(const char *str, long len)
{
#ifdef RT_USING_DEVICE
    rt_device_t console_device = rt_console_get_device();
#endif /* RT_USING_DEVICE */

    CONSOLE_TAKE;

#ifdef RT_USING_DEVICE
    if (console_device == RT_NULL)
    {
        rt_hw_console_output(str);
    }
    else
    {
        rt_device_write(console_device, 0, str, len);
    }
#else
    RT_UNUSED(len);
    rt_hw_console_output(str);
#endif /* RT_USING_DEVICE */

    CONSOLE_RELEASE;
}

/**
 * @brief 把一个 NUL 结尾字符串写到系统控制台。
 *
 * @param str 输入字符串；RT_NULL 或全局输出禁用时直接返回。
 */
void rt_kputs(const char *str)
{
    if (!str)
    {
        return;
    }

    if (!rt_console_output_get_enabled())
    {
        return;
    }

    _kputs(str, rt_strlen(str));
}

/**
 * @brief 格式化并打印到系统控制台。
 *
 * @param fmt printf 风格格式串，后续可变参数类型必须与转换说明匹配。
 *
 * @return 实际送往控制台的字符数；输出关闭时返回 0。若理想结果超过
 *         RT_CONSOLEBUF_SIZE，则返回截断后的长度，而不是标准 snprintf 的
 *         完整所需长度。
 *
 * 所有调用共享静态 `rt_log_buf`。启用线程安全打印时 `_prbuf_lock` 防止不同
 * CPU 同时格式化覆盖缓冲区，控制台锁则防止一条日志被另一条日志穿插。由于
 * 这些锁可能关闭中断或禁止调度，格式化和底层输出都应保持有界。
 */
rt_weak int rt_kprintf(const char *fmt, ...)
{
    va_list args;
    rt_size_t length = 0;
    static char rt_log_buf[RT_CONSOLEBUF_SIZE];

    if (!rt_console_output_get_enabled())
    {
        return 0;
    }

    va_start(args, fmt);
    PRINTF_BUFFER_TAKE;

    /*
     * rt_vsnprintf 返回缓冲区无限大时本应生成的字符数（不含结尾 NUL）。
     * 实际静态缓冲区较小时，只能把输出长度钳制到最后一个有效字符，避免
     * _kputs() 读取截断缓冲区之外的内容。
     */
    length = rt_vsnprintf(rt_log_buf, sizeof(rt_log_buf) - 1, fmt, args);
    if (length > RT_CONSOLEBUF_SIZE - 1)
    {
        length = RT_CONSOLEBUF_SIZE - 1;
    }

    _kputs(rt_log_buf, length);

    PRINTF_BUFFER_RELEASE;
    va_end(args);

    return length;
}
RTM_EXPORT(rt_kprintf);
#endif /* RT_USING_CONSOLE */

/**
 * @brief 从当前函数附近开始打印当前线程的调用栈。
 *
 * @return 成功启动遍历返回 RT_EOK；无法取得有效帧指针时返回 -RT_EINVAL。
 *
 * 宏先取得当前帧，再主动展开一次以跳过 rt_backtrace() 自身，随后交给通用
 * rt_backtrace_frame()。真实可用性取决于编译器是否保留帧链以及 CPU 端口的
 * unwind 实现。
 */
rt_weak rt_err_t rt_backtrace(void)
{
    struct rt_hw_backtrace_frame frame = {0};
    rt_thread_t thread = rt_thread_self();

    /* 静态分析抑制：宏会按所选端口约定填写 frame。 */
    RT_HW_BACKTRACE_FRAME_GET_SELF(&frame);
    if (!frame.fp)
        return -RT_EINVAL;

    /* 跳过回溯函数自己的内部帧，使首个地址更接近真正调用者。 */
    rt_hw_backtrace_frame_unwind(thread, &frame);

    return rt_backtrace_frame(thread, &frame);
}

/**
 * @brief 从指定帧开始逐层展开，并把程序计数器打印到控制台。
 *
 * @param thread 栈所属线程。
 * @param frame 起始帧；每次 unwind 会原地修改它，调用后原值不再保留。
 * @return 遍历过程结束返回 RT_EOK；端口 unwind 的终止错误只用于停止循环，
 *         不会原样传播。
 *
 * 最多打印 RT_BACKTRACE_LEVEL_MAX_NR 层，以避免损坏的帧链导致无限循环。
 */
rt_weak rt_err_t rt_backtrace_frame(rt_thread_t thread, struct rt_hw_backtrace_frame *frame)
{
    long nesting = 0;

    rt_kprintf("please use: addr2line -e rtthread.elf -a -f\n");

    while (nesting < RT_BACKTRACE_LEVEL_MAX_NR)
    {
        rt_kprintf(" 0x%lx", (rt_ubase_t)frame->pc);
        if (rt_hw_backtrace_frame_unwind(thread, frame))
        {
            break;
        }
        nesting++;
    }
    rt_kprintf("\n");
    return RT_EOK;
}

/**
 * @brief 打印已经保存到数组中的一组程序计数器。
 *
 * @param buffer 地址数组。
 * @param buflen 数组元素数；遇到第一个 0 地址会提前停止。
 * @return 打印结束返回 RT_EOK。
 */
rt_weak rt_err_t rt_backtrace_formatted_print(rt_ubase_t *buffer, long buflen)
{
    rt_kprintf("please use: addr2line -e rtthread.elf -a -f\n");

    for (rt_size_t i = 0; i < buflen && buffer[i] != 0; i++)
    {
        rt_kprintf(" 0x%lx", (rt_ubase_t)buffer[i]);
    }

    rt_kprintf("\n");
    return RT_EOK;
}


/**
 * @brief 将调用栈程序计数器收集到调用者数组，而不是直接打印。
 *
 * @param thread 栈所属线程，不得为 RT_NULL。
 * @param frame 可选起始帧；RT_NULL 表示使用宏取得当前帧。
 * @param skip 除本函数自身必跳过的一帧外，还要额外丢弃的层数。
 * @param buffer 输出地址数组，至少能容纳 @p buflen 个元素。
 * @param buflen 最大保存元素数。若实际帧数更少，函数会在最后一个地址之后
 *               写入 0；若数组恰好装满，则没有额外终止元素。
 * @return 成功返回 RT_EOK；线程或当前帧无效时返回 -RT_EINVAL。
 *
 * @warning 当前实现没有单独检查 @p buffer 和负的 @p buflen，调用者必须提供
 *          合法参数。传入的 @p frame 会被原地推进。
 */
rt_weak rt_err_t rt_backtrace_to_buffer(rt_thread_t thread,
                                        struct rt_hw_backtrace_frame *frame,
                                        long skip,
                                        rt_ubase_t *buffer,
                                        long buflen)
{
    long nesting = 0;
    struct rt_hw_backtrace_frame cur_frame = {0};

    if (!thread)
        return -RT_EINVAL;

    RT_ASSERT(rt_object_get_type(&thread->parent) == RT_Object_Class_Thread);

    if (!frame)
    {
        frame = &cur_frame;
        /* 静态分析抑制：端口宏负责初始化本地帧。 */
        RT_HW_BACKTRACE_FRAME_GET_SELF(frame);
        if (!frame->fp)
            return -RT_EINVAL;
    }

    /* 必定先丢弃本函数内部帧，再按 skip 继续向外展开。 */
    do {
        rt_hw_backtrace_frame_unwind(thread, frame);
    } while (skip-- > 0);

    while (nesting < buflen)
    {
        *buffer++ = (rt_ubase_t)frame->pc;
        if (rt_hw_backtrace_frame_unwind(thread, frame))
        {
            break;
        }
        nesting++;
    }

    if (nesting < buflen)
        *buffer = RT_NULL;

    return RT_EOK;
}

/**
 * @brief 取得并打印指定线程的已保存调用栈。
 *
 * @param thread 目标线程。
 * @return 端口取帧或通用打印函数的状态；空指针返回 -RT_EINVAL。
 *
 * 目标线程若正在另一 CPU 运行，其栈可能同时变化；是否支持这种情况以及需要
 * 什么暂停/锁定措施由架构端口决定，本包装函数本身不停止目标线程。
 */
rt_err_t rt_backtrace_thread(rt_thread_t thread)
{
    rt_err_t rc;
    struct rt_hw_backtrace_frame frame;
    if (thread)
    {
        rc = rt_hw_backtrace_frame_get(thread, &frame);
        if (rc == RT_EOK)
        {
            rc = rt_backtrace_frame(thread, &frame);
        }
    }
    else
    {
        rc = -RT_EINVAL;
    }
    return rc;
}

#ifdef RT_USING_CPU_USAGE_TRACER

#define RT_CPU_USAGE_CALC_INTERVAL_TICK \
    ((RT_TICK_PER_SECOND * RT_CPU_USAGE_CALC_INTERVAL_MS + 999U) / 1000U)

static rt_tick_t _cpu_usage_sample_tick;
static rt_bool_t _cpu_usage_inited = RT_FALSE;
static struct rt_cpu_usage_stats _cpu_usage_prev_cpu_stat[RT_CPUS_NR];
static struct rt_spinlock _cpu_usage_lock = RT_SPINLOCK_INIT;

/**
 * @brief 计算本采样窗口全部 CPU 的总运行时间增量，并刷新 CPU 快照。
 *
 * user/system/idle 分别先以 rt_ubase_t 宽度做无符号减法，因此单次自然回绕
 * 可按模运算得到正确差值，再扩展到 64 位求和。通用 tick 统计当前不更新 irq
 * 字段，所以这里也不把 irq 纳入分母。
 */
static rt_uint64_t _cpu_usage_calc_total_delta(void)
{
    rt_uint64_t total_delta = 0;
    int i;

    for (i = 0; i < RT_CPUS_NR; i++)
    {
        rt_cpu_t pcpu = rt_cpu_index(i);
        rt_ubase_t user_now = pcpu->cpu_stat.user;
        rt_ubase_t system_now = pcpu->cpu_stat.system;
        rt_ubase_t idle_now = pcpu->cpu_stat.idle;

        /* 先分别求模差值，再转成 64 位累加，避免先求总和产生回绕伪差。 */
        rt_ubase_t user_delta = (rt_ubase_t)(user_now - _cpu_usage_prev_cpu_stat[i].user);
        rt_ubase_t system_delta = (rt_ubase_t)(system_now - _cpu_usage_prev_cpu_stat[i].system);
        rt_ubase_t idle_delta = (rt_ubase_t)(idle_now - _cpu_usage_prev_cpu_stat[i].idle);

        total_delta += (rt_uint64_t)user_delta;
        total_delta += (rt_uint64_t)system_delta;
        total_delta += (rt_uint64_t)idle_delta;

        _cpu_usage_prev_cpu_stat[i].user = user_now;
        _cpu_usage_prev_cpu_stat[i].system = system_now;
        _cpu_usage_prev_cpu_stat[i].idle = idle_now;
    }

    return total_delta;
}

/**
 * @brief 建立线程与 CPU 统计的初始快照。
 *
 * 遍历全局线程对象表，把每个线程的上次总时间和缓存百分比清零，再把每 CPU
 * 快照清零。遍历时持有对象表自旋锁；模块私有线程若不在全局表中不会被处理。
 */
static void _cpu_usage_snapshot_init(void)
{
    struct rt_object_information *info;
    rt_list_t *list;
    rt_list_t *node;
    rt_base_t level;
    int i;

    info = rt_object_get_information(RT_Object_Class_Thread);
    list = &info->object_list;

    level = rt_spin_lock_irqsave(&info->spinlock);
    for (node = list->next; node != list; node = node->next)
    {
        struct rt_object *obj = rt_list_entry(node, struct rt_object, list);
        struct rt_thread *t = (struct rt_thread *)obj;

        t->total_time_prev = 0U;
        t->cpu_usage = 0U;
    }
    rt_spin_unlock_irqrestore(&info->spinlock, level);

    for (i = 0; i < RT_CPUS_NR; i++)
    {
        _cpu_usage_prev_cpu_stat[i].user = 0U;
        _cpu_usage_prev_cpu_stat[i].system = 0U;
        _cpu_usage_prev_cpu_stat[i].idle = 0U;
    }

    _cpu_usage_sample_tick = rt_tick_get();
    _cpu_usage_inited = RT_TRUE;
}

/**
 * @brief 用本窗口总 CPU 时间更新所有全局线程的百分比。
 *
 * 每个线程的分子是 `(user_time + system_time) - total_time_prev`，分母是所有
 * CPU 的 user/system/idle 增量。结果取整数百分比并钳制到 100；总增量为 0
 * 时写 0。函数在对象表锁内读写线程统计字段。
 */
static void _cpu_usage_refresh_threads(rt_uint64_t total_delta)
{
    struct rt_object_information *info;
    rt_list_t *list;
    rt_list_t *node;
    rt_base_t level;

    info = rt_object_get_information(RT_Object_Class_Thread);
    list = &info->object_list;

    level = rt_spin_lock_irqsave(&info->spinlock);
    for (node = list->next; node != list; node = node->next)
    {
        struct rt_object *obj = rt_list_entry(node, struct rt_object, list);
        struct rt_thread *t = (struct rt_thread *)obj;
        rt_ubase_t total_now = (rt_ubase_t)(t->user_time + t->system_time);
        rt_ubase_t total_delta_now = (rt_ubase_t)(total_now - t->total_time_prev);
        rt_uint64_t thread_delta = (rt_uint64_t)total_delta_now;

        if (total_delta > 0U)
        {
            rt_uint64_t usage = (thread_delta * 100U) / total_delta;
            t->cpu_usage = (rt_uint8_t)(usage > 100U ? 100U : usage);
        }
        else
        {
            t->cpu_usage = 0U;
        }

        t->total_time_prev = total_now;
    }
    rt_spin_unlock_irqrestore(&info->spinlock, level);
}

/**
 * @brief 到达配置采样间隔时生成一次新利用率快照。
 *
 * 首次调用只建立基线，但随后会绕过间隔检查完成一次计算。之后若距离上次
 * 样本不足 RT_CPU_USAGE_CALC_INTERVAL_TICK，则保留旧缓存，避免每次查询都
 * 遍历线程表。
 */
static void _cpu_usage_update(void)
{
    rt_tick_t tick_now;
    rt_tick_t delta_tick;
    rt_uint64_t total_delta;
    rt_bool_t bypass_interval_check = RT_FALSE;

    if (!_cpu_usage_inited)
    {
        _cpu_usage_snapshot_init();
        bypass_interval_check = RT_TRUE;
    }

    tick_now = rt_tick_get();
    delta_tick = rt_tick_get_delta(_cpu_usage_sample_tick);
    if (!bypass_interval_check && delta_tick < RT_CPU_USAGE_CALC_INTERVAL_TICK)
    {
        return;
    }

    total_delta = _cpu_usage_calc_total_delta();
    _cpu_usage_refresh_threads(total_delta);
    _cpu_usage_sample_tick = tick_now;
}

/**
 * @brief 返回线程在最近一个采样窗口中的 CPU 使用率整数百分比。
 *
 * 这里使用运行时间增量，而不是自启动以来的累计时间。
 *
 * @param thread 目标线程，不得为 RT_NULL，且查询期间必须保持存活。
 *
 * @return 0 到 100 的整数。采样间隔尚未到达时返回先前缓存值，初始为 0。
 *
 * @note 仅在启用 RT_USING_CPU_USAGE_TRACER 时存在。
 * @note 计算公式为
 *       (thread_time_delta * 100) / total_time_delta,
 *       其中 total_time_delta 是全部 CPU 的 user/system/idle 增量之和。
 * @note 可通过 RT_CPU_USAGE_CALC_INTERVAL_MS 调整采样间隔。
 *
 * `_cpu_usage_lock` 串行化全局采样更新。它不会为传入线程增加引用，调用者需
 * 避免与线程销毁并发。
 */
rt_uint8_t rt_thread_get_usage(rt_thread_t thread)
{
    rt_uint8_t usage;

    RT_ASSERT(thread != RT_NULL);

    rt_spin_lock(&_cpu_usage_lock);
    _cpu_usage_update();
    usage = thread->cpu_usage;
    rt_spin_unlock(&_cpu_usage_lock);

    return usage;
}
#endif /* RT_USING_CPU_USAGE_TRACER */

#if defined(RT_USING_LIBC) && defined(RT_USING_FINSH)
#include <limits.h>
#include <stdlib.h> /* 用于把命令行中的线程地址字符串转换为整数。 */

/** backtrace shell 命令在对象遍历回调与调用者之间共享的查找状态。 */
struct cmd_backtrace_find_ctx
{
    rt_uintptr_t pid;  /**< 用户输入并通过语法/范围检查的目标地址。 */
    rt_thread_t thread; /**< 匹配的全局线程对象；尚未找到时为 RT_NULL。 */
};

/**
 * @brief 对象表遍历回调：按对象地址匹配 shell 命令输入。
 *
 * 回调在全局线程对象表自旋锁内运行，因此只比较和写入上下文，不做打印或
 * 回溯。返回正数 1 表示找到目标并请求 rt_object_for_each() 正常提前停止。
 */
static rt_err_t cmd_backtrace_match_thread(struct rt_object *object, void *data)
{
    struct cmd_backtrace_find_ctx *ctx = data;

    if ((rt_uintptr_t)object == ctx->pid)
    {
        ctx->thread = (rt_thread_t)object;

        return 1;
    }

    return RT_EOK;
}

#if UINTPTR_MAX > ULONG_MAX
/**
 * @brief 在 `unsigned long` 小于指针宽度的平台上手工格式化完整十六进制地址。
 *
 * 输出形式为 `0x...`。缓冲区不足 4 字节时只尽可能写入空串，避免随后打印
 * 一个被截断且看似有效的地址。
 */
static void cmd_backtrace_format_pid(rt_uintptr_t pid, char *buf, rt_size_t size)
{
    static const char hex[] = "0123456789abcdef";
    char digits[sizeof(rt_uintptr_t) * 2];
    rt_size_t count = 0;
    rt_size_t index;

    if ((buf == RT_NULL) || (size < 4))
    {
        if ((buf != RT_NULL) && (size > 0))
        {
            buf[0] = '\0';
        }
        return;
    }

    do
    {
        digits[count++] = hex[pid & 0xf];
        pid >>= 4;
    }
    while ((pid != 0) && (count < sizeof(digits)));

    buf[0] = '0';
    buf[1] = 'x';

    for (index = 0; index < count; index++)
    {
        buf[2 + index] = digits[count - index - 1];
    }

    buf[2 + count] = '\0';
}
#endif

/**
 * @brief 严格解析 shell 参数中的线程地址。
 *
 * 接受 strtoul/strtoull 支持的 0、0x 等基数前缀，但拒绝正负号、空输入、
 * 尾随字符、溢出以及零地址。成功后才写入 @p pid。
 */
static rt_bool_t cmd_backtrace_parse_pid(const char *arg, rt_uintptr_t *pid)
{
    char *end_ptr;
#if UINTPTR_MAX > ULONG_MAX
    unsigned long long parsed_value;
#else
    unsigned long parsed_value;
#endif
    rt_uintptr_t value;

    if ((arg == RT_NULL) || (pid == RT_NULL))
    {
        return RT_FALSE;
    }

    if ((*arg == '+') || (*arg == '-'))
    {
        return RT_FALSE;
    }

    errno = 0;
#if UINTPTR_MAX > ULONG_MAX
    parsed_value = strtoull(arg, &end_ptr, 0);
#else
    parsed_value = strtoul(arg, &end_ptr, 0);
#endif
    if ((end_ptr == arg) || (*end_ptr != '\0') ||
        (errno == ERANGE) ||
#if UINTPTR_MAX > ULONG_MAX
        (parsed_value > (unsigned long long)(rt_uintptr_t)-1))
#else
        (parsed_value > (unsigned long)(rt_uintptr_t)-1))
#endif
    {
        return RT_FALSE;
    }

    value = (rt_uintptr_t)parsed_value;
    if (value == 0)
    {
        return RT_FALSE;
    }

    *pid = value;

    return RT_TRUE;
}

/**
 * @brief 仅在当前全局线程对象表中验证地址确实对应一个线程。
 *
 * 这样避免直接解引用任意用户输入地址。对象表遍历结束后返回的是借用指针，
 * 本函数并未增加生命周期引用；命令执行环境仍需避免目标同步销毁。
 */
static rt_thread_t cmd_backtrace_find_thread(rt_uintptr_t pid)
{
    struct cmd_backtrace_find_ctx ctx =
    {
        .pid = pid,
        .thread = RT_NULL,
    };

    rt_object_for_each(RT_Object_Class_Thread, cmd_backtrace_match_thread, &ctx);

    return ctx.thread;
}

/**
 * @brief FinSH/MSH 的 `backtrace [thread_address]` 命令实现。
 *
 * 无参数时打印当前调用栈；一个参数时严格解析并在对象表验证目标，然后调用
 * rt_backtrace_thread()；其他参数数量显示帮助。这里把“地址当作 pid”只是
 * 命令行命名习惯，并非 RT-Thread 线程另有数值 PID 字段。
 */
static void cmd_backtrace(int argc, char** argv)
{
    rt_uintptr_t pid;
    rt_thread_t target;
#if UINTPTR_MAX > ULONG_MAX
    char pid_buf[sizeof(rt_uintptr_t) * 2 + 3];
#endif

    if (argc != 2)
    {
        if (argc == 1)
        {
            rt_kprintf("[INFO] No thread specified\n"
                "[HELP] You can use commands like: backtrace %p\n"
                "Printing backtrace of calling stack...\n",
                rt_thread_self());
            rt_backtrace();
            return ;
        }
        else
        {
            rt_kprintf("please use: backtrace [thread_address]\n");
            return;
        }
    }

    if (!cmd_backtrace_parse_pid(argv[1], &pid))
    {
        rt_kprintf("Invalid input: %s\n", argv[1]);
        return ;
    }

    target = cmd_backtrace_find_thread(pid);
#if UINTPTR_MAX > ULONG_MAX
    cmd_backtrace_format_pid(pid, pid_buf, sizeof(pid_buf));
#endif
    if (target != RT_NULL)
    {
#if UINTPTR_MAX > ULONG_MAX
        rt_kprintf("backtrace %s(%s), from %s\n", target->parent.name, pid_buf, argv[1]);
#else
        rt_kprintf("backtrace %s(0x%lx), from %s\n",
                   target->parent.name, (unsigned long)pid, argv[1]);
#endif
        rt_backtrace_thread(target);
    }
    else
    {
#if UINTPTR_MAX > ULONG_MAX
        rt_kprintf("Invalid pid: %s\n", pid_buf);
#else
        rt_kprintf("Invalid pid: %lx\n", (unsigned long)pid);
#endif
    }
}
MSH_CMD_EXPORT_ALIAS(cmd_backtrace, backtrace, print backtrace of a thread);

#endif /* RT_USING_LIBC */

#if defined(RT_USING_HEAP) && !defined(RT_USING_USERHEAP)
#ifdef RT_USING_HOOK
static void (*rt_malloc_hook)(void **ptr, rt_size_t size);
static void (*rt_realloc_entry_hook)(void **ptr, rt_size_t size);
static void (*rt_realloc_exit_hook)(void **ptr, rt_size_t size);
static void (*rt_free_hook)(void **ptr);

/**
 * @ingroup group_hook
 * @{
 */

/**
 * @brief 安装 rt_malloc() 返回前的单监听者钩子。
 *
 * @param hook 回调；RT_NULL 表示禁用。
 *
 * 回调在系统堆锁释放后执行，参数是“结果指针变量”的地址和请求长度。写入
 * `*ptr` 会改变 rt_malloc() 最终返回值，因此纯追踪钩子不应修改它。钩子
 * 继承分配调用者上下文，不得递归使用系统堆。
 */
void rt_malloc_sethook(void (*hook)(void **ptr, rt_size_t size))
{
    rt_malloc_hook = hook;
}

/**
 * @brief 安装 rt_realloc() 进入系统堆前的钩子。
 *
 * @param hook 回调；RT_NULL 表示禁用。
 *
 * 回调接收旧指针变量地址和新长度，并在取得堆锁之前执行。修改 `*ptr` 会改变
 * 实际被 realloc 的对象；通常只应观察。禁止在钩子中递归分配。
 */
void rt_realloc_set_entry_hook(void (*hook)(void **ptr, rt_size_t size))
{
    rt_realloc_entry_hook = hook;
}

/**
 * @brief 安装 rt_realloc() 完成并释放系统堆锁后的钩子。
 *
 * @param hook 回调；RT_NULL 表示禁用。修改结果变量会改变调用者收到的地址，
 *             但不会自动释放被替换的真实结果。
 */
void rt_realloc_set_exit_hook(void (*hook)(void **ptr, rt_size_t size))
{
    rt_realloc_exit_hook = hook;
}

/**
 * @brief 安装 rt_free() 取得系统堆锁之前的钩子。
 *
 * @param hook 回调；RT_NULL 表示禁用。
 *
 * 回调接收待释放指针变量的地址。它在空指针检查之前调用，修改 `*ptr` 会改变
 * 实际释放目标，必须非常谨慎。钩子可能继承中断上下文（取决于堆配置），
 * 不得阻塞或递归调用堆服务。
 */
void rt_free_sethook(void (*hook)(void **ptr))
{
    rt_free_hook = hook;
}

/**@}*/

#endif /* RT_USING_HOOK */

#if defined(RT_USING_HEAP_ISR)
static struct rt_spinlock _heap_spinlock;
#elif defined(RT_USING_MUTEX)
static struct rt_mutex _lock;
#endif

/**
 * @brief 初始化系统堆包装层选择的并发保护原语。
 *
 * RT_USING_HEAP_ISR 使用自旋锁并保存中断状态；普通线程堆在有 mutex 支持时
 * 使用可睡眠互斥量；两者都未启用则退化为调度临界区。真正的分配器后端在
 * `_MEM_*` 宏之后选择，与这里的锁策略相互独立。
 */
rt_inline void _heap_lock_init(void)
{
#if defined(RT_USING_HEAP_ISR)
    rt_spin_lock_init(&_heap_spinlock);
#elif defined(RT_USING_MUTEX)
    rt_mutex_init(&_lock, "heap", RT_IPC_FLAG_PRIO);
#endif
}

/**
 * @brief 进入系统堆临界区，并返回与所选后端配对的恢复值。
 *
 * 自旋锁分支返回原中断状态；mutex 分支返回 take 状态；临界区分支返回
 * RT_EOK。mutex 分支在启动早期没有当前线程时不加锁，假设此时尚无并发线程。
 */
rt_inline rt_base_t _heap_lock(void)
{
#if defined(RT_USING_HEAP_ISR)
    return rt_spin_lock_irqsave(&_heap_spinlock);
#elif defined(RT_USING_MUTEX)
    if (rt_thread_self())
        return rt_mutex_take(&_lock, RT_WAITING_FOREVER);
    else
        return RT_EOK;
#else
    rt_enter_critical();
    return RT_EOK;
#endif
}

/** @brief 使用 _heap_lock() 返回的原值退出对应系统堆临界区。 */
rt_inline void _heap_unlock(rt_base_t level)
{
#if defined(RT_USING_HEAP_ISR)
    rt_spin_unlock_irqrestore(&_heap_spinlock, level);
#elif defined(RT_USING_MUTEX)
    RT_ASSERT(level == RT_EOK);
    if (rt_thread_self())
        rt_mutex_release(&_lock);
#else
    rt_exit_critical();
#endif
}

#ifdef RT_USING_UTESTCASES
/* 向单元测试暴露内部加锁路径，以便验证不同配置的语句和状态。 */
#ifdef _MSC_VER
#define rt_heap_lock() _heap_lock()
#define rt_heap_unlock() _heap_unlock()
#else
rt_base_t rt_heap_lock(void) __attribute__((alias("_heap_lock")));
void rt_heap_unlock(rt_base_t level) __attribute__((alias("_heap_unlock")));
#endif /* _MSC_VER */
#endif

#if defined(RT_USING_SMALL_MEM_AS_HEAP)
static rt_smem_t system_heap;
/** 从 small-memory 通用统计对象复制系统堆快照。调用者已经持有外层堆锁。 */
rt_inline void _smem_info(rt_size_t *total,
    rt_size_t *used, rt_size_t *max_used)
{
    if (total)
        *total = system_heap->total;
    if (used)
        *used = system_heap->used;
    if (max_used)
        *max_used = system_heap->max;
}
#define _MEM_INIT(_name, _start, _size) \
    system_heap = rt_smem_init(_name, _start, _size)
#define _MEM_MALLOC(_size)  \
    rt_smem_alloc(system_heap, _size)
#define _MEM_REALLOC(_ptr, _newsize)\
    rt_smem_realloc(system_heap, _ptr, _newsize)
#define _MEM_FREE(_ptr) \
    rt_smem_free(_ptr)
#define _MEM_INFO(_total, _used, _max)  \
    _smem_info(_total, _used, _max)
#elif defined(RT_USING_MEMHEAP_AS_HEAP)
static struct rt_memheap system_heap;
void *_memheap_alloc(struct rt_memheap *heap, rt_size_t size);
void _memheap_free(void *rmem);
void *_memheap_realloc(struct rt_memheap *heap, void *rmem, rt_size_t newsize);
#define _MEM_INIT(_name, _start, _size) \
    do {\
        rt_memheap_init(&system_heap, _name, _start, _size); \
        system_heap.locked = RT_TRUE; \
    } while(0)
#define _MEM_MALLOC(_size)  \
    _memheap_alloc(&system_heap, _size)
#define _MEM_REALLOC(_ptr, _newsize)    \
    _memheap_realloc(&system_heap, _ptr, _newsize)
#define _MEM_FREE(_ptr)   \
    _memheap_free(_ptr)
#define _MEM_INFO(_total, _used, _max)   \
    rt_memheap_info(&system_heap, _total, _used, _max)
#elif defined(RT_USING_SLAB_AS_HEAP)
static rt_slab_t system_heap;
/** 从 slab 的通用内存父对象复制系统堆统计。调用者已经持有外层堆锁。 */
rt_inline void _slab_info(rt_size_t *total,
    rt_size_t *used, rt_size_t *max_used)
{
    if (total)
        *total = system_heap->total;
    if (used)
        *used = system_heap->used;
    if (max_used)
        *max_used = system_heap->max;
}
#define _MEM_INIT(_name, _start, _size) \
    system_heap = rt_slab_init(_name, _start, _size)
#define _MEM_MALLOC(_size)  \
    rt_slab_alloc(system_heap, _size)
#define _MEM_REALLOC(_ptr, _newsize)    \
    rt_slab_realloc(system_heap, _ptr, _newsize)
#define _MEM_FREE(_ptr) \
    rt_slab_free(system_heap, _ptr)
#define _MEM_INFO       _slab_info
#else
#define _MEM_INIT(...)
#define _MEM_MALLOC(...)     RT_NULL
#define _MEM_REALLOC(...)    RT_NULL
#define _MEM_FREE(...)
#define _MEM_INFO(...)
#endif

/**
 * @brief 对齐一段地址范围并初始化所配置的系统堆后端和外层锁。
 *
 * @param begin_addr 原始可用区域起点。
 *
 * @param end_addr 原始区域的开区间终点，不属于堆。
 *
 * `_MEM_INIT` 在编译期映射到 small-memory、memheap 或 slab。若没有选择任何
 * 后端，宏为空且后续分配始终失败。调用者必须在并发分配发生前完成初始化。
 */
void rt_system_heap_init_generic(void *begin_addr, void *end_addr)
{
    rt_uintptr_t begin_align = RT_ALIGN((rt_uintptr_t)begin_addr, RT_ALIGN_SIZE);
    rt_uintptr_t end_align   = RT_ALIGN_DOWN((rt_uintptr_t)end_addr, RT_ALIGN_SIZE);

    RT_ASSERT(end_align > begin_align);

    /* 使用对齐后的半开区间初始化选定分配器。 */
    _MEM_INIT("heap", (void *)begin_align, end_align - begin_align);
    /* 分配器就绪后再建立统一的多线程竞争保护。 */
    _heap_lock_init();
}

/**
 * @brief 可由板级/诊断实现覆盖的系统堆初始化入口。
 *
 * @param begin_addr 内存区起点。
 *
 * @param end_addr 内存区开区间终点。
 *
 * 弱默认实现直接调用 generic 版本。覆盖者可插入 heap sanitizer 等工作，但
 * 必须最终建立与 rt_malloc 系列兼容的后端和锁。
 */
rt_weak void rt_system_heap_init(void *begin_addr, void *end_addr)
{
    rt_system_heap_init_generic(begin_addr, end_addr);
}

/**
 * @brief 从统一系统堆分配至少 @p size 字节。
 *
 * @param size 请求长度，具体 0 长度行为由所选后端决定。
 *
 * @return 成功返回用户地址，失败返回 RT_NULL。
 *
 * 先在统一外层锁内调用后端，解锁后再触发 malloc 钩子，因此钩子不会处于
 * 堆锁内，但仍继承调用者的线程/中断上下文。该函数为弱符号，用户堆实现可
 * 覆盖整个入口。
 */
rt_weak void *rt_malloc(rt_size_t size)
{
    rt_base_t level;
    void *ptr;

    /* 进入所选锁策略保护的系统堆临界区。 */
    level = _heap_lock();
    /* 编译期宏分派到唯一选定的分配器后端。 */
    ptr = _MEM_MALLOC(size);
    /* 在调用用户钩子之前释放堆锁。 */
    _heap_unlock(level);
    /* 钩子看到活动结果变量，理论上可以改写返回值。 */
    RT_OBJECT_HOOK_CALL(rt_malloc_hook, (&ptr, size));
    return ptr;
}
RTM_EXPORT(rt_malloc);

/**
 * @brief 调整系统堆块大小。
 *
 * @param ptr 原块；RT_NULL/新长度 0 的具体兼容语义由后端实现。
 *
 * @param newsize 新长度。
 *
 * @return 成功返回新地址；失败返回 RT_NULL，后端通常保持原块有效。
 *
 * entry 钩子在加锁前运行，exit 钩子在解锁后运行；两者接收不同的活动指针
 * 变量。函数本身不额外复制数据，全部 realloc 语义由 `_MEM_REALLOC` 后端负责。
 */
rt_weak void *rt_realloc(void *ptr, rt_size_t newsize)
{
    rt_base_t level;
    void *nptr;

    /* 入口钩子可观察甚至改变真正传给后端的旧指针。 */
    RT_OBJECT_HOOK_CALL(rt_realloc_entry_hook, (&ptr, newsize));
    /* 串行化后端元数据修改。 */
    level = _heap_lock();
    /* 由选定后端尝试原地调整或搬迁。 */
    nptr = _MEM_REALLOC(ptr, newsize);
    /* 后端状态稳定后释放锁。 */
    _heap_unlock(level);
    /* 出口钩子接收最终结果变量。 */
    RT_OBJECT_HOOK_CALL(rt_realloc_exit_hook, (&nptr, newsize));
    return nptr;
}
RTM_EXPORT(rt_realloc);

/**
 * @brief 连续分配 @p count 个、每个 @p size 字节的对象并清零。
 *
 * @note 成功区域的每一个字节都会被写成 0。
 *
 * @param count 对象个数。
 *
 * @param size 单个对象字节数。
 *
 * @return 成功返回清零区域，失败返回 RT_NULL。
 *
 * @warning 当前实现直接计算 `count * size`，没有乘法溢出检查。调用者必须
 *          先保证乘积能由 rt_size_t 表示，否则可能分配过小区域。
 */
rt_weak void *rt_calloc(rt_size_t count, rt_size_t size)
{
    void *p;

    /* 先按总字节数调用统一分配入口。 */
    p = rt_malloc(count * size);
    /* 仅在分配成功后清零，避免解引用空指针。 */
    if (p)
    {
        rt_memset(p, 0, count * size);
    }
    return p;
}
RTM_EXPORT(rt_calloc);

/**
 * @brief 把 rt_malloc 系列返回的块归还系统堆。
 *
 * @param ptr 待释放地址；RT_NULL 被忽略。
 *
 * free 钩子在空指针检查和加锁之前运行，而且接收活动变量地址。随后函数在
 * 统一锁内调用唯一选定后端。错误来源、重复释放和指针归属验证取决于后端。
 */
rt_weak void rt_free(void *ptr)
{
    rt_base_t level;

    /* 钩子既能观察也能改变实际释放目标。 */
    RT_OBJECT_HOOK_CALL(rt_free_hook, (&ptr));
    /* 钩子执行后再检查，因此钩子可以把目标改成 RT_NULL 以取消释放。 */
    if (ptr == RT_NULL) return;
    /* 保护后端空闲链、页表或 slab 区域元数据。 */
    level = _heap_lock();
    _MEM_FREE(ptr);
    /* 归还完成后恢复锁/中断状态。 */
    _heap_unlock(level);
}
RTM_EXPORT(rt_free);

/**
* @brief 读取系统堆总量、当前用量和历史峰值。
*
* @param total 可选总容量输出。
*
* @param used 可选当前用量输出。
*
* @param max_used 可选历史峰值输出。
*
* 三个指针是否允许为空由后端信息函数处理；当前内置后端均允许。整个快照在
* 系统堆外层锁内获取，使相关统计不会来自一次分配的不同中间阶段。
*/
rt_weak void rt_memory_info(rt_size_t *total,
                            rt_size_t *used,
                            rt_size_t *max_used)
{
    rt_base_t level;

    /* 与分配/释放使用同一把外层锁，得到一致统计。 */
    level = _heap_lock();
    _MEM_INFO(total, used, max_used);
    /* 输出写入完成后释放锁。 */
    _heap_unlock(level);
}
RTM_EXPORT(rt_memory_info);

#if defined(RT_USING_SLAB) && defined(RT_USING_SLAB_AS_HEAP)
void *rt_page_alloc(rt_size_t npages)
{
    rt_base_t level;
    void *ptr;

    /* 页分配器与普通 slab 分配共享元数据，使用同一系统堆锁。 */
    level = _heap_lock();
    /* 返回连续 npages 个 RT_MM_PAGE_SIZE 页面。 */
    ptr = rt_slab_page_alloc(system_heap, npages);
    /* 退出临界区后页面归调用者独占。 */
    _heap_unlock(level);
    return ptr;
}

void rt_page_free(void *addr, rt_size_t npages)
{
    rt_base_t level;

    /* 页空闲链修改必须与普通 slab 操作互斥。 */
    level = _heap_lock();
    /* 调用者必须传回完全相同的起点和页数。 */
    rt_slab_page_free(system_heap, addr, npages);
    /* 页面重新进入 slab 页分配器后释放外层锁。 */
    _heap_unlock(level);
}
#endif

/**
 * @brief 分配一个起始地址满足 @p align 对齐要求的系统堆块。
 *
 * @param size 用户需要的字节数。
 *
 * @param align 对齐值。算法按位取整，调用者应传入非零的 2 的幂；函数只把它
 *              向上调整到指针大小整数倍，并不会验证幂次条件。
 *
 * @return 成功返回对齐用户地址，系统堆不足时返回 RT_NULL。
 *
 * 函数额外申请对齐余量，并在返回地址前一个指针槽保存 rt_malloc() 的真实
 * 返回值。因此这种地址必须用 rt_free_align() 释放，不能直接传给 rt_free()。
 */
rt_weak void *rt_malloc_align(rt_size_t size, rt_size_t align)
{
    void *ptr = RT_NULL;
    void *align_ptr = RT_NULL;
    int uintptr_size = 0;
    rt_size_t align_size = 0;

    /* uintptr_size 暂存“指针字节数减一”，用于向上取整。 */
    uintptr_size = sizeof(void*);
    uintptr_size -= 1;

    /* 至少保证保存真实指针的隐藏槽满足指针对齐。 */
    align = ((align + uintptr_size) & ~uintptr_size);

    /* 用户长度取整后再增加一个 align 余量，其中包含隐藏指针槽。 */
    align_size = ((size + uintptr_size) & ~uintptr_size) + align;
    /* 底层真实块仍由普通系统堆管理。 */
    ptr = rt_malloc(align_size);
    if (ptr != RT_NULL)
    {
        /* 已对齐时也前移 align，确保前面有空间保存真实指针。 */
        if (((rt_uintptr_t)ptr & (align - 1)) == 0)
        {
            align_ptr = (void *)((rt_uintptr_t)ptr + align);
        }
        else
        {
            align_ptr = (void *)(((rt_uintptr_t)ptr + (align - 1)) & ~(align - 1));
        }

        /* 在用户不可见的前一个指针槽记录真实分配起点。 */
        *((rt_uintptr_t *)((rt_uintptr_t)align_ptr - sizeof(void *))) = (rt_uintptr_t)ptr;

        ptr = align_ptr;
    }

    return ptr;
}
RTM_EXPORT(rt_malloc_align);

/**
 * @brief 释放 rt_malloc_align() 返回的对齐块。
 *
 * @param ptr 对齐用户地址；RT_NULL 被忽略。
 *
 * 函数读取用户地址前方隐藏槽恢复真实堆指针，再交给 rt_free()。传入普通
 * rt_malloc 地址或已经释放的地址会读取无效元数据，属于未定义的调用错误。
 */
rt_weak void rt_free_align(void *ptr)
{
    void *real_ptr = RT_NULL;

    /* 空指针无需访问其前方隐藏槽。 */
    if (ptr == RT_NULL) return;
    real_ptr = (void *) * (rt_uintptr_t *)((rt_uintptr_t)ptr - sizeof(void *));
    rt_free(real_ptr);
}
RTM_EXPORT(rt_free_align);
#endif /* RT_USING_HEAP */

/**
 * @brief 查找 32 位整数中最高置位的位置。
 * @details 最低有效位编号为 1，最高有效位编号为 32；输入为 0 时返回 0。
 *
 * 示例：
 * - fls(0) = 0
 * - fls(1) = 1
 * - fls(0x80000000) = 32
 *
 * @param val 要检查的 32 位位图。
 * @return 最高置位的 1 基编号（1～32），0 表示没有置位。
 *
 * 实现通过分段左移和缩小候选范围完成，不需要循环扫描 32 次。
 */
int __rt_fls(int val)
{
    int bit = 32;

    if (!val)
    {
        return 0;
    }
    if (!(val & 0xffff0000u))
    {
        val <<= 16;
        bit -= 16;
    }
    if (!(val & 0xff000000u))
    {
        val <<= 8;
        bit -= 8;
    }
    if (!(val & 0xf0000000u))
    {
        val <<= 4;
        bit -= 4;
    }
    if (!(val & 0xc0000000u))
    {
        val <<= 2;
        bit -= 2;
    }
    if (!(val & 0x80000000u))
    {
        bit -= 1;
    }

    return bit;
}

#ifndef RT_USING_CPU_FFS
#ifdef RT_USING_TINY_FFS
const rt_uint8_t __lowest_bit_bitmap[] =
{
    /*  0 - 7  */  0,  1,  2, 27,  3, 24, 28, 32,
    /*  8 - 15 */  4, 17, 25, 31, 29, 12, 32, 14,
    /* 16 - 23 */  5,  8, 18, 32, 26, 23, 32, 16,
    /* 24 - 31 */ 30, 11, 13,  7, 32, 22, 15, 10,
    /* 32 - 36 */  6, 21,  9, 20, 19
};

/**
 * @brief 查找从最低位开始遇到的第一个置位。
 *
 * 位编号从 1 开始；返回 0 表示输入没有任何置位。
 *
 * @param value 待查询位图。
 *
 * @return 最低置位的 1 基编号；@p value 为 0 时返回 0。
 *
 * tiny 实现先用 `value & (value - 1) ^ value` 隔离最低置位，再对 37 取模，
 * 通过预计算表映射到位号，以较小查表空间换取常数时间。
 */
int __rt_ffs(int value)
{
    return __lowest_bit_bitmap[(rt_uint32_t)(value & (value - 1) ^ value) % 37];
}
#else
const rt_uint8_t __lowest_bit_bitmap[] =
{
    /* 00 */ 0, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 10 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 20 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 30 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 40 */ 6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 50 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 60 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 70 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 80 */ 7, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* 90 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* A0 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* B0 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* C0 */ 6, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* D0 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* E0 */ 5, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0,
    /* F0 */ 4, 0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0
};

/**
 * @brief 查找从最低位开始遇到的第一个置位。
 *
 * 位编号从 1 开始；返回 0 表示输入没有任何置位。
 *
 * @param value 待查询位图。
 *
 * @return 最低置位的 1 基编号；@p value 为 0 时返回 0。
 *
 * 通用实现依次检查四个字节，只对第一个非零字节访问 256 项查表，再加上
 * 字节偏移得到最终位号。
 */
int __rt_ffs(int value)
{
    if (value == 0)
    {
        return 0;
    }

    if (value & 0xff)
    {
        return __lowest_bit_bitmap[value & 0xff] + 1;
    }

    if (value & 0xff00)
    {
        return __lowest_bit_bitmap[(value & 0xff00) >> 8] + 9;
    }

    if (value & 0xff0000)
    {
        return __lowest_bit_bitmap[(value & 0xff0000) >> 16] + 17;
    }

    return __lowest_bit_bitmap[(value & 0xff000000) >> 24] + 25;
}
#endif /* RT_USING_TINY_FFS */
#endif /* RT_USING_CPU_FFS */

#ifdef RT_DEBUGING_ASSERT
/* RT_ASSERT(EX) 使用的单监听者失败钩子。 */

void (*rt_assert_hook)(const char *ex, const char *func, rt_size_t line);

/**
 * @brief 安装断言表达式为假时调用的处理钩子。
 *
 * @param hook 回调；RT_NULL 恢复默认的打印、回溯和停机行为。
 *
 * 设置操作没有加锁，通常应在启动阶段完成。钩子会在断言发生的原始上下文中
 * 同步执行，可能处于中断、持锁或内核状态已损坏的环境，只能使用极少量可靠
 * 服务。钩子返回后 rt_assert_handler() 也会返回，即是否停机由钩子决定。
 */
void rt_assert_set_hook(void (*hook)(const char *ex, const char *func, rt_size_t line))
{
    rt_assert_hook = hook;
}

/**
 * @brief RT_ASSERT 宏最终进入的失败处理函数。
 *
 * @param ex_string 被字符串化的失败表达式。
 *
 * @param func 断言所在函数名。
 *
 * @param line 源文件行号。
 *
 * 没有自定义钩子时，动态模块中的断言只终止当前模块；内核本体断言会打印
 * 信息、尝试回溯，然后在 volatile 条件循环中永久停住，便于调试器接管。
 * 自定义钩子存在时完全委托钩子，函数不会自动停机。
 */
void rt_assert_handler(const char *ex_string, const char *func, rt_size_t line)
{
    volatile char dummy = 0;

    if (rt_assert_hook == RT_NULL)
    {
#ifdef RT_USING_MODULE
        if (dlmodule_self())
        {
            /* 模块断言隔离在模块退出路径，避免直接冻结整个内核。 */
            dlmodule_exit(-1);
        }
        else
#endif /*RT_USING_MODULE*/
        {
            rt_kprintf("(%s) assertion failed at function:%s, line number:%d \n", ex_string, func, line);
            rt_backtrace();
            while (dummy == 0);
        }
    }
    else
    {
        rt_assert_hook(ex_string, func, line);
    }
}
RTM_EXPORT(rt_assert_handler);
#endif /* RT_DEBUGING_ASSERT */

/**@}*/
