/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2024-09-22     Meco Man     首个版本
 */

/**
 * @file kerrno.c
 * @brief RT-Thread 错误码文本和“线程局部优先、全局后备”的 errno 存储。
 *
 * 正常线程运行时，错误值保存在当前线程控制块的 `error` 字段，因此不同线程
 * 互不覆盖。中断上下文以及调度器尚未选出当前线程的启动早期没有合适 TCB，
 * 只能共用 `__rt_errno`。这个全局后备槽不是 SMP 原子日志：嵌套中断或另一
 * CPU 可能覆盖它，所以它只适合临时诊断，不能用于同步或持久错误传递。
 */

#include <rtthread.h>

/**
 * @brief 中断和无当前线程阶段共用的 errno 后备槽。
 *
 * volatile 只阻止编译器省略每次访问，不提供跨 CPU 原子性或线程隔离。其类型
 * 是 int；在 rt_err_t 更宽的平台上，经该槽或 `_rt_errno()` lvalue 访问的值
 * 必须保持在 int 可表示范围内。
 */
static volatile int __rt_errno;

/**
 * @struct _errno_str_t
 * @brief 一项“RT-Thread 错误码—固定短文本”映射。
 *
 * 文本为静态只读字符串，主要用于紧凑诊断输出，并不是本地化的完整错误说明。
 */
struct _errno_str_t
{
    rt_err_t error;      /**< 正值形式的 RT_E* 错误码。 */
    const char *str;     /**< 对应固定宽度短文本，存储期覆盖整个系统运行。 */
};

/**
 * @brief 内置错误码到短文本的线性查找表。
 *
 * 表很小，因此 rt_strerror() 直接线性扫描；未列出的扩展错误统一返回
 * `"EUNKNOW"`。字符串内容是对外诊断格式，不应由调用者修改或释放。
 */
static struct _errno_str_t  rt_errno_strs[] =
{
    {RT_EOK     , "OK     "},  /**< 操作成功。 */
    {RT_ERROR   , "ERROR  "},  /**< 未细分的通用错误。 */
    {RT_ETIMEOUT, "ETIMOUT"},  /**< 等待超过时限。 */
    {RT_EFULL   , "ERSFULL"},  /**< 有界资源已满。 */
    {RT_EEMPTY  , "ERSEPTY"},  /**< 有界资源为空。 */
    {RT_ENOMEM  , "ENOMEM "},  /**< 内存不足。 */
    {RT_ENOSYS  , "ENOSYS "},  /**< 功能尚未实现。 */
    {RT_EBUSY   , "EBUSY  "},  /**< 资源正忙。 */
    {RT_EIO     , "EIO    "},  /**< 输入/输出错误。 */
    {RT_EINTR   , "EINTRPT"},  /**< 操作被信号等事件中断。 */
    {RT_EINVAL  , "EINVAL "},  /**< 参数无效。 */
    {RT_ENOENT  , "ENOENT "},  /**< 指定对象或条目不存在。 */
    {RT_ENOSPC  , "ENOSPC "},  /**< 存储或设备空间不足。 */
    {RT_EPERM   , "EPERM  "},  /**< 操作没有权限。 */
    {RT_ETRAP   , "ETRAP  "},  /**< 陷阱/异常错误。 */
};

/**
 * @brief 把错误码转换为静态短文本。
 *
 * @param error 正值或负值形式均可；函数先取其绝对值再查表。
 * @return 匹配文本，未知错误返回静态字符串 `"EUNKNOW"`。
 *
 * @warning 对最小负值直接取负在有符号 C 类型中可能溢出；常规 RT_E* 值均很
 *          小，不会触发该边界。返回指针不得修改或释放。
 */
const char *rt_strerror(rt_err_t error)
{
    int i = 0;

    if (error < 0)
        error = -error;

    for (i = 0; i < sizeof(rt_errno_strs) / sizeof(rt_errno_strs[0]); i++)
    {
        if (rt_errno_strs[i].error == error)
            return rt_errno_strs[i].str;
    }

    return "EUNKNOW";
}
RTM_EXPORT(rt_strerror);

/**
 * @brief 读取当前执行上下文对应的 errno。
 *
 * @return 中断/启动早期返回全局后备槽，正常线程返回其 TCB error 字段。
 */
rt_err_t rt_get_errno(void)
{
    rt_thread_t tid = RT_NULL;

    if (rt_interrupt_get_nest() != 0)
    {
        /* ISR 不使用被中断线程的私有 errno，避免污染其后续错误处理。 */
        return __rt_errno;
    }

    tid = rt_thread_self();
    if (tid == RT_NULL)
    {
        return __rt_errno;
    }

    return tid->error;
}
RTM_EXPORT(rt_get_errno);

/**
 * @brief 写入当前执行上下文对应的 errno。
 *
 * @param error 原样保存的值；函数不统一正负号。
 */
void rt_set_errno(rt_err_t error)
{
    rt_thread_t tid = RT_NULL;

    if (rt_interrupt_get_nest() != 0)
    {
        /* 中断上下文写入共享后备槽。 */
        __rt_errno = error;

        return;
    }

    tid = rt_thread_self();
    if (tid == RT_NULL)
    {
        __rt_errno = error;

        return;
    }

    tid->error = error;
}
RTM_EXPORT(rt_set_errno);

/**
 * @brief 返回可作为 C `errno` 左值使用的 int 指针。
 *
 * @return 中断/启动阶段返回全局槽地址，正常线程返回 TCB error 字段地址。
 *
 * @warning TCB 字段类型是 rt_err_t，此处为兼容 errno 强制转换成 int 指针。
 *          当两者宽度不同时，通过该指针只会访问 int 宽度部分；调用者应把值
 *          限定在 int 范围内，并避免与 rt_get/set_errno 混用超宽值。
 */
int *_rt_errno(void)
{
    rt_thread_t tid = RT_NULL;

    if (rt_interrupt_get_nest() != 0)
    {
        return (int *)&__rt_errno;
    }

    tid = rt_thread_self();
    if (tid != RT_NULL)
    {
        return (int *) & (tid->error);
    }

    return (int *)&__rt_errno;
}
RTM_EXPORT(_rt_errno);
