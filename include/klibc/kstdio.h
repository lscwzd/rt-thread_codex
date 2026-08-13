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
 * @file kstdio.h
 * @brief 供内核代码使用的面向缓冲区的格式化输出和输入接口。
 *
 * 此接口在 RT-Thread 的 `rt_` 命名空间下提供与 sprintf/snprintf/sscanf 系列相似的
 * 功能。它不会进行流或控制台 I/O：输出写入调用者拥有的内存，输入则从调用者提供的
 * NUL 结尾字符串中解析。
 *
 * 具体实现由 klibc 配置控制。输出可使用紧凑的 RT-Thread 格式化器、较完整的标准
 * 格式化器或 libc `vsnprintf`。各选项并非彼此独立：long-long 支持是非 libc 选项，
 * 由标准格式化器选择；浮点、`%n` 和 MSVC 风格控制属于标准格式化器子菜单；tiny
 * 格式化器拥有 `%b` 等固定扩展；libc 后端则遵循所选库的行为。扫描可使用 RT-Thread
 * 解析器或 libc `vsscanf`。可移植代码只能使用其所支持的每种目标配置都具备的转换子集。
 *
 * 格式字符串是受信任的类型约定。每个转换说明符必须与提升后的可变参数类型匹配，
 * 每个扫描目标也必须具有规定的指针类型和足够容量。不匹配属于未定义行为，这些例程
 * 无法可靠检测。
 */

#ifndef __RT_KSTDIO_H__
#define __RT_KSTDIO_H__

/* RT-Thread 标量类型包含 size_t；stdarg.h 定义 va_list 的处理方式。 */
#include <rttypes.h>
#include <stdarg.h>

#ifdef __cplusplus
/* 让 C++ 内核或组件代码发出的调用保持 C ABI。 */
extern "C" {
#endif

/**
 * @brief 将可变参数列表格式化到容量不受此接口限制的目标缓冲区。
 *
 * 这是 rt_sprintf() 的 `va_list` 形式。它等同于认为输出容量无限，因此函数无法防止
 * 目标缓冲区溢出。调用者必须预先计算或以其他方式确保能容纳全部格式化字符和结尾 NUL 字节。
 *
 * 函数会从 @p arg_ptr 中取用参数。若调用者需要再次使用该列表，必须传入一个 `va_copy`
 * 得到的副本，并按平台 ABI 要求随后对副本调用 `va_end`。
 *
 * @param dest 容量足够的可写目标缓冲区；不得与通过 @p format 或格式化参数读取的存储区重叠。
 * @param format 所选后端支持的 NUL 结尾格式字符串。
 * @param arg_ptr 参数列表，其中提升后的类型必须与 @p format 匹配。
* @return 不含结尾 NUL 的格式化字符数；若所选后端报告编码或格式错误，则返回负值。
 */
int rt_vsprintf(char *dest, const char *format, va_list arg_ptr);

/**
 * @brief 将可变参数列表格式化到具有容量上限的缓冲区。
 *
 * 当 @p size 非零时，最多保存 `size - 1` 个格式化字符，随后写入 NUL 终止符。容量为零
 * 时不会保存任何内容，也不会写入终止符。为兼容不同后端，即使进行零容量长度计算也最好
 * 传入有效的 @p buf 指针；非零容量始终要求至少 @p size 字节的可写存储区。
 *
 * 非负返回值表示若不截断本应产生的完整字符数，不包括 NUL。因此应先确认返回值非负，
 * 再用 `(size_t)return_value >= size` 判断是否发生截断。函数绝不会分配输出缓冲区。
 *
* @param buf 目标缓冲区；当大小非零时，可写 @p 大小的字节。
 * @param size总目的地容量，包括最终 NUL 的空间。
 * @param fmt所选后端支持的以 NUL 结尾的格式字符串。
* @param args 匹配 @p fmt 的参数列表；它被这个调用消耗了。
 * @return所需的格式化长度（不包括 NUL）或负后端错误。
 */
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

/**
 * @brief将可变参数格式化为无界目标缓冲区。
 *
 * 这个方便的包装器创建一个 `va_list` 并委托给
 * rt_vsprintf()。  它与 sprintf 具有相同的溢出风险：@p buf 必须保存
 * 完整结果并终止 NUL。  每当以下情况时首选 rt_snprintf()
 * 目的地容量已知。
 *
 * @param buf可写目的地，具有足够的容量来获取完整的结果。
 * @param formatNUL 终止的格式字符串。
 * @param...其提升类型与转换完全匹配的值。
 * @return写入的字符数（不包括 NUL）或负后端错误。
 */
int rt_sprintf(char *buf, const char *format, ...);

/**
 * @brief将可变参数格式化为大小有限的目标缓冲区。
 *
 * 这个方便的包装器委托给 rt_vsnprintf()。  当 @p 尺寸为
 * 非零，结果始终是 NUL-由选定的一致终止
 * 后端，包括发生截断时。  返回值描述了
 * 未截断的结果，而不仅仅是存储的字节。
 *
* @param buf 目标缓冲区；当大小非零时，可写 @p 大小的字节。
 * @param size总容量包括终止 NUL。
 * @param formatNUL 终止的格式字符串。
 * @param...其提升类型与转换完全匹配的值。
 * @return所需的格式化长度（不包括 NUL）或负后端错误。
 */
int rt_snprintf(char *buf, size_t size, const char *format, ...);

/**
 * @brief使用现有参数列表从字符串中解析格式化字段。
 *
* 输入仅从 @p 缓冲区消耗；没有发生设备或控制台读取。
 * 空白和转换行为遵循所选的 RT-Thread/libc
 * 扫描器。  @p ap 中的目标指针必须与转换类型匹配。
 * 对于 `%s`、`%c` 和 `%[` 转换，请使用从以下公式导出的字段宽度：
* 目的地容量；省略/过多的宽度可能会超出缓冲区。
 * `%n`，当支持时，写入一个计数并且本身不会增加
 * 赋值结果。
 *
 * RT-Thread扫描器后端获取临时的256字节字符类
* 表从系统堆中取出并在返回前释放；未能
 * 分配该内部表报告为-1。  因此这个后端
 * 需要 RT_USING_HEAP，其可用的执行上下文受到以下限制
 * 选择堆的锁定和 RT_USING_HEAP_ISR 策略。  无堆构建必须
 * 选择 libc 扫描器或提供其他兼容的实现。  这
 * libc 后端遵循自己的分配和区域设置规则。  无返回输入
 * 子字符串归该函数所有。
 *
 * @param buffer以 NUL 结尾的输入字符串对于调用仍然可读。
 * @param formatNUL 终止扫描格式。
* @param ap 与 @p 格式匹配的目标指针参数列表；消耗于
 *        这个调用，所以如果必须重用的话先复制它。
 * @return后端报告的分配计数或失败。  内置扫描仪
 *         对于某些文字匹配失败返回零，但对于以下情况返回 -1
 *         分配失败，第一次转换前输入耗尽，
 *         以及几次首次转换解析失败。  libc 后端如下
 *         libc 的 vsscanf 合约。
 */
int rt_vsscanf(const char *buffer, const char *format, va_list ap);

/**
 * @brief将字符串中的格式化字段解析为可变目标。
 *
 * 该包装器构造一个 `va_list` 并委托给 rt_vsscanf()。  它有
 * 相同的目标类型、字段宽度、堆后端和返回契约。
 * 输入字符串不会被修改。
 *
 * @param strNUL 终止的输入字符串。
 * @param formatNUL 终止扫描格式。
 * @param...与每个未抑制的转换匹配的可写目标指针。
* @return 分配计数或后端特定的失败值；看
 *         rt_vsscanf() 用于内置扫描仪的零/-1区分。
 */
int rt_sscanf(const char *str, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif
