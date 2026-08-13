/*
 * Copyright (c) 2006-2024, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 修改记录：
 * 日期           作者         说明
 * 2024-03-10     Meco Man     首个版本
 */

/**
 * @file kstdio.c
 * @brief RT-Thread 格式化输入/输出的轻量包装层。
 *
 * 本文件只负责构造和结束 `va_list`，或在选择 libc 后端时把 rt_* 名称转发到
 * libc。真正的内置 printf/scanf 解析器分别位于 rt_vsnprintf_*.c 和
 * rt_vsscanf.c。所有接口都只操作调用者缓冲区，不直接访问控制台设备。
 */

#include <rtthread.h>
#if defined(RT_KLIBC_USING_LIBC_VSSCANF) || \
    defined(RT_KLIBC_USING_LIBC_VSNPRINTF)
#include <stdio.h>
#endif

/**
 * @brief 按长度限制把可变参数格式化到缓冲区。
 *
 * @param buf 目标缓冲区。
 *
 * @param size 缓冲区总容量，包含结尾 NUL；0 可用于只计算长度。
 *
 * @param fmt printf 格式串，后续参数类型必须匹配。
 *
 * @return 转发 rt_vsnprintf() 的返回值，通常是缓冲区足够大时本应生成的字符数。
 */
int rt_snprintf(char *buf, size_t size, const char *fmt, ...)
{
    int n = 0;
    va_list args;

    va_start(args, fmt);
    n = rt_vsnprintf(buf, size, fmt, args);
    va_end(args);

    return n;
}
RTM_EXPORT(rt_snprintf);

/**
 * @brief 使用现有 va_list 执行不受目标容量保护的格式化。
 *
 * @param buf 必须足够大的目标缓冲区。
 *
 * @param format printf 格式串。
 *
 * @param arg_ptr 与格式串匹配的参数列表。
 *
 * @return rt_vsnprintf() 报告的结果长度。
 *
 * 函数把容量伪装成 `(size_t)-1`，因此无法阻止写越界，仅应在调用者已精确
 * 保证容量时使用。
 */
int rt_vsprintf(char *buf, const char *format, va_list arg_ptr)
{
    return rt_vsnprintf(buf, (size_t) - 1, format, arg_ptr);
}
RTM_EXPORT(rt_vsprintf);

/**
 * @brief 可变参数版本的不受限字符串格式化。
 *
 * @param buf 足够大的目标缓冲区。
 *
 * @param format printf 格式串。
 *
 * @return rt_vsprintf() 返回值。
 */
int rt_sprintf(char *buf, const char *format, ...)
{
    int n = 0;
    va_list arg_ptr;

    va_start(arg_ptr, format);
    n = rt_vsprintf(buf, format, arg_ptr);
    va_end(arg_ptr);

    return n;
}
RTM_EXPORT(rt_sprintf);

#ifdef RT_KLIBC_USING_LIBC_VSNPRINTF
/** 当配置选择 libc 后端时，保持 RT-Thread 命名空间并直接转发标准函数。 */
int rt_vsnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
    return vsnprintf(buf, size, fmt, args);
}
#endif /* RT_KLIBC_USING_LIBC_VSNPRINTF */
RTM_EXPORT(rt_vsnprintf);

#ifdef RT_KLIBC_USING_LIBC_VSSCANF
/** 当配置选择 libc 扫描器时，直接转发标准 vsscanf。 */
int rt_vsscanf(const char *buffer, const char *format, va_list ap)
{
    return vsscanf(buffer, format, ap);
}
#endif /* RT_KLIBC_USING_LIBC_VSSCANF */
RTM_EXPORT(rt_vsscanf);

/**
 * @brief 按格式串从输入字符串解析多个字段。
 *
 * @param str NUL 结尾输入文本。
 *
 * @param format scanf 格式串；目标指针类型和容量必须匹配转换说明。
 *
 * @return 转发 rt_vsscanf() 的成功赋值数或后端失败值。
 */
int rt_sscanf(const char *str, const char *format, ...)
{
    va_list ap;
    int rv;

    va_start(ap, format);
    rv = rt_vsscanf(str, format, ap);
    va_end(ap);

    return rv;
}
RTM_EXPORT(rt_sscanf);
