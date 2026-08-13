/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 变更记录：
 * 日期 作者备注
 * 2024-09-22 Meco Man 第一版
 */

/**
 * @file kerrno.h
 * @brief RT-Thread 错误码命名空间和与执行上下文相关的 errno 接口。
 *
 * RT-Thread 在内核 API 中使用 `RT_E*` 名称，同时可选择与使用 `E*` 数值及
 * `errno` 左值的 POSIX/libc 代码互操作。在使用完整且非 Nano libc 的构建中，
 * 大多数 RT-Thread 常量都是最接近的 POSIX 数值的别名。裸机式和 Nano 配置使用
 * 紧凑的私有编号，而不是 POSIX 常量别名。这并不表示每个非 Nano 构建都不会包含
 * `<sys/errno.h>`：即使选用了紧凑常量，rttypes.h 仍会为非 Nano 配置包含该系统头文件。
 *
 * 错误码的正负号是 API 层面的约定，并非此处强制的属性。许多内核函数返回
 * `-RT_E*`，而 POSIX 接口通常返回 -1，并在 errno 中保存正的 `E*` 数值。
 * `rt_set_errno()` 会原样保存传入值，不会统一正负号；`rt_get_errno()` 返回选中
 * 存储槽中的数值。因此调用者必须遵循所实现接口的约定。数值应能由 `int` 表示：
 * 中断/早期启动存储槽以及 `_rt_errno()` 左值接口的宽度均为 int，在已配置的 64 位
 * 目标上它可能比 rt_err_t 更窄。
 *
* errno 的存储位置取决于执行上下文。正在运行的线程使用其 TCB 的 `error` 字段，
 * 因而具备通常的线程局部行为。中断上下文和早期启动/尚无当前线程的上下文共用一个
 * 后备全局槽。因此 ISR 中的 errno 只是短暂的共享诊断状态：嵌套或稍后的中断，或其他
 * CPU，都可能覆盖它；不得将其用于同步或需要长期保留的错误报告。
 */

#ifndef __RT_KERRNO_H__
#define __RT_KERRNO_H__

/* 功能选择决定 POSIX 映射；rttypes.h 提供 rt_err_t。 */
#include <rtconfig.h>
#include <rttypes.h>

#ifdef __cplusplus
/* 供 C++ 代码包含错误接口时，保持 C 链接方式。 */
extern "C" {
#endif

#if defined(RT_USING_LIBC) && !defined(RT_USING_NANO)
/**
 * @name POSIX 兼容 libc 构建中的错误值
 *
 * 别名保留宿主/libc 的数值，使错误能在 RT-Thread 与 POSIX 接口之间传递而无需
 * 转换表。RT_ERROR、RT_ETRAP 以及调度器诊断码使用 RT-Thread 专用保留值。
 * RT_EFULL 和 RT_ENOSPC 有意映射到相同的 POSIX 状态，而 RT_EEMPTY 使用 ENODATA。
 * @{
 */
#define RT_EOK                          0               /**< 操作成功；绝不能对此值取负。 */
#define RT_ERROR                        255             /**< RT-Thread 的通用或未分类失败。 */
#define RT_ETIMEOUT                     ETIMEDOUT       /**< 操作超过允许的等待时间。 */
#define RT_EFULL                        ENOSPC          /**< 有界 RT-Thread 资源没有空余容量。 */
#define RT_EEMPTY                       ENODATA         /**< 有界 RT-Thread 资源当前没有数据。 */
#define RT_ENOMEM                       ENOMEM          /**< 内存分配失败或所需内存不可用。 */
#define RT_ENOSYS                       ENOSYS          /**< 请求的操作尚未实现。 */
#define RT_EBUSY                        EBUSY           /**< 资源正忙，无法继续操作。 */
#define RT_EIO                          EIO             /**< 设备或其他输入输出操作失败。 */
#define RT_EINTR                        EINTR           /**< 阻塞操作在完成前被中断。 */
#define RT_EINVAL                       EINVAL          /**< 参数值或参数组合无效。 */
#define RT_ENOENT                       ENOENT          /**< 请求的具名对象或条目不存在。 */
#define RT_ENOSPC                       ENOSPC          /**< 存储、设备或资源已无剩余空间。 */
#define RT_EPERM                        EPERM           /**< 调用者无权执行该操作。 */
#define RT_EFAULT                       EFAULT          /**< 所提供的地址无法按要求访问。 */
#define RT_ENOBUFS                      ENOBUFS         /**< 所需的网络或系统缓冲区不可用。 */
#define RT_ESCHEDISR                    253             /**< 调用者在 ISR 中，立即切换被延后。 */
#define RT_ESCHEDLOCKED                 252             /**< 嵌套调度器临界区使立即切换被延后。 */
#define RT_ETRAP                        254             /**< RT-Thread 陷阱或异常事件。 */
/** @} */
#else
/**
 * @name 裸机式或 Nano 构建使用的紧凑错误值
 *
 * 这些较小且稳定的数值不是平台 POSIX errno 编号的别名。它们只描述 RT-Thread
 * 状态；当接口边界需要平台特定的 POSIX 数值时，必须显式映射。非 Nano 构建仍可能
 * 通过 rttypes.h 间接包含系统 errno 头文件。
 * @{
 */
#define RT_EOK                          0               /**< 操作成功；绝不能对此值取负。 */
#define RT_ERROR                        1               /**< 通用或未分类失败。 */
#define RT_ETIMEOUT                     2               /**< 操作超过允许的等待时间。 */
#define RT_EFULL                        3               /**< 有界资源没有空余容量。 */
#define RT_EEMPTY                       4               /**< 有界资源当前没有数据。 */
#define RT_ENOMEM                       5               /**< 无法取得所需内存。 */
#define RT_ENOSYS                       6               /**< 请求的操作尚未实现。 */
#define RT_EBUSY                        7               /**< 资源正忙，无法继续操作。 */
#define RT_EIO                          8               /**< 设备或其他输入输出操作失败。 */
#define RT_EINTR                        9               /**< 阻塞操作被中断。 */
#define RT_EINVAL                       10              /**< 参数值或参数组合无效。 */
#define RT_ENOENT                       11              /**< 请求的具名对象或条目不存在。 */
#define RT_ENOSPC                       12              /**< 存储、设备或资源已无剩余空间。 */
#define RT_EPERM                        13              /**< 调用者无权执行该操作。 */
#define RT_ETRAP                        14              /**< RT-Thread 陷阱或异常事件。 */
#define RT_EFAULT                       15              /**< 所提供的地址无法按要求访问。 */
#define RT_ENOBUFS                      16              /**< 所需的网络或系统缓冲区不可用。 */
#define RT_ESCHEDISR                    17              /**< 调用者在 ISR 中，立即切换被延后。 */
#define RT_ESCHEDLOCKED                 18              /**< 嵌套调度器临界区使立即切换被延后。 */
/** @} */
#endif /* defined(RT_USING_LIBC) && !defined(RT_USING_NANO) */

/**
 * @brief 返回当前执行上下文对应的 errno 值。
 *
 * 在线程上下文中，此函数读取当前线程 TCB 的错误字段。在中断期间，或当前线程尚未
 * 存在时，它读取共享的全局后备槽。函数不会清除数值，也不会统一其正负号。
 *
* @return 选中存储槽转换后的 rt_err_t 值。线程存储为 rt_err_t；共享后备槽为 int 宽度。
 */
rt_err_t rt_get_errno(void);

/**
 * @brief 为当前执行上下文保存 errno 值。
 *
 * 在普通线程上下文中，只会修改调用线程的错误字段。中断和早期启动调用会更新共享的
 * int 宽度后备槽。无论正的 POSIX 风格 errno 值，还是负的 RT-Thread 风格值，都会
 * 原样接受而不转换符号。在 rt_err_t 宽于 int 的目标上，超出 int 范围的值在后备上下文
 * 中会被截窄。
 *
 * @param no 要保存的错误值；不会转换符号或检查范围。
 */
void rt_set_errno(rt_err_t no);

/**
 * @brief 返回当前上下文 errno 的、可作为左值使用的地址。
 *
 * 这是兼容 `errno` 宏所使用的访问器。返回的地址按 rt_get_errno() 相同的规则，选择
 * 当前线程字段或中断/早期启动全局槽。该函数的 ABI 是 `int *`；在 64 位构建中，它会
 * 将更宽的 rt_err_t 线程字段转换为 `int *`，因此通过 errno 左值写入时只会更新该字段
 * 中 int 大小的一部分。这是实现的位宽限制；需要完整 rt_err_t API 时应优先使用
 * rt_set_errno()/rt_get_errno()。不得把返回地址交给其他上下文，也不得在线程结束后
 * 继续保存其字段地址。
 *
 * @return指向当前 errno 存储的指针。  该指针属于
 *         内核并且不能被释放。
 */
int *_rt_errno(void);

/**
 * @brief将已知的 RT-Thread 错误值转换为静态诊断字符串。
 *
 * 正面和负面形式的处理方式相同。  仅紧凑表
* 识别klibc实现的核心RT-Thread错误；其他 POSIX 或
 * 特定于组件的值会产生实现的未知错误
 * 细绳。  这是诊断标签，而不是本地化的 `strerror()` 消息。
 *
 * @param error正或负 RT-螺纹错误代码。
 * @return指向不可变静态存储的指针。  调用者不得修改或
 *         释放它，并且不需要调用者提供的缓冲区。
 */
const char *rt_strerror(rt_err_t error);

/*
 * Newlib 和 Windows 版本抑制了此兼容性宏。  RT-线程的
 * Newlibglue仍然可以通过转发到来实现Newlib的`__errno()`
 * _rt_errno(); suppression here merely avoids replacing Newlib's public macro.
 * 对于其他配置，仅当前一个标头未定义 errno 时才定义 errno
 * 已经定义了它。  扩展是一个左值：
* 每次都是 `errno = value` 并通过 _rt_errno() 读取调度，这
 * 就是在调度过程中保留每个线程的选择。
 */
#if !defined(RT_USING_NEWLIBC) && !defined(_WIN32)
#ifndef errno
#define errno    *_rt_errno()
#endif
#endif /* !defined(RT_USING_NEWLIBC) && !defined(_WIN32) */

#ifdef __cplusplus
}
#endif

#endif
